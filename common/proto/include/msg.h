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

	MSG_ID_RTC_TIME = 0x0201, /* STM32 -> ESP32 */
	MSG_ID_RTC_SET = 0x0202,  /* ESP32 -> STM32 */

	MSG_ID_ENV_DATA = 0x0301, /* STM32 -> ESP32: BME280 */
} ProtoMessageId;

#define PROTO_MSG_ID_BUTTON_FIRST MSG_ID_BUTTON_PRESSED
#define PROTO_MSG_ID_BUTTON_LAST  MSG_ID_BUTTON_LONG_PRESS

static inline bool ProtoMsg_IsButtonEvent(uint16_t id)
{
	return (id >= (uint16_t)PROTO_MSG_ID_BUTTON_FIRST) && (id <= (uint16_t)PROTO_MSG_ID_BUTTON_LAST);
}

/* ---- Button event ---- */

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

/* ---- Sys ping/pong ---- */

#define PROTO_SYS_PING_SIZE 4U

typedef struct
{
	uint32_t uptime_ms;
} ProtoSysPing;

ProtocolStatus ProtoMsg_EncodeSysPing(const ProtoSysPing *in, uint8_t *out, size_t out_size, size_t *out_length);
ProtocolStatus ProtoMsg_DecodeSysPing(const uint8_t *data, size_t length, ProtoSysPing *out);

/* ---- RTC time (MSG_ID_RTC_TIME / MSG_ID_RTC_SET) ---- */

#define PROTO_RTC_TIME_SIZE 8U

typedef struct
{
	uint16_t year;   /* 2026 */
	uint8_t month;   /* 1..12 */
	uint8_t date;    /* 1..31 */
	uint8_t weekday; /* 1..7, 1 = Sunday */
	uint8_t hours;   /* 0..23 */
	uint8_t minutes; /* 0..59 */
	uint8_t seconds; /* 0..59 */
} ProtoRtcTime;

ProtocolStatus ProtoMsg_EncodeRtcTime(const ProtoRtcTime *in, uint8_t *out, size_t out_size, size_t *out_length);
ProtocolStatus ProtoMsg_DecodeRtcTime(const uint8_t *data, size_t length, ProtoRtcTime *out);

/* ---- Environment (MSG_ID_ENV_DATA) ---- */

#define PROTO_ENV_DATA_SIZE 8U

typedef struct
{
	int16_t temperature_c_x100;  /* 2345 = 23.45 C     */
	uint16_t humidity_pct_x100;  /* 4567 = 45.67 %RH   */
	uint32_t pressure_pa;        /* 101325 = 1013.25 hPa */
} ProtoEnvData;

ProtocolStatus ProtoMsg_EncodeEnvData(const ProtoEnvData *in, uint8_t *out, size_t out_size, size_t *out_length);
ProtocolStatus ProtoMsg_DecodeEnvData(const uint8_t *data, size_t length, ProtoEnvData *out);

#ifdef __cplusplus
}
#endif

#endif
