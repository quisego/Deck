#ifndef ESP32_LINK_H
#define ESP32_LINK_H

#include <stdint.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LINK_MAX_SUBSCRIBERS
#define LINK_MAX_SUBSCRIBERS 16U
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
	uart_port_t uart_num;
	int tx_pin;
	int rx_pin;
	int baud_rate;
	int rx_buffer_size;
	int tx_buffer_size;

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

	uint8_t *tx_pool;
	uint16_t tx_slot_size;
	QueueHandle_t tx_free_queue;
	QueueHandle_t tx_ready_queue;
	uint8_t tx_packet[PROTOCOL_MAX_PACKET_SIZE];
	uint8_t tx_seq;
	TaskHandle_t tx_task;

	TaskHandle_t rx_task;
	SemaphoreHandle_t lock;
	LinkSubscriber subscribers[LINK_MAX_SUBSCRIBERS];
	LinkSubscriber rx_snapshot[LINK_MAX_SUBSCRIBERS];
	LinkStats stats;
} Link;

LinkStatus Link_Init(Link *link, const LinkConfig *config);
LinkStatus Link_Send(Link *link, uint16_t message_id, const void *payload, uint16_t length, TickType_t queue_wait_ticks);
LinkStatus Link_Subscribe(Link *link, uint16_t message_id, Link_Callback callback, void *context);
LinkStatus Link_Unsubscribe(Link *link, uint16_t message_id, Link_Callback callback, void *context);
LinkStatus Link_GetStats(Link *link, LinkStats *out);
LinkStatus Link_ResetStats(Link *link);

#define LINK_SEND(link, id, data) \
	Link_Send((link), (id), &(data), (uint16_t)sizeof(data), 0)

#define LINK_SEND_WAIT(link, id, data, wait_ticks) \
	Link_Send((link), (id), &(data), (uint16_t)sizeof(data), (wait_ticks))

#ifdef __cplusplus
}
#endif

#endif
