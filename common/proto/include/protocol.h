#ifndef SHARED_COMMUNICATION_PROTOCOL_H
#define SHARED_COMMUNICATION_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROTOCOL_SOF0              0xA5U
#define PROTOCOL_SOF1              0x5AU
#define PROTOCOL_VERSION           1U

#define PROTOCOL_HEADER_SIZE       8U
#define PROTOCOL_CRC_SIZE          2U
#define PROTOCOL_MAX_PAYLOAD       256U
#define PROTOCOL_MAX_PACKET_SIZE   (PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD + PROTOCOL_CRC_SIZE)

#define PROTOCOL_CRC_START_OFFSET  2U

typedef enum
{
	PROTOCOL_OK,
	PROTOCOL_INVALID_ARG,
	PROTOCOL_PAYLOAD_TOO_LARGE,
	PROTOCOL_BUFFER_TOO_SMALL
} ProtocolStatus;

typedef struct
{
	uint16_t id;
	uint8_t seq;
	uint16_t length;
	const uint8_t *payload;
} ProtocolMessage;

typedef enum
{
	PROTOCOL_PARSE_NONE,
	PROTOCOL_PARSE_MESSAGE,
	PROTOCOL_PARSE_BAD_SOF,
	PROTOCOL_PARSE_BAD_VERSION,
	PROTOCOL_PARSE_BAD_LENGTH,
	PROTOCOL_PARSE_BAD_CRC
} ProtocolParseResult;

typedef enum
{
	PROTOCOL_STATE_HUNT_SOF0,
	PROTOCOL_STATE_WAIT_SOF1,
	PROTOCOL_STATE_HEADER,
	PROTOCOL_STATE_BODY
} ProtocolParserState;

typedef struct
{
	uint8_t buf[PROTOCOL_MAX_PACKET_SIZE];
	uint16_t fill;
	uint16_t total;
	uint16_t replay_pos;
	uint16_t replay_end;
	uint16_t max_payload;
	ProtocolParserState state;
} ProtocolParser;

uint16_t Protocol_Crc16(const uint8_t *data, size_t length);

ProtocolStatus Protocol_Encode(
	uint8_t *out,
	size_t out_size,
	size_t *out_length,
	uint16_t message_id,
	uint8_t seq,
	const void *payload,
	uint16_t length);

ProtocolStatus Protocol_ParserInit(ProtocolParser *parser, uint16_t max_payload);
void Protocol_ParserReset(ProtocolParser *parser);

ProtocolParseResult Protocol_ParserProcess(
	ProtocolParser *parser,
	const uint8_t *data,
	size_t length,
	size_t *offset,
	ProtocolMessage *message);

#ifdef __cplusplus
}
#endif

#endif
