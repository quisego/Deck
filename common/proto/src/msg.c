#include "msg.h"

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFFU);
	p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFU);
	p[1] = (uint8_t)((v >> 8) & 0xFFU);
	p[2] = (uint8_t)((v >> 16) & 0xFFU);
	p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- Button event ---- */

ProtocolStatus ProtoMsg_EncodeButtonEvent(const ProtoButtonEvent *in, uint8_t *out, size_t out_size, size_t *out_length)
{
	if ((in == NULL) || (out == NULL) || (out_length == NULL))
	{
		return PROTOCOL_INVALID_ARG;
	}
	if (out_size < PROTO_BUTTON_EVENT_SIZE)
	{
		return PROTOCOL_BUFFER_TOO_SMALL;
	}

	out[0] = in->button_id;
	out[1] = in->click_count;
	put_u16(&out[2], in->duration_ms);
	put_u32(&out[4], in->timestamp_ms);

	*out_length = PROTO_BUTTON_EVENT_SIZE;
	return PROTOCOL_OK;
}

ProtocolStatus ProtoMsg_DecodeButtonEvent(const uint8_t *data, size_t length, ProtoButtonEvent *out)
{
	if ((data == NULL) || (out == NULL) || (length != PROTO_BUTTON_EVENT_SIZE))
	{
		return PROTOCOL_INVALID_ARG;
	}

	out->button_id = data[0];
	out->click_count = data[1];
	out->duration_ms = get_u16(&data[2]);
	out->timestamp_ms = get_u32(&data[4]);
	return PROTOCOL_OK;
}

/* ---- Sys ping/pong ---- */

ProtocolStatus ProtoMsg_EncodeSysPing(const ProtoSysPing *in, uint8_t *out, size_t out_size, size_t *out_length)
{
	if ((in == NULL) || (out == NULL) || (out_length == NULL))
	{
		return PROTOCOL_INVALID_ARG;
	}
	if (out_size < PROTO_SYS_PING_SIZE)
	{
		return PROTOCOL_BUFFER_TOO_SMALL;
	}

	put_u32(&out[0], in->uptime_ms);

	*out_length = PROTO_SYS_PING_SIZE;
	return PROTOCOL_OK;
}

ProtocolStatus ProtoMsg_DecodeSysPing(const uint8_t *data, size_t length, ProtoSysPing *out)
{
	if ((data == NULL) || (out == NULL) || (length != PROTO_SYS_PING_SIZE))
	{
		return PROTOCOL_INVALID_ARG;
	}

	out->uptime_ms = get_u32(&data[0]);
	return PROTOCOL_OK;
}

/* ---- RTC time ---- */

ProtocolStatus ProtoMsg_EncodeRtcTime(const ProtoRtcTime *in, uint8_t *out, size_t out_size, size_t *out_length)
{
	if ((in == NULL) || (out == NULL) || (out_length == NULL))
	{
		return PROTOCOL_INVALID_ARG;
	}
	if (out_size < PROTO_RTC_TIME_SIZE)
	{
		return PROTOCOL_BUFFER_TOO_SMALL;
	}

	put_u16(&out[0], in->year);
	out[2] = in->month;
	out[3] = in->date;
	out[4] = in->weekday;
	out[5] = in->hours;
	out[6] = in->minutes;
	out[7] = in->seconds;

	*out_length = PROTO_RTC_TIME_SIZE;
	return PROTOCOL_OK;
}

ProtocolStatus ProtoMsg_DecodeRtcTime(const uint8_t *data, size_t length, ProtoRtcTime *out)
{
	if ((data == NULL) || (out == NULL) || (length != PROTO_RTC_TIME_SIZE))
	{
		return PROTOCOL_INVALID_ARG;
	}

	out->year = get_u16(&data[0]);
	out->month = data[2];
	out->date = data[3];
	out->weekday = data[4];
	out->hours = data[5];
	out->minutes = data[6];
	out->seconds = data[7];
	return PROTOCOL_OK;
}

/* ---- Environment ---- */

ProtocolStatus ProtoMsg_EncodeEnvData(const ProtoEnvData *in, uint8_t *out, size_t out_size, size_t *out_length)
{
	if ((in == NULL) || (out == NULL) || (out_length == NULL))
	{
		return PROTOCOL_INVALID_ARG;
	}
	if (out_size < PROTO_ENV_DATA_SIZE)
	{
		return PROTOCOL_BUFFER_TOO_SMALL;
	}

	put_u16(&out[0], (uint16_t)in->temperature_c_x100);
	put_u16(&out[2], in->humidity_pct_x100);
	put_u32(&out[4], in->pressure_pa);

	*out_length = PROTO_ENV_DATA_SIZE;
	return PROTOCOL_OK;
}

ProtocolStatus ProtoMsg_DecodeEnvData(const uint8_t *data, size_t length, ProtoEnvData *out)
{
	if ((data == NULL) || (out == NULL) || (length != PROTO_ENV_DATA_SIZE))
	{
		return PROTOCOL_INVALID_ARG;
	}

	out->temperature_c_x100 = (int16_t)get_u16(&data[0]);
	out->humidity_pct_x100 = get_u16(&data[2]);
	out->pressure_pa = get_u32(&data[4]);
	return PROTOCOL_OK;
}
