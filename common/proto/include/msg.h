#ifndef SHARED_PROTOCOL_MESSAGES_H
#define SHARED_PROTOCOL_MESSAGES_H

#include "protocol.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	MSG_ID_NONE = 0x0000,

	MSG_ID_SYS_PING = 0x0001,
	MSG_ID_SYS_PONG = 0x0002,

	MSG_ID_BUTTON_PRESSED = 0x0101,
	MSG_ID_BUTTON_RELEASED = 0x0102,
	MSG_ID_BUTTON_CLICKS = 0x0103,
	MSG_ID_BUTTON_LONG_PRESS = 0x0104,
} ProtoMessageId;

#define PROTO_MSG_ID_BUTTON_FIRST MSG_ID_BUTTON_PRESSED
#define PROTO_MSG_ID_BUTTON_LAST  MSG_ID_BUTTON_LONG_PRESS

static inline bool ProtoMsg_IsButtonEvent(uint16_t id)
{
	return (id >= (uint16_t)PROTO_MSG_ID_BUTTON_FIRST) && (id <= (uint16_t)PROTO_MSG_ID_BUTTON_LAST);
}

#define PROTO_BUTTON_EVENT_SIZE 8U

typedef struct
{
	uint8_t button_id;
	uint8_t click_count;
	uint16_t duration_ms;
	uint32_t timestamp_ms;
} ProtoButtonEvent;

ProtocolStatus ProtoMsg_EncodeButtonEvent(const ProtoButtonEvent *in, uint8_t *out, size_t out_size, size_t *out_length);
ProtocolStatus ProtoMsg_DecodeButtonEvent(const uint8_t *data, size_t length, ProtoButtonEvent *out);

#define PROTO_SYS_PING_SIZE 4U

typedef struct
{
	uint32_t uptime_ms;
} ProtoSysPing;

ProtocolStatus ProtoMsg_EncodeSysPing(const ProtoSysPing *in, uint8_t *out, size_t out_size, size_t *out_length);
ProtocolStatus ProtoMsg_DecodeSysPing(const uint8_t *data, size_t length, ProtoSysPing *out);

#ifdef __cplusplus
}
#endif

#endif
