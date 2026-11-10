/*
*	Logger: prints messages to UART without slowing the other tasks.
*	Idea: LOG_x() only puts a small event in a queue (fast).
*	A separate logger task makes the text and sends it with DMA (slow part).
*/

#include "logger.h"
#include "FreeRTOS.h"
#include "portmacro.h"
#include "projdefs.h"
#include "queue.h"
#include "task.h"
#include <stdatomic.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LOGGER_MIN_MESSAGE_LENGTH 32U

/*
*	Queue log event. ~40 bytes
*	We do NOT copy the whole text. We keep a pointer to the format string
*	and up to 6 arguments. The text is built later by the logger task.
*/
typedef struct {
	const char 	*fmt;
	const char 	*file;
	uint32_t 		tick;
	LogArg 			args[LOGGER_MAX_ARGS];
	uint16_t 		line;
	uint8_t 		level;
} LogEvent;

/*
*	Module state (one logger in the whole program).
*	s_dropped counts messages we lost when the queue was full.
*/
static UART_HandleTypeDef *s_huart;
static QueueHandle_t 			s_queue;
static TaskHandle_t 			s_task;
static uint8_t 						*s_tx_buf;
static uint32_t 					s_tx_size;
static uint32_t 					s_max_len;
static LogLevel 					s_min_level;
static atomic_uint 				s_dropped;
 
#define LOGGER_FAIL(err) RESULT_ERROR(LoggerError, LoggerError, (err))

/*
*	Make one text line from one event, like: "[1234] I file.c:10: message".
*	Returns the length of the line.
*/
static uint32_t format_event(const LogEvent *e, char *out)
{
	const size_t limit = (size_t)s_max_len - 2U; // 2 bytes for "\r\n"
	// Pick a letter for the level: D, I, W or E.
	const char level_char = "DIWE"[e->level & 3U];

	int n;

	// prefix
	if (e->file != NULL)
	{
		n = snprintf(out, limit, "[%lu] %c %s:%u: ", (unsigned long)e->tick, level_char, e->file, (unsigned)e->line);
	}
	else
	{
		n = snprintf(out, limit, "[%lu] %c: ", (unsigned long)e->tick, level_char);
	}

	// pos = how many characters we wrote. We make sure it never goes past the buffer.
	size_t pos = (n < 0) ? 0U : (((size_t)n < limit) ? (size_t)n : (limit - 1U));

	const size_t room = limit - pos;

// The format string is not a fixed text, so the compiler warns about it.
// We turn these warnings off only for the next line.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"

	// Put the message text after the prefix. snprintf never writes more than "room" bytes.
	n = snprintf(&out[pos], room, e->fmt, e->args[0], e->args[1], e->args[2], e->args[3], e->args[4], e->args[5]);

#pragma GCC diagnostic pop

	if (n > 0)
	{
		pos += ((size_t)n < room) ? (size_t)n : (room - 1U);
	}

	// End the line with CR LF.
	out[pos++] = '\r';
	out[pos++] = '\n';

	return (uint32_t)pos;
}

/*
*	One round of the logger task: wait, collect messages, send them in one DMA transfer.
*/
static void process_queue(void)
{
	LogEvent e;

	// 1. Sleep while first event not apear
	if (xQueueReceive(s_queue, &e, portMAX_DELAY) != pdPASS)
	{
		return;
	}

	uint32_t len = format_event(&e, (char *)s_tx_buf);

	// 2. Add all event from queue, while tx_buf has size for one more raw max length
	while (
		(len + s_max_len <= s_tx_size)
		&& (xQueueReceive(s_queue, &e, 0) == pdPASS)
	)
	{
		len += format_event(&e, (char *)&s_tx_buf[len]);
	}

	// 3. Clear old notification and start DMA
	ulTaskNotifyTake(pdTRUE, 0);
	
	if (HAL_UART_Transmit_DMA(s_huart, s_tx_buf, (uint16_t)len) != HAL_OK)
	{
		return;
	}

	// 4. Wait signal form ISR.
	// How long can sending take? About 12 bits per byte at the baud rate, plus 20 ms extra.
	const uint32_t wait_ms = (uint32_t)(((uint64_t)len * 12000U) / s_huart->Init.BaudRate) + 20U;

	// If no signal, abort TX
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

/*
*	Called from an interrupt. It wakes the logger task ("sending is done").
*/
static void notify_task_from_isr(void)
{
	BaseType_t woken = pdFALSE;
	vTaskNotifyGiveFromISR(s_task, &woken);
	portYIELD_FROM_ISR(woken);
}

/*
*	Start the logger. Needs the UART with a DMA TX channel in normal mode.
*/
Result(LoggerError, LoggerError) Logger_Init(const LoggerConfig *cfg)
{
	// First: do not start twice. Then: check the config (UART, DMA mode, sizes).
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

	// One big TX buffer, so we can send many lines together. If memory fails, free what we got.
	const uint32_t tx_size = (uint32_t)cfg->max_message_length * LOGGER_TX_BATCH;
	uint8_t *tx_buf = (uint8_t *)pvPortMalloc(tx_size);
	QueueHandle_t queue = xQueueCreate(cfg->queue_length, sizeof(LogEvent));
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

	// Set the queue LAST. Until now Logger_Log() ignores all calls, so nothing is used too early.
	s_queue = queue;
	return RESULT_OK(LoggerError, LoggerError, LOGGER_ERROR_NONE);
}

/*
*	Called by the LOG_x macros. It works in tasks and in interrupts. It never waits.
*/
void Logger_Log(LogLevel level, const char *file, int line, const char *fmt, LogArg a0, LogArg a1, LogArg a2, LogArg a3, LogArg a4, LogArg a5)
{
	if ((s_queue == NULL) || (level < s_min_level))
	{
		return;
	}

	if (level > LOG_ERROR)
	{
		level = LOG_ERROR;
	}

	// IPSR is not 0 when we are inside an interrupt. Then we must use the "FromISR" functions.
	const bool isISR = (__get_IPSR() != 0U);
	const LogEvent e = {
		.fmt = fmt,
		.file = file,
		.tick = isISR ? (uint32_t)xTaskGetTickCountFromISR() : (uint32_t)xTaskGetTickCount(),
		.args = {a0, a1, a2, a3, a4, a5},
		.line = (uint16_t)line,
		.level = (uint8_t)level,
	};

	BaseType_t ok;
	if (isISR)
	{
		BaseType_t woken = pdFALSE;
		ok = xQueueSendFromISR(s_queue, &e, &woken);
		portYIELD_FROM_ISR(woken);
	}
	else
	{
		ok = xQueueSend(s_queue, &e, 0);
	}

	// Queue is full: we drop the message and count it.
	if (ok != pdPASS)
	{
		atomic_fetch_add(&s_dropped, 1U);
	}
}

/*
*	Called when UART sending is finished. Wake the logger task.
*/
void Logger_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
	if (huart == s_huart)
	{
		notify_task_from_isr();
	}
}

/*
*	UART error: wake the task too, so it does not wait for the full timeout.
*/
void Logger_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	if ((huart == s_huart) && (huart->gState != HAL_UART_STATE_BUSY_TX))
	{
		notify_task_from_isr();
	}
}

/*
*	How many messages were lost.
*/
uint32_t Logger_GetDroppedCount(void)
{
	return atomic_load(&s_dropped);
}
