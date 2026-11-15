/*
*	Packet protocol between STM32 and ESP32 (over UART).
*
* One packet looks like this:
*		[SOF0][SOF1] [version] [message id: 2 bytes] [seq][length: 2 bytes] [payload...] [CRC16: 2 bytes]
*
*	SOF = "start of frame" marker bytes. The receiver looks for them to find a packet start.
*	seq = packet counter. CRC16 = checksum to find broken data.
*/

#include "protocol.h"
#include <string.h>

/*
*	Byte positions inside the header.
*/
#define OFFSET_VERSION 2U
#define OFFSET_MSG_ID  3U
#define OFFSET_SEQ     5U
#define OFFSET_LEN     6U
#define OFFSET_PAYLOAD PROTOCOL_HEADER_SIZE

/*
*	Read/write a 16-bit number in little-endian order (low byte first).
*/
static uint16_t read_u16le(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static void write_u16le(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)(value & 0xFFU);
	p[1] = (uint8_t)(value >> 8);
}

/*
*	CRC16 checksum (CCITT type, start value 0xFFFF). If one bit changes, the CRC changes.
*/
uint16_t Protocol_Crc16(const uint8_t *data, size_t length)
{
	uint16_t crc = 0xFFFFU;
	for (size_t i = 0; i < length; i++)
	{
		// Mix the next byte into the CRC, then process it bit by bit.
		crc ^= (uint16_t)((uint16_t)data[i] << 8);
		for (uint8_t bit = 0; bit < 8U; bit++)
		{
			// If the top bit is 1, shift and XOR with the polynomial 0x1021. If it is 0, only shift.
			if ((crc & 0x8000U) != 0U)
			{
				crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021U);
			}
			else
			{
				crc = (uint16_t)(crc << 1);
			}
		}
	}
	return crc;
}

/*
*	Build one full packet in "out". Returns the packet size in out_length.
*/
ProtocolStatus Protocol_Encode(
	uint8_t *out,
	size_t out_size,
	size_t *out_length,
	uint16_t message_id,
	uint8_t seq,
	const void *payload,
	uint16_t length)
{
	// Check the input and make sure the payload and the buffer are not too big.
	if ((out == NULL) || (out_length == NULL) || ((payload == NULL) && (length > 0U)))
	{
		return PROTOCOL_INVALID_ARG;
	}
	if (length > PROTOCOL_MAX_PAYLOAD)
	{
		return PROTOCOL_PAYLOAD_TOO_LARGE;
	}

	// Total size = header + payload + CRC.
	const size_t total = (size_t)PROTOCOL_HEADER_SIZE + length + PROTOCOL_CRC_SIZE;
	if (out_size < total)
	{
		return PROTOCOL_BUFFER_TOO_SMALL;
	}

	// Write the header fields one by one.
	out[0] = PROTOCOL_SOF0;
	out[1] = PROTOCOL_SOF1;
	out[OFFSET_VERSION] = (uint8_t)PROTOCOL_VERSION;
	write_u16le(&out[OFFSET_MSG_ID], message_id);
	out[OFFSET_SEQ] = seq;
	write_u16le(&out[OFFSET_LEN], length);

	if (length > 0U)
	{
		memcpy(&out[OFFSET_PAYLOAD], payload, length);
	}

	// Calculate CRC over the header (after the SOF bytes) and the payload, then put it at the end.
	const uint16_t crc = Protocol_Crc16(
		&out[PROTOCOL_CRC_START_OFFSET],
		(size_t)(PROTOCOL_HEADER_SIZE - PROTOCOL_CRC_START_OFFSET) + length);
	write_u16le(&out[OFFSET_PAYLOAD + length], crc);

	*out_length = total;
	return PROTOCOL_OK;
}

/*
*	Prepare the parser. max_payload is the biggest payload we accept.
*/
ProtocolStatus Protocol_ParserInit(ProtocolParser *parser, uint16_t max_payload)
{
	if ((parser == NULL) || (max_payload > PROTOCOL_MAX_PAYLOAD))
	{
		return PROTOCOL_INVALID_ARG;
	}
	parser->max_payload = max_payload;
	Protocol_ParserReset(parser);
	return PROTOCOL_OK;
}

/*
*	Forget everything and wait for a new packet start.
*/
void Protocol_ParserReset(ProtocolParser *parser)
{
	if (parser == NULL)
	{
		return;
	}
	parser->fill = 0U;
	parser->total = 0U;
	parser->replay_pos = 0U;
	parser->replay_end = 0U;
	parser->state = PROTOCOL_STATE_HUNT_SOF0;
}

/*
*	Something went wrong (bad data). Do NOT throw away all bytes.
*	We skip only the first byte (the false SOF0) and read the rest again ("replay"),
*	because a real packet start may be hidden inside.
*/
static void parser_resync(ProtocolParser *p)
{
	const uint16_t pending = (uint16_t)(p->replay_end - p->replay_pos);
	memmove(&p->buf[p->fill], &p->buf[p->replay_pos], pending);
	p->replay_pos = 1U;
	p->replay_end = (uint16_t)(p->fill + pending);
	p->fill = 0U;
	p->state = PROTOCOL_STATE_HUNT_SOF0;
}

/*
*	The parser reads ONE byte at a time. It is a state machine:
*	HUNT_SOF0 -> WAIT_SOF1 -> HEADER -> BODY -> message is ready.
*/
static ProtocolParseResult parser_step(ProtocolParser *p, uint8_t byte, ProtocolMessage *message)
{
	switch (p->state)
	{
	// Look for the first start byte. Other bytes are noise.
	case PROTOCOL_STATE_HUNT_SOF0:
		if (byte == PROTOCOL_SOF0)
		{
			p->buf[0] = byte;
			p->fill = 1U;
			p->state = PROTOCOL_STATE_WAIT_SOF1;
			return PROTOCOL_PARSE_NONE;
		}
		return PROTOCOL_PARSE_BAD_SOF;

	// The second start byte must come right after the first.
	case PROTOCOL_STATE_WAIT_SOF1:
		if (byte == PROTOCOL_SOF1)
		{
			p->buf[p->fill++] = byte;
			p->state = PROTOCOL_STATE_HEADER;
			return PROTOCOL_PARSE_NONE;
		}
		if (byte == PROTOCOL_SOF0)
		{
			return PROTOCOL_PARSE_BAD_SOF;
		}
		p->fill = 0U;
		p->state = PROTOCOL_STATE_HUNT_SOF0;
		return PROTOCOL_PARSE_BAD_SOF;

	// Collect header bytes. When the header is complete, check version and length.
	case PROTOCOL_STATE_HEADER:
	{
		p->buf[p->fill++] = byte;
		if (p->fill < PROTOCOL_HEADER_SIZE)
		{
			return PROTOCOL_PARSE_NONE;
		}
		// Wrong protocol version -> not a good packet.
		if (p->buf[OFFSET_VERSION] != (uint8_t)PROTOCOL_VERSION)
		{
			parser_resync(p);
			return PROTOCOL_PARSE_BAD_VERSION;
		}
		// Too long payload -> not a good packet. Otherwise we know the full packet size now.
		const uint16_t length = read_u16le(&p->buf[OFFSET_LEN]);
		if (length > p->max_payload)
		{
			parser_resync(p);
			return PROTOCOL_PARSE_BAD_LENGTH;
		}
		p->total = (uint16_t)(PROTOCOL_HEADER_SIZE + length + PROTOCOL_CRC_SIZE);
		p->state = PROTOCOL_STATE_BODY;
		return PROTOCOL_PARSE_NONE;
	}

	// Collect payload and CRC bytes. When all bytes are here, check the CRC.
	case PROTOCOL_STATE_BODY:
	{
		p->buf[p->fill++] = byte;
		if (p->fill < p->total)
		{
			return PROTOCOL_PARSE_NONE;
		}
		const uint16_t length = (uint16_t)(p->total - PROTOCOL_HEADER_SIZE - PROTOCOL_CRC_SIZE);
		// expected = CRC from the packet. actual = CRC we calculate. They must be equal.
		const uint16_t expected = read_u16le(&p->buf[PROTOCOL_HEADER_SIZE + length]);
		const uint16_t actual = Protocol_Crc16(
			&p->buf[PROTOCOL_CRC_START_OFFSET],
			(size_t)(PROTOCOL_HEADER_SIZE - PROTOCOL_CRC_START_OFFSET) + length);

		if (expected != actual)
		{
			parser_resync(p);
			return PROTOCOL_PARSE_BAD_CRC;
		}

		// CRC is OK: give the finished message to the caller.
		message->id = read_u16le(&p->buf[OFFSET_MSG_ID]);
		message->seq = p->buf[OFFSET_SEQ];
		message->length = length;
		message->payload = &p->buf[OFFSET_PAYLOAD];

		p->fill = 0U;
		p->state = PROTOCOL_STATE_HUNT_SOF0;
		return PROTOCOL_PARSE_MESSAGE;
	}

	// Should never happen. Reset to be safe.
	default:
		Protocol_ParserReset(p);
		return PROTOCOL_PARSE_NONE;
	}
}

/*
*	Feed the parser with received bytes. Call it in a loop until it returns PARSE_NONE.
*	It can return one message or one error type each time.
*/
ProtocolParseResult Protocol_ParserProcess(
	ProtocolParser *parser,
	const uint8_t *data,
	size_t length,
	size_t *offset,
	ProtocolMessage *message)
{
	if ((parser == NULL) || (offset == NULL) || (message == NULL) || ((data == NULL) && (length > 0U)))
	{
		return PROTOCOL_PARSE_NONE;
	}

	for (;;)
	{
		uint8_t byte;
		bool from_replay;

		// First use the bytes waiting for "replay", then the new bytes.
		if (parser->replay_pos < parser->replay_end)
		{
			byte = parser->buf[parser->replay_pos++];
			if (parser->replay_pos == parser->replay_end)
			{
				parser->replay_pos = 0U;
				parser->replay_end = 0U;
			}
			from_replay = true;
		}
		else if (*offset < length)
		{
			byte = data[(*offset)++];
			from_replay = false;
		}
		else
		{
			return PROTOCOL_PARSE_NONE;
		}

		const ProtocolParseResult result = parser_step(parser, byte, message);
		if (result == PROTOCOL_PARSE_NONE)
		{
			continue;
		}
		// Do not report "bad SOF" for replayed bytes. We already counted that error.
		if ((result == PROTOCOL_PARSE_BAD_SOF) && from_replay)
		{
			continue;
		}
		return result;
	}
}
