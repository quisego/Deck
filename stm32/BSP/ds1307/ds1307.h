#ifndef BSP_DS1307_H
#define BSP_DS1307_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "msg.h"
#include "result.h"

#define DS1307_I2C_ADDR       (0x68 << 1)
#define DS1307_I2C_TIMEOUT    100U
#define DS1307_REG_SECONDS    0x00
#define DS1307_REG_MINUTES    0x01
#define DS1307_REG_HOURS      0x02
#define DS1307_REG_DAY        0x03
#define DS1307_REG_DATE       0x04
#define DS1307_REG_MONTH      0x05
#define DS1307_REG_YEAR       0x06
#define DS1307_REG_CONTROL    0x07
#define DS1307_CH_BIT         0x80
#define DS1307_HOURS_12H_BIT  0x40
#define DS1307_HOURS_PM_BIT   0x20

typedef struct DS1307 DS1307;
typedef struct DS1307_Config DS1307_Config;
typedef struct DS1307_Ops DS1307_Ops;
typedef enum DS1307_Error DS1307_Error;

enum DS1307_Error
{
	DS1307_ERROR_NONE = 0,
	DS1307_ERROR_NULLPTR,
	DS1307_ERROR_INVALID_ARG,
	DS1307_ERROR_UNINITIALIZED,
	DS1307_ERROR_QUEUE_FULL,
	DS1307_ERROR_TASK_CREATE_FAILED,
};

struct DS1307_Config
{
	I2C_HandleTypeDef *hi2c;
	uint16_t addr;
	uint32_t poll_ms;
};

struct DS1307
{
	DS1307_Config config;
	const DS1307_Ops *ops;
};

DEFINE_RESULT(DS1307, DS1307_Error);

struct DS1307_Ops
{
	bool (*get_last)(const DS1307 *self, ProtoRtcTime *out);
	DS1307_Error (*set_time)(const DS1307 *self, const ProtoRtcTime *time);
	bool (*is_online)(const DS1307 *self);
};

typedef void (*DS1307_EventCallback)(uint16_t message_id, const ProtoRtcTime *time, void *ctx);

void DS1307_SetEventCallback(DS1307_EventCallback callback, void *ctx);

Result(DS1307, DS1307_Error) DS1307_Init(const DS1307_Config *config);

bool DS1307_GetLast(const DS1307 *self, ProtoRtcTime *out);

DS1307_Error DS1307_SetTime(const DS1307 *self, const ProtoRtcTime *time);

void DS1307_TimeFormat(const ProtoRtcTime *time, char *buf, size_t size);
void DS1307_DateFormat(const ProtoRtcTime *time, char *buf, size_t size);

bool DS1307_IsOnline(const DS1307 *self);

#ifdef __cplusplus
}
#endif

#endif
