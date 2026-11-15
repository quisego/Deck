#ifndef STM32_LINK_H
#define STM32_LINK_H

#include <stdint.h>
#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LINK_MAX_SUBSCRIBERS
#define LINK_MAX_SUBSCRIBERS 16U
#endif

#ifndef LINK_RX_BUFFER_SIZE
#define LINK_RX_BUFFER_SIZE 256U
#endif

#ifndef LINK_MAX_INSTANCES
#define LINK_MAX_INSTANCES 2U
#endif

typedef enum
{
	LINK_OK,
	LINK_ERROR,
	LINK_QUEUE_FULL,
	LINK_TIMEOUT,
	LINK_INVALID_ARG,
	LINK_NOT_INITIALIZED,
	LINK_NO_SPACE,
	LINK_ALREADY_EXISTS,
	LINK_NOT_FOUND
} LinkStatus;

typedef struct
{
	uint16_t id;
	uint16_t length;
	const uint8_t *payload;
} LinkMessage;

typedef void (*Link_Callback)(const LinkMessage *message, void *context);

typedef struct
{
	uint32_t tx_messages;
	uint32_t tx_queue_full;
	uint32_t tx_timeouts;
	uint32_t tx_errors;
	uint32_t rx_messages;
	uint32_t rx_unknown_id;
	uint32_t rx_overflow;
	uint32_t rx_uart_errors;
	uint32_t rx_invalid_sof;
	uint32_t rx_invalid_version;
	uint32_t rx_invalid_length;
	uint32_t rx_crc_errors;
} LinkStats;

typedef struct
{
	UART_HandleTypeDef *huart;
	uint16_t tx_queue_length;
	uint16_t max_payload;
	uint16_t tx_task_stack_words;
	uint16_t rx_task_stack_words;
	UBaseType_t tx_task_priority;
	UBaseType_t rx_task_priority;
	TickType_t tx_timeout_ticks;
	TickType_t rx_poll_ticks;
} LinkConfig;

typedef struct
{
	uint8_t in_use;
	uint16_t message_id;
	Link_Callback callback;
	void *context;
} LinkSubscriber;

typedef struct Link
{
	LinkConfig config;
	ProtocolParser parser;
	uint8_t initialized;

	UART_HandleTypeDef *huart;
	uint8_t rx_buf[LINK_RX_BUFFER_SIZE];
	uint32_t rx_read_total;
	volatile uint32_t rx_halves;
	volatile uint8_t rx_restart_pending;
	volatile uint8_t tx_state;
	TaskHandle_t rx_waiter;
	TaskHandle_t tx_waiter;
	volatile uint32_t rx_overflow;
	volatile uint32_t uart_errors;

	uint8_t *tx_pool;
	uint16_t tx_slot_size;
	QueueHandle_t tx_free_queue;
	QueueHandle_t tx_ready_queue;
	uint8_t tx_packet[PROTOCOL_MAX_PACKET_SIZE];
	uint8_t tx_seq;
	TaskHandle_t tx_task;

	uint8_t rx_chunk[64U];
	LinkSubscriber rx_snapshot[LINK_MAX_SUBSCRIBERS];
	TaskHandle_t rx_task;

	SemaphoreHandle_t lock;
	LinkSubscriber subscribers[LINK_MAX_SUBSCRIBERS];
	LinkStats stats;
} Link;

LinkStatus Link_Init(Link *link, const LinkConfig *config);
LinkStatus Link_Send(Link *link, uint16_t message_id, const void *payload, uint16_t length, TickType_t queue_wait_ticks);
LinkStatus Link_Subscribe(Link *link, uint16_t message_id, Link_Callback callback, void *context);
LinkStatus Link_Unsubscribe(Link *link, uint16_t message_id, Link_Callback callback, void *context);
LinkStatus Link_GetStats(Link *link, LinkStats *out);
LinkStatus Link_ResetStats(Link *link);

/* STM32 HAL ISR Callbacks Routing */
void Link_UART_IRQHandler(UART_HandleTypeDef *huart);
void Link_UART_TxCpltCallback(UART_HandleTypeDef *huart);
void Link_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart);
void Link_UART_RxCpltCallback(UART_HandleTypeDef *huart);
void Link_UART_ErrorCallback(UART_HandleTypeDef *huart);

#define LINK_SEND(link, id, data) \
	Link_Send((link), (id), &(data), (uint16_t)sizeof(data), 0)

#define LINK_SEND_WAIT(link, id, data, wait_ticks) \
	Link_Send((link), (id), &(data), (uint16_t)sizeof(data), (wait_ticks))

#ifdef __cplusplus
}
#endif

#endif
