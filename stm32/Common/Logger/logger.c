#include "logger.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOGGER_MIN_MESSAGE_LENGTH 32U

static UART_HandleTypeDef *s_huart;
static QueueHandle_t 			s_queue;
static TaskHandle_t 			s_task;
static uint8_t 						*s_tx_buf;
static uint32_t 					s_tx_size;
static uint32_t 					s_max_len;
static LogLevel 					s_min_level;
static atomic_uint 				s_dropped;
 
#define LOGGER_FAIL(err) RESULT_ERROR(LoggerError, LoggerError, (err))

static void process_queue(void)
{
	if (xQueueReceive(s_queue, s_tx_buf, portMAX_DELAY) != pdPASS)
	{
		return;
	}

	uint32_t len = (uint32_t)strlen((const char *)s_tx_buf);

	while (
		(len + s_max_len <= s_tx_size)
		&& (xQueueReceive(s_queue, &s_tx_buf[len], 0) == pdPASS)
	)
	{
		len += (uint32_t)strlen((const char *)&s_tx_buf[len]);
	}

	ulTaskNotifyTake(pdTRUE, 0);
	
	if (HAL_UART_Transmit_DMA(s_huart, s_tx_buf, (uint16_t)len) != HAL_OK)
	{
		return;
	}

	const uint32_t wait_ms = (uint32_t)(((uint64_t)len * 12000U) / s_huart->Init.BaudRate) + 20U;

	if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms)) == 0U)
	{
		HAL_UART_AbortTransmit(s_huart);
	}
}

static void logger_task(void *arg)
{
	while (true)
	{
		process_queue();
	}
}

static void notify_task_from_isr(void)
{
	BaseType_t woken = pdFALSE;
	vTaskNotifyGiveFromISR(s_task, &woken);
	portYIELD_FROM_ISR(woken);
}

Result(LoggerError, LoggerError) Logger_Init(const LoggerConfig *cfg)
{
	if (s_queue != NULL)
	{
		return LOGGER_FAIL(LOGGER_ERROR_ALREADY_INITIALIZED);
	}

	if (
		(cfg == NULL)
		|| (cfg->huart == NULL)
		|| (cfg->huart->hdmatx == NULL)
		|| (cfg->huart->hdmatx->Init.Mode != DMA_NORMAL)
		|| (cfg->huart->Init.BaudRate == 0U)
		|| (cfg->min_log_level > LOG_ERROR) 
		|| (cfg->queue_length == 0U) 
		|| (cfg->max_message_length < LOGGER_MIN_MESSAGE_LENGTH) 
		|| (cfg->max_message_length > LOGGER_MAX_MESSAGE_LENGTH_LIMIT) 
		|| (cfg->task_stack_words == 0U)
	)
	{
		return LOGGER_FAIL(LOGGER_ERROR_INVALID_CONFIG);
	}

	const uint32_t tx_size = (uint32_t)cfg->max_message_length * LOGGER_TX_BATCH;
	uint8_t *tx_buf = (uint8_t *)pvPortMalloc(tx_size);
	QueueHandle_t queue = xQueueCreate(cfg->queue_length, cfg->max_message_length);
	if ((tx_buf == NULL) || (queue == NULL))
	{
		if (tx_buf != NULL)
		{
			vPortFree(tx_buf);
		}

		if (queue != NULL)
		{
			vQueueDelete(queue);
		}

		return LOGGER_FAIL(LOGGER_ERROR_NO_MEMORY);
	}
 
	s_huart = cfg->huart;
	s_tx_buf = tx_buf;
	s_tx_size = tx_size;
	s_max_len = cfg->max_message_length;
	s_min_level = cfg->min_log_level;
	atomic_store(&s_dropped, 0U);
 
	if (xTaskCreate(logger_task, "Logger", cfg->task_stack_words, NULL, cfg->task_priority, &s_task) != pdPASS)
	{
		vPortFree(tx_buf);
		vQueueDelete(queue);
		return LOGGER_FAIL(LOGGER_ERROR_TASK_CREATE_FAILED);
	}
 
	s_queue = queue;
	return RESULT_OK(LoggerError, LoggerError, LOGGER_ERROR_NONE);
}

void Logger_Log(LogLevel level, const char *file, int line, const char *fmt, ...)
{
	if ((s_queue == NULL) || (level < s_min_level))
	{
		return;
	}

	if (level > LOG_ERROR)
	{
		level = LOG_ERROR;
	}

	const bool isISR = (__get_IPSR() != 0U);
	const unsigned long tick = isISR ? (unsigned long)xTaskGetTickCountFromISR() : (unsigned long)xTaskGetTickCount();
	const size_t cap = s_max_len;
	const size_t limit = cap - 2U;

	char msg[cap];

	const char level_char = "DIWE"[level];
	
	int n;
	if (file != NULL)
	{
		n = snprintf(msg, limit, "[%lu] %c %s:%d: ", tick, level_char, file, line);
	}
	else
	{
		n = snprintf(msg, limit, "[%lu] %c: ", tick, level_char);
	}

	size_t pos = (n < 0)
		? 0U : (((size_t)n < limit)
			? (size_t)n : (limit - 1U));
	
	const size_t room = limit - pos;
	
	va_list ap;
	va_start(ap, fmt);
	n = vsnprintf(&msg[pos], room, fmt, ap);
	va_end(ap);

	if (n > 0)
	{
		pos += ((size_t)n < room) ? (size_t)n : (room - 1U);
	}

	msg[pos++] = '\r';
	msg[pos++] = '\n';
	msg[pos] = '\0';

	BaseType_t ok;
	if (isISR)
	{
		BaseType_t woken = pdFALSE;
		ok = xQueueSendFromISR(s_queue, msg, &woken);
		portYIELD_FROM_ISR(woken);
	}
	else
	{
		ok = xQueueSend(s_queue, msg, 0);
	}

	if (ok != pdPASS)
	{
		atomic_fetch_add(&s_dropped, 1U);
	}
}

void Logger_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
	if (huart == s_huart)
	{
		notify_task_from_isr();
	}
}

void Logger_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	if ((huart == s_huart) && (huart->gState != HAL_UART_STATE_BUSY_TX))
	{
		notify_task_from_isr();
	}
}

uint32_t Logger_GetDroppedCount(void)
{
	return atomic_load(&s_dropped);
}
