/*
*	Link (ESP32 version). Same idea and same API as the STM32 link.c,
*	but here we use the ESP-IDF UART driver. The driver does buffering for us,
*	so we do not need DMA code or a ring buffer.
*/

#include "link.h"
#include <string.h>

/*
*	One TX slot: id, length, then the payload bytes.
*/
typedef struct
{
	uint16_t id;
	uint16_t length;
	uint8_t payload[];
} LinkTxSlot;

#define TX_SLOT_HEADER_SIZE ((uint16_t)sizeof(LinkTxSlot))

/*
*	Mutex helpers for statistics and the subscriber list.
*/
static void link_tx_task(void *arg);
static void link_rx_task(void *arg);

static void lock(Link *link) { (void)xSemaphoreTake(link->lock, portMAX_DELAY); }
static void unlock(Link *link) { (void)xSemaphoreGive(link->lock); }

static void stat_add(Link *link, uint32_t *counter, uint32_t value)
{
	if (value == 0U) return;
	lock(link);
	*counter += value;
	unlock(link);
}

static LinkTxSlot *tx_slot(Link *link, uint16_t index)
{
	return (LinkTxSlot *)(void *)(link->tx_pool + ((size_t)index * link->tx_slot_size));
}

/*
*	Start the link: set up the UART, allocate memory, create queues and tasks.
*/
LinkStatus Link_Init(Link *link, const LinkConfig *config)
{
	if ((link == NULL) || (config == NULL)) return LINK_INVALID_ARG;

	memset(link, 0, sizeof(*link));
	link->config = *config;

	if (Protocol_ParserInit(&link->parser, config->max_payload) != PROTOCOL_OK) return LINK_ERROR;

	// UART settings: 8 data bits, no parity, 1 stop bit, no flow control.
	uart_config_t uart_config = {
		.baud_rate = config->baud_rate > 0 ? config->baud_rate : 115200,
		.data_bits = UART_DATA_8_BITS,
		.parity    = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	if (uart_param_config(config->uart_num, &uart_config) != ESP_OK) return LINK_ERROR;
	if (uart_set_pin(config->uart_num, config->tx_pin, config->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return LINK_ERROR;
	// Install the driver. It has its own RX and TX buffers.
	if (uart_driver_install(config->uart_num, config->rx_buffer_size > 0 ? config->rx_buffer_size : 1024, config->tx_buffer_size > 0 ? config->tx_buffer_size : 1024, 0, NULL, 0) != ESP_OK)
	{
		return LINK_ERROR;
	}

	// Slot size is rounded up to a multiple of 4 bytes.
	link->tx_slot_size = (uint16_t)((TX_SLOT_HEADER_SIZE + config->max_payload + 3U) & ~3U);
	link->tx_pool = (uint8_t *)pvPortMalloc((size_t)link->tx_slot_size * config->tx_queue_length);
	link->tx_free_queue = xQueueCreate(config->tx_queue_length, sizeof(uint16_t));
	link->tx_ready_queue = xQueueCreate(config->tx_queue_length, sizeof(uint16_t));
	link->lock = xSemaphoreCreateMutex();

	if (!link->tx_pool || !link->tx_free_queue || !link->tx_ready_queue || !link->lock)
	{
		uart_driver_delete(config->uart_num);
		return LINK_ERROR;
	}

	// At the start all slots are free.
	for (uint16_t i = 0; i < config->tx_queue_length; i++)
	{
		xQueueSend(link->tx_free_queue, &i, 0);
	}

	link->initialized = 1U;

	if (xTaskCreate(link_rx_task, "LinkRx", config->rx_task_stack_words > 0 ? config->rx_task_stack_words : 2048, link, config->rx_task_priority, &link->rx_task) != pdPASS ||
	    xTaskCreate(link_tx_task, "LinkTx", config->tx_task_stack_words > 0 ? config->tx_task_stack_words : 2048, link, config->tx_task_priority, &link->tx_task) != pdPASS)
	{
		link->initialized = 0U;
		uart_driver_delete(config->uart_num);
		return LINK_ERROR;
	}

	return LINK_OK;
}

/*
*	Copy the message into a free slot and give it to the TX task.
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
*	TX task: make a packet from a slot and write it to the UART.
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

		xQueueSend(link->tx_free_queue, &index, 0);

		if (encoded != PROTOCOL_OK)
		{
			stat_add(link, &link->stats.tx_errors, 1U);
			continue;
		}
		link->tx_seq++;

		// If not all bytes were accepted, count an error.
		int written = uart_write_bytes(link->config.uart_num, link->tx_packet, packet_length);
		if (written < 0 || (size_t)written != packet_length)
		{
			stat_add(link, &link->stats.tx_errors, 1U);
		}
		else
		{
			stat_add(link, &link->stats.tx_messages, 1U);
		}
	}
}

/*
*	Find subscribers for this message id and call them.
*	They are copied under the mutex, but called AFTER unlock.
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
		link->rx_snapshot[i].callback(&message, link->rx_snapshot[i].context);
	}
}

/*
*	RX task: read bytes from the UART, give them to the parser, handle each result.
*/
static void link_rx_task(void *arg)
{
	Link *link = (Link *)arg;
	uint8_t rx_chunk[64];

	// Wait for data (up to the timeout). Then feed the parser.
	TickType_t ticks = link->config.rx_poll_ticks;
	uint32_t timeout_ms = (ticks == portMAX_DELAY) ? portMAX_DELAY : (ticks * portTICK_PERIOD_MS);
	if (timeout_ms == 0) {
		timeout_ms = 1;
	}

	for (;;)
	{
		int len = uart_read_bytes(link->config.uart_num, rx_chunk, sizeof(rx_chunk), timeout_ms);
		if (len > 0)
		{
			size_t offset = 0U;
			ProtocolMessage msg;
			ProtocolParseResult res;
			while ((res = Protocol_ParserProcess(&link->parser, rx_chunk, (size_t)len, &offset, &msg)) != PROTOCOL_PARSE_NONE)
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
*	Add a callback for one message id.
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
