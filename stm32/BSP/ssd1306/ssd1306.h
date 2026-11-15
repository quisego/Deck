#ifndef BSP_SSD1306_H
#define BSP_SSD1306_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"
#include <stdbool.h>
#include <stdint.h>
#include "result.h"

#define SSD1306_I2C_ADDR       (0x3C << 1)
#define SSD1306_WIDTH          128
#define SSD1306_HEIGHT         64

#define SSD1306_LINE_COUNT     5U
#define SSD1306_LINE_MAX_CHARS 21U

typedef struct SSD1306 SSD1306;
typedef struct SSD1306_Config SSD1306_Config;
typedef struct SSD1306_Ops SSD1306_Ops;
typedef enum SSD1306_Line SSD1306_Line;
typedef enum SSD1306_Error SSD1306_Error;

enum SSD1306_Line
{
	SSD1306_LINE_DATE = 0,
	SSD1306_LINE_TIME,
	SSD1306_LINE_TEMPERATURE,
	SSD1306_LINE_HUMIDITY,
	SSD1306_LINE_PRESSURE,
};

enum SSD1306_Error
{
	SSD1306_ERROR_NONE = 0,
	SSD1306_ERROR_NULLPTR,
	SSD1306_ERROR_INVALID_CONFIG,
	SSD1306_ERROR_TASK_CREATE_FAILED,
};

struct SSD1306_Config
{
	I2C_HandleTypeDef *hi2c;
	uint8_t i2c_addr;
};

struct SSD1306
{
	SSD1306_Config config;
	const SSD1306_Ops *ops;
};

DEFINE_RESULT(SSD1306, SSD1306_Error);

struct SSD1306_Ops
{
	bool (*set_line)(const SSD1306 *self, SSD1306_Line line, const char *text);
	bool (*is_online)(const SSD1306 *self);
};

Result(SSD1306, SSD1306_Error) SSD1306_Init(const SSD1306_Config *config);

bool SSD1306_SetLine(const SSD1306 *self, SSD1306_Line line, const char *text);
bool SSD1306_IsOnline(const SSD1306 *self);

void SSD1306_I2C_TxCpltCallback(I2C_HandleTypeDef *hi2c);
void SSD1306_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c);

#ifdef __cplusplus
}
#endif

#endif
