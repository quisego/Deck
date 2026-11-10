/*
*	Link (STM32 version) = a message channel over UART between STM32 and ESP32.
*	SEND: Link_Send() copies the message to a free slot -> TX task makes a packet -> UART DMA sends it.
*	RECEIVE: UART DMA fills a ring buffer -> RX task reads it -> parser finds packets
*	-> callbacks of subscribers are called.
*/

#include "link.h"
#include <string.h>

/*
*	RX buffer is a ring (circular) buffer. DMA writes into it all the time.
*/
#define RX_SIZE LINK_RX_BUFFER_SIZE
#define RX_HALF (RX_SIZE / 2U)

/*
*	States of one transmit operation.
*/
enum
{
	TX_IDLE = 0,
	TX_PENDING,
	TX_DONE,
	TX_FAILED
};

/*
*	One TX slot: id, length, and then the payload bytes (flexible array).
*/
typedef struct
{
	uint16_t id;
	uint16_t length;
	uint8_t payload[];
} LinkTxSlot;

#define TX_SLOT_HEADER_SIZE ((uint16_t)sizeof(LinkTxSlot))

/*
*	All Link objects. Interrupt callbacks use it to find the Link for a UART.
*/
static Link *s_ports[LINK_MAX_INSTANCES] = {0};

static Link *find_port(const UART_HandleTypeDef *huart)
{
	for (uint32_t i = 0; i < LINK_MAX_INSTANCES; i++)
	{
		if ((s_ports[i] != NULL) && (s_ports[i]->huart == huart))
		{
			return s_ports[i];
		}
	}
	return NULL;
}

/*
*	Wake a task from an interrupt.
*/
static void notify_from_isr(TaskHandle_t task)
{
	if (task != NULL)
	{
		BaseType_t woken = pdFALSE;
		vTaskNotifyGiveFromISR(task, &woken);
		portYIELD_FROM_ISR(woken);
	}
}

/*
*	Atomic counter +1. It is safe in interrupts and in tasks.
*/
static void count(volatile uint32_t *counter)
{
	(void)__atomic_add_fetch(counter, 1U, __ATOMIC_RELAXED);
}

/*
*	Mutex helpers. We use the mutex to protect statistics and the subscriber list.
*/
static void lock(Link *link) { (void)xSemaphoreTake(link->lock, portMAX_DELAY); }
static void unlock(Link *link) { (void)xSemaphoreGive(link->lock); }

static void stat_add(Link *link, uint32_t *counter, uint32_t value)
{
	if (value == 0U) return;
	lock(link);
	*counter += value;
	unlock(link);
}

/*
*	Get the address of slot number "index" in the pool.
*/
static LinkTxSlot *tx_slot(Link *link, uint16_t index)
{
	return (LinkTxSlot *)(void *)(link->tx_pool + ((size_t)index * link->tx_slot_size));
}

/*
*	Start circular DMA receive. The IDLE interrupt fires when the line is quiet for a moment
*	(this means "a packet probably ended").
*/
static HAL_StatusTypeDef rx_start(Link *p)
{
	p->rx_read_total = 0U;
	p->rx_halves = 0U;
	const HAL_StatusTypeDef status = HAL_UART_Receive_DMA(p->huart, p->rx_buf, (uint16_t)RX_SIZE);
	if (status == HAL_OK)
	{
		__HAL_UART_CLEAR_IDLEFLAG(p->huart);
		__HAL_UART_ENABLE_IT(p->huart, UART_IT_IDLE);
	}
	return status;
}

/*
*	Copy new received bytes from the ring buffer to "dst". Returns how many bytes we copied.
*	DMA does not tell us how many laps it made, so we count half-buffer interrupts (rx_halves).
*/
static uint16_t rx_take(Link *p, uint8_t *dst, uint16_t max_len)
{
	// Read the lap counter and the DMA position. Repeat if an interrupt changed the counter meanwhile.
	uint32_t halves, pos;
	do
	{
		halves = p->rx_halves;
		const uint32_t ndtr = __HAL_DMA_GET_COUNTER(p->huart->hdmarx);
		pos = (RX_SIZE - ndtr) % RX_SIZE;
	} while (halves != p->rx_halves);

	// Fix the lap counter if the interrupt for a half is late.
	const uint32_t in_second_half = (pos >= RX_HALF) ? 1U : 0U;
	if ((halves & 1U) != in_second_half) halves++;

	// Total bytes written by DMA since the start = laps * size + position.
	const uint32_t written = ((halves >> 1) * RX_SIZE) + pos;
	const uint32_t pending = written - p->rx_read_total;

	// More unread bytes than the buffer size: DMA already overwrote old data. We were too slow.
	// Count the error and skip the lost data.
	if (pending > RX_SIZE)
	{
		count(&p->rx_overflow);
		p->rx_read_total = written;
		return 0U;
	}

	uint32_t n = pending;
	if (n > max_len) n = max_len;
	if (n == 0U) return 0U;

	// Copy in two parts if the data goes around the end of the ring buffer.
	const uint32_t index = p->rx_read_total % RX_SIZE;
	const uint32_t first = ((RX_SIZE - index) < n) ? (RX_SIZE - index) : n;
	memcpy(dst, &p->rx_buf[index], first);
	if (n > first) memcpy(dst + first, &p->rx_buf[0], n - first);
	p->rx_read_total += n;
	return (uint16_t)n;
}

static void link_tx_task(void *arg);
static void link_rx_task(void *arg);

/*
*	Start the link. Steps: check input, allocate memory, create queues, start DMA, create tasks.
*/
LinkStatus Link_Init(Link *link, const LinkConfig *config)
{
	if ((link == NULL) || (config == NULL) || (config->huart == NULL)) return LINK_INVALID_ARG;
	// UART needs a TX DMA and an RX DMA in circular mode.
	if (config->huart->hdmatx == NULL || config->huart->hdmarx == NULL || config->huart->hdmarx->Init.Mode != DMA_CIRCULAR)
	{
		return LINK_INVALID_ARG;
	}

	memset(link, 0, sizeof(*link));
	link->config = *config;
	link->huart = config->huart;

	if (Protocol_ParserInit(&link->parser, config->max_payload) != PROTOCOL_OK) return LINK_ERROR;

	// Slot size is rounded up to a multiple of 4 bytes (memory alignment).
	link->tx_slot_size = (uint16_t)((TX_SLOT_HEADER_SIZE + config->max_payload + 3U) & ~3U);
	link->tx_pool = (uint8_t *)pvPortMalloc((size_t)link->tx_slot_size * config->tx_queue_length);
	link->tx_free_queue = xQueueCreate(config->tx_queue_length, sizeof(uint16_t));
	link->tx_ready_queue = xQueueCreate(config->tx_queue_length, sizeof(uint16_t));
	link->lock = xSemaphoreCreateMutex();

	if (!link->tx_pool || !link->tx_free_queue || !link->tx_ready_queue || !link->lock)
	{
		if (link->tx_pool) vPortFree(link->tx_pool);
		if (link->tx_free_queue) vQueueDelete(link->tx_free_queue);
		if (link->tx_ready_queue) vQueueDelete(link->tx_ready_queue);
		if (link->lock) vSemaphoreDelete(link->lock);
		return LINK_ERROR;
	}

	// At the start all slots are free: put all indexes in the "free" queue.
	for (uint16_t i = 0; i < config->tx_queue_length; i++)
	{
		xQueueSend(link->tx_free_queue, &i, 0);
	}

	// Find a free place in s_ports. Reject the same UART used twice.
	uint32_t slot_idx = LINK_MAX_INSTANCES;
	for (uint32_t i = 0; i < LINK_MAX_INSTANCES; i++)
	{
		if ((s_ports[i] != NULL) && (s_ports[i]->huart == config->huart))
		{
			return LINK_INVALID_ARG;
		}
		if ((s_ports[i] == NULL) && (slot_idx == LINK_MAX_INSTANCES))
		{
			slot_idx = i;
		}
	}
	if (slot_idx == LINK_MAX_INSTANCES)
	{
		return LINK_ERROR;
	}

	s_ports[slot_idx] = link;

	if (rx_start(link) != HAL_OK)
	{
		s_ports[slot_idx] = NULL;
		return LINK_ERROR;
	}

	link->initialized = 1U;

	if (xTaskCreate(link_rx_task, "LinkRx", config->rx_task_stack_words, link, config->rx_task_priority, &link->rx_task) != pdPASS ||
	    xTaskCreate(link_tx_task, "LinkTx", config->tx_task_stack_words, link, config->tx_task_priority, &link->tx_task) != pdPASS)
	{
		link->initialized = 0U;
		s_ports[slot_idx] = NULL;
		return LINK_ERROR;
	}

	return LINK_OK;
}

/*
*	Send a message. Copy it into a free slot and give the slot to the TX task.
*	queue_wait_ticks = how long we wait for a free slot (0 = do not wait).
*/
LinkStatus Link_Send(Link *link, uint16_t message_id, const void *payload, uint16_t length, TickType_t queue_wait_ticks)
{
	if (!link || link->initialized == 0U) return LINK_NOT_INITIALIZED;
	if ((!payload && length > 0U) || length > link->config.max_payload) return LINK_INVALID_ARG;

	uint16_t index;
	if (xQueueReceive(link->tx_free_queue, &index, queue_wait_ticks) != pdPASS)
	{
		stat_add(link, &link->stats.tx_queue_full, 1U);
		return (queue_wait_ticks == 0U) ? LINK_QUEUE_FULL : LINK_TIMEOUT;
	}

	LinkTxSlot *slot = tx_slot(link, index);
	slot->id = message_id;
	slot->length = length;
	if (length > 0U) memcpy(slot->payload, payload, length);

	xQueueSend(link->tx_ready_queue, &index, 0);
	return LINK_OK;
}

/*
*	TX task: take a slot, make a packet, send it with DMA, wait until it is done.
*/
static void link_tx_task(void *arg)
{
	Link *link = (Link *)arg;
	for (;;)
	{
		uint16_t index;
		if (xQueueReceive(link->tx_ready_queue, &index, portMAX_DELAY) != pdPASS) continue;

		LinkTxSlot *slot = tx_slot(link, index);
		size_t packet_length = 0U;
		ProtocolStatus encoded = Protocol_Encode(
			link->tx_packet, sizeof(link->tx_packet), &packet_length,
			slot->id, link->tx_seq, slot->payload, slot->length);

		// The packet is in tx_packet now, so the slot is free again.
		xQueueSend(link->tx_free_queue, &index, 0);

		if (encoded != PROTOCOL_OK)
		{
			stat_add(link, &link->stats.tx_errors, 1U);
			continue;
		}
		link->tx_seq++;

		// Prepare to wait: the TX complete interrupt will wake this task.
		link->tx_waiter = xTaskGetCurrentTaskHandle();
		link->tx_state = TX_PENDING;
		(void)ulTaskNotifyTake(pdTRUE, 0);

		if (HAL_UART_Transmit_DMA(link->huart, link->tx_packet, (uint16_t)packet_length) != HAL_OK)
		{
			link->tx_state = TX_IDLE;
			stat_add(link, &link->stats.tx_errors, 1U);
			continue;
		}

		// Wait for DMA to finish. We also stop on timeout so we never wait forever.
		const TickType_t start = xTaskGetTickCount();
		while (link->tx_state == TX_PENDING)
		{
			TickType_t elapsed = xTaskGetTickCount() - start;
			if (elapsed >= link->config.tx_timeout_ticks) break;
			ulTaskNotifyTake(pdTRUE, link->config.tx_timeout_ticks - elapsed);
		}

		// Still pending = timeout. Abort the transfer and count it.
		if (link->tx_state == TX_PENDING)
		{
			HAL_UART_AbortTransmit(link->huart);
			link->tx_state = TX_IDLE;
			stat_add(link, &link->stats.tx_timeouts, 1U);
		}
		else if (link->tx_state == TX_DONE)
		{
			stat_add(link, &link->stats.tx_messages, 1U);
		}
		else
		{
			stat_add(link, &link->stats.tx_errors, 1U);
		}
		link->tx_state = TX_IDLE;
	}
}

/*
*	A full message arrived. Copy the matching subscribers while the mutex is locked.
*/
static void link_dispatch(Link *link, const ProtocolMessage *pm)
{
	uint32_t count = 0U;
	lock(link);
	for (uint32_t i = 0; i < LINK_MAX_SUBSCRIBERS; i++)
	{
		if (link->subscribers[i].in_use && link->subscribers[i].message_id == pm->id)
		{
			link->rx_snapshot[count++] = link->subscribers[i];
		}
	}
	link->stats.rx_messages++;
	if (count == 0U) link->stats.rx_unknown_id++;
	unlock(link);

	const LinkMessage message = { .id = pm->id, .length = pm->length, .payload = pm->payload };
	for (uint32_t i = 0; i < count; i++)
	{
		// Call the callbacks AFTER we unlock, so a callback can use Link functions without deadlock.
		link->rx_snapshot[i].callback(&message, link->rx_snapshot[i].context);
	}
}

/*
*	RX task: read bytes, give them to the parser, and handle every result.
*/
static void link_rx_task(void *arg)
{
	Link *link = (Link *)arg;
	for (;;)
	{
		link->rx_waiter = xTaskGetCurrentTaskHandle();
		uint16_t n = rx_take(link, link->rx_chunk, sizeof(link->rx_chunk));
		if (n == 0U)
		{
			// After a UART error, restart DMA receive.
			if (link->rx_restart_pending)
			{
				HAL_UART_AbortReceive(link->huart);
				rx_start(link);
				link->rx_restart_pending = 0;
			}
			// Sleep until an interrupt wakes us, or until the poll time ends.
			ulTaskNotifyTake(pdTRUE, link->config.rx_poll_ticks);
			n = rx_take(link, link->rx_chunk, sizeof(link->rx_chunk));
		}

		if (n > 0U)
		{
			size_t offset = 0U;
			ProtocolMessage msg;
			ProtocolParseResult res;
			// Handle each result: message -> dispatch, error -> count it in statistics.
			while ((res = Protocol_ParserProcess(&link->parser, link->rx_chunk, n, &offset, &msg)) != PROTOCOL_PARSE_NONE)
			{
				if (res == PROTOCOL_PARSE_MESSAGE) link_dispatch(link, &msg);
				else if (res == PROTOCOL_PARSE_BAD_SOF) stat_add(link, &link->stats.rx_invalid_sof, 1U);
				else if (res == PROTOCOL_PARSE_BAD_VERSION) stat_add(link, &link->stats.rx_invalid_version, 1U);
				else if (res == PROTOCOL_PARSE_BAD_LENGTH) stat_add(link, &link->stats.rx_invalid_length, 1U);
				else if (res == PROTOCOL_PARSE_BAD_CRC) stat_add(link, &link->stats.rx_crc_errors, 1U);
			}
		}
	}
}

/*
*	Add a callback for one message id. The same callback cannot be added twice.
*/
LinkStatus Link_Subscribe(Link *link, uint16_t message_id, Link_Callback callback, void *context)
{
	if (!link || !callback) return LINK_INVALID_ARG;
	if (!link->initialized) return LINK_NOT_INITIALIZED;
	LinkStatus res = LINK_NO_SPACE;
	LinkSubscriber *free_entry = NULL;
	lock(link);
	for (uint32_t i = 0; i < LINK_MAX_SUBSCRIBERS; i++)
	{
		if (!link->subscribers[i].in_use && !free_entry) free_entry = &link->subscribers[i];
		else if (link->subscribers[i].in_use && link->subscribers[i].message_id == message_id && link->subscribers[i].callback == callback && link->subscribers[i].context == context)
		{
			res = LINK_ALREADY_EXISTS;
			free_entry = NULL;
			break;
		}
	}
	if (free_entry)
	{
		free_entry->message_id = message_id;
		free_entry->callback = callback;
		free_entry->context = context;
		free_entry->in_use = 1U;
		res = LINK_OK;
	}
	unlock(link);
	return res;
}

/*
*	Remove a callback.
*/
LinkStatus Link_Unsubscribe(Link *link, uint16_t message_id, Link_Callback callback, void *context)
{
	if (!link || !callback) return LINK_INVALID_ARG;
	if (!link->initialized) return LINK_NOT_INITIALIZED;
	LinkStatus res = LINK_NOT_FOUND;
	lock(link);
	for (uint32_t i = 0; i < LINK_MAX_SUBSCRIBERS; i++)
	{
		LinkSubscriber *s = &link->subscribers[i];
		if (s->in_use && s->message_id == message_id && s->callback == callback && s->context == context)
		{
			memset(s, 0, sizeof(*s));
			res = LINK_OK;
			break;
		}
	}
	unlock(link);
	return res;
}

/*
*	Get a copy of the statistics.
*/
LinkStatus Link_GetStats(Link *link, LinkStats *out)
{
	if (!link || !out) return LINK_INVALID_ARG;
	if (!link->initialized) return LINK_NOT_INITIALIZED;
	lock(link);
	*out = link->stats;
	unlock(link);
	return LINK_OK;
}

LinkStatus Link_ResetStats(Link *link)
{
	if (!link) return LINK_INVALID_ARG;
	if (!link->initialized) return LINK_NOT_INITIALIZED;
	lock(link);
	memset(&link->stats, 0, sizeof(link->stats));
	unlock(link);
	return LINK_OK;
}

/*
*	Call this from the UART interrupt. IDLE flag = the line was quiet = wake the RX task.
*/
void Link_UART_IRQHandler(UART_HandleTypeDef *huart)
{
	Link *p = find_port(huart);
	if (!p) return;
	if (__HAL_UART_GET_FLAG(huart, UART_FLAG_IDLE) != RESET)
	{
		if (__HAL_UART_GET_FLAG(huart, UART_FLAG_ORE) != RESET) count(&p->rx_overflow);
		__HAL_UART_CLEAR_IDLEFLAG(huart);
		notify_from_isr(p->rx_waiter);
	}
}

/*
*	TX DMA finished OK: tell the TX task.
*/
void Link_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
	Link *p = find_port(huart);
	if (p && p->tx_state == TX_PENDING)
	{
		p->tx_state = TX_DONE;
		notify_from_isr(p->tx_waiter);
	}
}

/*
*	Count one half of the ring buffer and wake the RX task.
*/
void Link_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart)
{
	Link *p = find_port(huart);
	if (p) { p->rx_halves++; notify_from_isr(p->rx_waiter); }
}

void Link_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	Link *p = find_port(huart);
	if (p) { p->rx_halves++; notify_from_isr(p->rx_waiter); }
}

/*
*	UART error: count it, ask the RX task to restart DMA, and wake the TX task if it waits.
*/
void Link_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	Link *p = find_port(huart);
	if (!p) return;
	count(&p->uart_errors);
	p->rx_restart_pending = 1U;
	if (p->tx_state == TX_PENDING)
	{
		p->tx_state = TX_FAILED;
		notify_from_isr(p->tx_waiter);
	}
	notify_from_isr(p->rx_waiter);
}
