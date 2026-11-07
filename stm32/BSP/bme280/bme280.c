#include "bme280.h"
#include "FreeRTOS.h"
#include "task.h"
#include "supervisor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BME280_TASK_STACK_WORDS   512U
#define BME280_SPI_TIMEOUT_MS     100U
#define BME280_CHIP_ID            0x60U
#define BME280_MEASURE_DELAY_MS   12U
#define BME280_MAX_FAILURES       3U

typedef struct
{
	uint16_t dig_T1;
	int16_t dig_T2, dig_T3;
	uint16_t dig_P1;
	int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
	uint8_t dig_H1;
	int16_t dig_H2;
	uint8_t dig_H3;
	int16_t dig_H4, dig_H5;
	int8_t dig_H6;
} BME280_Calib;

typedef struct
{
	BME280 sensor;
	BME280_Calib calib;
	int32_t t_fine;

	ProtoEnvData last;
	volatile bool has_last;
	bool calibrated;
	uint8_t failures;

	volatile bool in_use;
	volatile SupervisorTaskID supervisor_id;
} BME280_Runtime;

static BME280_Runtime s_rt;
static BME280_EventCallback s_event_cb;
static void *s_event_ctx;

static void cs_low(const BME280_Config *c)
{
	HAL_GPIO_WritePin(c->cs_port, c->cs_pin, GPIO_PIN_RESET);
}

static void cs_high(const BME280_Config *c)
{
	HAL_GPIO_WritePin(c->cs_port, c->cs_pin, GPIO_PIN_SET);
}

static HAL_StatusTypeDef read_regs(const BME280_Config *c, uint8_t reg, uint8_t *out, uint8_t len)
{
	uint8_t tx[27] = { 0 };
	uint8_t rx[27];

	if (len > 26U)
	{
		return HAL_ERROR;
	}

	tx[0] = (uint8_t)(reg | 0x80U);

	cs_low(c);
	const HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(c->hspi, tx, rx, (uint16_t)(len + 1U), BME280_SPI_TIMEOUT_MS);
	cs_high(c);

	if (status == HAL_OK)
	{
		memcpy(out, &rx[1], len);
	}

	return status;
}

static HAL_StatusTypeDef write_reg(const BME280_Config *c, uint8_t reg, uint8_t value)
{
	uint8_t tx[2] = { (uint8_t)(reg & 0x7FU), value };

	cs_low(c);
	const HAL_StatusTypeDef status = HAL_SPI_Transmit(c->hspi, tx, 2U, BME280_SPI_TIMEOUT_MS);
	cs_high(c);

	return status;
}

static uint16_t u16_le(const uint8_t *b)
{
	return (uint16_t)((uint16_t)b[0] | (uint16_t)((uint16_t)b[1] << 8));
}

static int32_t compensate_temperature(BME280_Runtime *rt, int32_t adc_t)
{
	const BME280_Calib *c = &rt->calib;
	const int32_t t1 = (int32_t)c->dig_T1;
	const int32_t var1 = (((adc_t >> 3) - (t1 << 1)) * (int32_t)c->dig_T2) >> 11;
	const int32_t d = (adc_t >> 4) - t1;
	const int32_t var2 = (((d * d) >> 12) * (int32_t)c->dig_T3) >> 14;

	rt->t_fine = var1 + var2;

	return (rt->t_fine * 5 + 128) >> 8;
}

static uint32_t compensate_pressure(const BME280_Runtime *rt, int32_t adc_p)
{
	const BME280_Calib *c = &rt->calib;
	int32_t var1 = (rt->t_fine >> 1) - 64000;
	int32_t var2 = (((var1 >> 2) * (var1 >> 2)) >> 11) * (int32_t)c->dig_P6;

	var2 = var2 + ((var1 * (int32_t)c->dig_P5) << 1);
	var2 = (var2 >> 2) + ((int32_t)c->dig_P4 << 16);
	var1 = ((((int32_t)c->dig_P3 * (((var1 >> 2) * (var1 >> 2)) >> 13)) >> 3) + (((int32_t)c->dig_P2 * var1) >> 1)) >> 18;
	var1 = ((32768 + var1) * (int32_t)c->dig_P1) >> 15;

	if (var1 == 0)
	{
		return 0U;
	}

	uint32_t p = (((uint32_t)(1048576 - adc_p)) - (uint32_t)(var2 >> 12)) * 3125U;

	if (p < 0x80000000U)
	{
		p = (p << 1) / (uint32_t)var1;
	}
	else
	{
		p = (p / (uint32_t)var1) * 2U;
	}

	var1 = ((int32_t)c->dig_P9 * (int32_t)(((p >> 3) * (p >> 3)) >> 13)) >> 12;
	var2 = ((int32_t)(p >> 2) * (int32_t)c->dig_P8) >> 13;
	p = (uint32_t)((int32_t)p + ((var1 + var2 + (int32_t)c->dig_P7) >> 4));

	return p;
}

static uint16_t compensate_humidity(const BME280_Runtime *rt, int32_t adc_h)
{
	const BME280_Calib *c = &rt->calib;
	int32_t v = rt->t_fine - 76800;

	v = (((((adc_h << 14) - ((int32_t)c->dig_H4 << 20) - ((int32_t)c->dig_H5 * v)) + 16384) >> 15)
		* (((((((v * (int32_t)c->dig_H6) >> 10) * (((v * (int32_t)c->dig_H3) >> 11) + 32768)) >> 10) + 2097152)
		* (int32_t)c->dig_H2 + 8192) >> 14));
	v = v - (((((v >> 15) * (v >> 15)) >> 7) * (int32_t)c->dig_H1) >> 4);

	if (v < 0)
	{
		v = 0;
	}
	if (v > 419430400)
	{
		v = 419430400;
	}

	const uint32_t q22_10 = (uint32_t)(v >> 12); /* %RH * 1024 */

	return (uint16_t)((q22_10 * 100U) / 1024U);
}

static bool sensor_init(BME280_Runtime *rt)
{
	const BME280_Config *cfg = &rt->sensor.config;
	uint8_t id = 0U;
	uint8_t buf_tp[26];
	uint8_t buf_h[7];

	if (read_regs(cfg, BME280_REG_ID, &id, 1U) != HAL_OK || id != BME280_CHIP_ID)
	{
		return false;
	}

	(void)write_reg(cfg, 0xE0, 0xB6); /* soft reset */
	vTaskDelay(pdMS_TO_TICKS(10));

	if (read_regs(cfg, 0x88, buf_tp, sizeof(buf_tp)) != HAL_OK || read_regs(cfg, 0xE1, buf_h, sizeof(buf_h)) != HAL_OK)
	{
		return false;
	}

	BME280_Calib *c = &rt->calib;

	c->dig_T1 = u16_le(&buf_tp[0]);
	c->dig_T2 = (int16_t)u16_le(&buf_tp[2]);
	c->dig_T3 = (int16_t)u16_le(&buf_tp[4]);
	c->dig_P1 = u16_le(&buf_tp[6]);
	c->dig_P2 = (int16_t)u16_le(&buf_tp[8]);
	c->dig_P3 = (int16_t)u16_le(&buf_tp[10]);
	c->dig_P4 = (int16_t)u16_le(&buf_tp[12]);
	c->dig_P5 = (int16_t)u16_le(&buf_tp[14]);
	c->dig_P6 = (int16_t)u16_le(&buf_tp[16]);
	c->dig_P7 = (int16_t)u16_le(&buf_tp[18]);
	c->dig_P8 = (int16_t)u16_le(&buf_tp[20]);
	c->dig_P9 = (int16_t)u16_le(&buf_tp[22]);
	c->dig_H1 = buf_tp[25];
	c->dig_H2 = (int16_t)u16_le(&buf_h[0]);
	c->dig_H3 = buf_h[2];
	c->dig_H4 = (int16_t)(((int32_t)(int8_t)buf_h[3] * 16) | (int32_t)(buf_h[4] & 0x0FU));
	c->dig_H5 = (int16_t)(((int32_t)(int8_t)buf_h[5] * 16) | (int32_t)(buf_h[4] >> 4));
	c->dig_H6 = (int8_t)buf_h[6];

	return true;
}

static bool measure(BME280_Runtime *rt, ProtoEnvData *out)
{
	const BME280_Config *cfg = &rt->sensor.config;
	uint8_t raw[8];

	/* forced mode: osrs_h x1, osrs_t x1, osrs_p x1 */
	if (write_reg(cfg, 0xF2, 0x01) != HAL_OK || write_reg(cfg, 0xF4, 0x25) != HAL_OK)
	{
		return false;
	}

	vTaskDelay(pdMS_TO_TICKS(BME280_MEASURE_DELAY_MS));

	if (read_regs(cfg, 0xF7, raw, sizeof(raw)) != HAL_OK)
	{
		return false;
	}

	const int32_t adc_p = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) | (int32_t)(raw[2] >> 4);
	const int32_t adc_t = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) | (int32_t)(raw[5] >> 4);
	const int32_t adc_h = ((int32_t)raw[6] << 8) | (int32_t)raw[7];

	out->temperature_c_x100 = (int16_t)compensate_temperature(rt, adc_t); /* спочатку T: оновлює t_fine */
	out->pressure_pa = compensate_pressure(rt, adc_p);
	out->humidity_pct_x100 = compensate_humidity(rt, adc_h);

	return true;
}

static bool changed_enough(const BME280_Config *c, const ProtoEnvData *a, const ProtoEnvData *b)
{
	const int32_t dt = abs((int32_t)a->temperature_c_x100 - (int32_t)b->temperature_c_x100);
	const int32_t dh = abs((int32_t)a->humidity_pct_x100 - (int32_t)b->humidity_pct_x100);
	const int32_t dp = abs((int32_t)a->pressure_pa - (int32_t)b->pressure_pa);

	return dt >= (int32_t)c->temperature_step_x100
		|| dh >= (int32_t)c->humidity_step_x100
		|| dp >= (int32_t)c->pressure_step_pa;
}

void BME280_SetEventCallback(BME280_EventCallback callback, void *ctx)
{
	s_event_cb = callback;
	s_event_ctx = ctx;
}

static void bme280_task(void *arg)
{
	BME280_Runtime *rt = (BME280_Runtime *)arg;
	const BME280_Config *cfg = &rt->sensor.config;

	TickType_t period = pdMS_TO_TICKS(cfg->period_ms);
	if (period == 0U)
	{
		period = 1U;
	}

	for (;;)
	{
		if (!rt->calibrated)
		{
			rt->calibrated = sensor_init(rt);
		}

		if (rt->calibrated)
		{
			ProtoEnvData now;

			if (measure(rt, &now))
			{
				rt->failures = 0U;

				if (!rt->has_last || changed_enough(cfg, &now, &rt->last))
				{
					taskENTER_CRITICAL();
					rt->last = now;
					rt->has_last = true;
					taskEXIT_CRITICAL();

					if (s_event_cb != NULL)
					{
						s_event_cb((uint16_t)MSG_ID_ENV_DATA, &now, s_event_ctx);
					}
				}
			}
			else if (++rt->failures >= BME280_MAX_FAILURES)
			{
				rt->calibrated = false;
				rt->failures = 0U;
			}
		}

		if (rt->supervisor_id != SUPERVISOR_INVALID_ID)
		{
			Supervisor_CheckIn(rt->supervisor_id);
		}

		vTaskDelay(period);
	}
}

static const BME280_Ops bme280_ops = {
	.get_last = BME280_GetLast,
};

Result(BME280, BME280_Error) BME280_Init(const BME280_Config *config)
{
	if (config == NULL)
	{
		return RESULT_ERROR(BME280, BME280_Error, BME280_ERROR_NULLPTR);
	}

	if (config->hspi == NULL || config->cs_port == NULL || config->cs_pin == 0U || config->period_ms == 0U)
	{
		return RESULT_ERROR(BME280, BME280_Error, BME280_ERROR_INVALID_CONFIG);
	}

	if (s_rt.in_use)
	{
		return RESULT_OK(BME280, BME280_Error, s_rt.sensor);
	}

	memset(&s_rt, 0, sizeof(s_rt));

	s_rt.sensor.config = *config;
	s_rt.sensor.ops = &bme280_ops;
	s_rt.supervisor_id = SUPERVISOR_INVALID_ID;

	cs_high(config);

	s_rt.in_use = true;

	TaskHandle_t handle = NULL;

	if (xTaskCreate(bme280_task, "bme280", BME280_TASK_STACK_WORDS, &s_rt, tskIDLE_PRIORITY + 1U, &handle) != pdPASS)
	{
		s_rt.in_use = false;
		return RESULT_ERROR(BME280, BME280_Error, BME280_ERROR_TASK_CREATE_FAILED);
	}

	s_rt.supervisor_id = Supervisor_Register(handle, config->period_ms * 2U + 2000U);

	return RESULT_OK(BME280, BME280_Error, s_rt.sensor);
}

bool BME280_GetLast(const BME280 *self, ProtoEnvData *out)
{
	(void)self;

	if (!s_rt.in_use || out == NULL || !s_rt.has_last)
	{
		return false;
	}

	taskENTER_CRITICAL();
	*out = s_rt.last;
	taskEXIT_CRITICAL();

	return true;
}

void BME280_FormatTemperature(const ProtoEnvData *data, char *buf, size_t size)
{
	const int32_t t = data->temperature_c_x100;
	const int32_t t10 = (t >= 0) ? ((t + 5) / 10) : ((t - 5) / 10);
	const int32_t a = abs(t10);

	(void)snprintf(buf, size, "T: %s%d.%d C", (t10 < 0) ? "-" : "", (int)(a / 10), (int)(a % 10));
}

void BME280_FormatHumidity(const ProtoEnvData *data, char *buf, size_t size)
{
	(void)snprintf(buf, size, "RH: %u %%", (unsigned)((data->humidity_pct_x100 + 50U) / 100U));
}

void BME280_FormatPressure(const ProtoEnvData *data, char *buf, size_t size)
{
	(void)snprintf(buf, size, "P: %lu hPa", (unsigned long)((data->pressure_pa + 50U) / 100U));
}
