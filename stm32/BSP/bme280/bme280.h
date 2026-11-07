#ifndef BSP_BME280_H
#define BSP_BME280_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "msg.h"
#include "result.h"

#define BME280_REG_ID 0xD0

typedef struct BME280 BME280;
typedef struct BME280_Config BME280_Config;
typedef struct BME280_Ops BME280_Ops;
typedef enum BME280_Error BME280_Error;

enum BME280_Error
{
	BME280_ERROR_NONE = 0,
	BME280_ERROR_NULLPTR,
	BME280_ERROR_INVALID_CONFIG,
	BME280_ERROR_TASK_CREATE_FAILED,
};

struct BME280_Config
{
	SPI_HandleTypeDef *hspi;
	GPIO_TypeDef *cs_port;
	uint16_t cs_pin;

	uint32_t period_ms;

	uint16_t temperature_step_x100; /* 10  -> 0.1 C   */
	uint16_t humidity_step_x100;    /* 100 -> 1 %RH   */
	uint32_t pressure_step_pa;      /* 100 -> 1 hPa   */
};

struct BME280
{
	BME280_Config config;
	const BME280_Ops *ops;
};

DEFINE_RESULT(BME280, BME280_Error);

struct BME280_Ops
{
	bool (*get_last)(const BME280 *self, ProtoEnvData *out);
};

typedef void (*BME280_EventCallback)(uint16_t message_id, const ProtoEnvData *data, void *ctx);

void BME280_SetEventCallback(BME280_EventCallback callback, void *ctx);

Result(BME280, BME280_Error) BME280_Init(const BME280_Config *config);

bool BME280_GetLast(const BME280 *self, ProtoEnvData *out);

void BME280_FormatTemperature(const ProtoEnvData *data, char *buf, size_t size);
void BME280_FormatHumidity(const ProtoEnvData *data, char *buf, size_t size);
void BME280_FormatPressure(const ProtoEnvData *data, char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif
