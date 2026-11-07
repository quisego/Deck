#include "ssd1306.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "i2c_bus.h"
#include "supervisor.h"
#include "u8g2.h"
#include <string.h>

#define SSD1306_TASK_STACK_WORDS        512U
#define SSD1306_MAX_WAIT_MS             500U
#define SSD1306_RETRY_MS                1000U
#define SSD1306_SUPERVISOR_TIMEOUT_MS   3000U
#define SSD1306_I2C_LOCK_TIMEOUT_MS     200U
#define SSD1306_DMA_TIMEOUT_MS          100U
#define SSD1306_LINE_STEP_PX            12U

typedef struct
{
	SSD1306 display;
	u8g2_t u8g2;

	uint8_t buffer[32];
	uint8_t buf_idx;

	char lines[SSD1306_LINE_COUNT][SSD1306_LINE_MAX_CHARS + 1U];

	SemaphoreHandle_t done;
	TaskHandle_t task;

	volatile bool in_use;
	volatile bool online;
	volatile bool dirty;
	volatile bool tx_error;
	bool tx_failed;
	volatile SupervisorTaskID supervisor_id;
} SSD1306_Runtime;

static SSD1306_Runtime s_rt;

static void delay_ms(uint32_t ms)
{
	TickType_t ticks = pdMS_TO_TICKS(ms);

	if (ms > 0U && ticks == 0U)
	{
		ticks = 1U;
	}

	vTaskDelay(ticks);
}

static bool i2c_send_dma(SSD1306_Runtime *rt)
{
	I2C_HandleTypeDef *h = rt->display.config.hi2c;
	bool ok = false;

	if (!I2C_Bus_Lock(pdMS_TO_TICKS(SSD1306_I2C_LOCK_TIMEOUT_MS)))
	{
		return false;
	}

	(void)xSemaphoreTake(rt->done, 0);
	rt->tx_error = false;

	if (HAL_I2C_Master_Transmit_DMA(h, rt->display.config.i2c_addr, rt->buffer, rt->buf_idx) == HAL_OK)
	{
		if (xSemaphoreTake(rt->done, pdMS_TO_TICKS(SSD1306_DMA_TIMEOUT_MS)) == pdTRUE)
		{
			ok = !rt->tx_error;
		}
		else
		{
			(void)HAL_I2C_DeInit(h);
			(void)HAL_I2C_Init(h);
		}
	}

	I2C_Bus_Unlock();

	return ok;
}

static uint8_t u8x8_byte_hw_i2c_dma(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
	(void)u8x8;
	SSD1306_Runtime *rt = &s_rt;

	switch (msg)
	{
	case U8X8_MSG_BYTE_SEND:
	{
		const uint8_t *data = (const uint8_t *)arg_ptr;

		while (arg_int > 0U)
		{
			if (rt->buf_idx < sizeof(rt->buffer))
			{
				rt->buffer[rt->buf_idx++] = *data;
			}

			data++;
			arg_int--;
		}
		break;
	}
	case U8X8_MSG_BYTE_INIT:
	case U8X8_MSG_BYTE_SET_DC:
		break;
	case U8X8_MSG_BYTE_START_TRANSFER:
		rt->buf_idx = 0U;
		break;
	case U8X8_MSG_BYTE_END_TRANSFER:
		if (rt->buf_idx > 0U && !rt->tx_failed)
		{
			if (!i2c_send_dma(rt))
			{
				rt->tx_failed = true;
			}
		}
		break;
	default:
		return 0;
	}

	return 1;
}

static uint8_t u8x8_gpio_and_delay(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr)
{
	(void)u8x8;
	(void)arg_ptr;

	if (msg == U8X8_MSG_DELAY_MILLI)
	{
		delay_ms(arg_int);
	}

	return 1;
}

void SSD1306_I2C_TxCpltCallback(I2C_HandleTypeDef *hi2c)
{
	if (!s_rt.in_use || hi2c != s_rt.display.config.hi2c)
	{
		return;
	}

	BaseType_t woken = pdFALSE;

	(void)xSemaphoreGiveFromISR(s_rt.done, &woken);
	portYIELD_FROM_ISR(woken);
}

void SSD1306_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
	if (!s_rt.in_use || hi2c != s_rt.display.config.hi2c)
	{
		return;
	}

	BaseType_t woken = pdFALSE;

	s_rt.tx_error = true;
	(void)xSemaphoreGiveFromISR(s_rt.done, &woken);
	portYIELD_FROM_ISR(woken);
}

static bool display_hw_init(SSD1306_Runtime *rt)
{
	const SSD1306_Config *cfg = &rt->display.config;
	bool ready = false;

	if (I2C_Bus_Lock(pdMS_TO_TICKS(SSD1306_I2C_LOCK_TIMEOUT_MS)))
	{
		ready = (HAL_I2C_IsDeviceReady(cfg->hi2c, cfg->i2c_addr, 3U, 100U) == HAL_OK);
		I2C_Bus_Unlock();
	}

	if (!ready)
	{
		return false;
	}

	rt->tx_failed = false;
	rt->buf_idx = 0U;

	u8g2_Setup_ssd1306_i2c_128x64_noname_f(&rt->u8g2, U8G2_R0, u8x8_byte_hw_i2c_dma, u8x8_gpio_and_delay);
	u8g2_InitDisplay(&rt->u8g2);
	u8g2_SetPowerSave(&rt->u8g2, 0);
	u8g2_ClearBuffer(&rt->u8g2);
	u8g2_SetFont(&rt->u8g2, u8g2_font_6x12_tr);

	return !rt->tx_failed;
}

static void render(SSD1306_Runtime *rt)
{
	char local[SSD1306_LINE_COUNT][SSD1306_LINE_MAX_CHARS + 1U];

	taskENTER_CRITICAL();
	memcpy(local, rt->lines, sizeof(local));
	rt->dirty = false;
	taskEXIT_CRITICAL();

	rt->tx_failed = false;

	u8g2_ClearBuffer(&rt->u8g2);

	for (uint8_t i = 0U; i < SSD1306_LINE_COUNT; i++)
	{
		u8g2_DrawStr(&rt->u8g2, 0, (u8g2_uint_t)((i + 1U) * SSD1306_LINE_STEP_PX), local[i]);
	}

	u8g2_SendBuffer(&rt->u8g2);

	if (rt->tx_failed)
	{
		rt->online = false;
		rt->dirty = true;
	}
}

static void display_task(void *arg)
{
	SSD1306_Runtime *rt = (SSD1306_Runtime *)arg;

	for (;;)
	{
		if (!rt->online && display_hw_init(rt))
		{
			rt->online = true;
			rt->dirty = true;
		}

		if (rt->online && rt->dirty)
		{
			render(rt);
		}

		if (rt->supervisor_id != SUPERVISOR_INVALID_ID)
		{
			Supervisor_CheckIn(rt->supervisor_id);
		}

		const uint32_t wait_ms = rt->online ? SSD1306_MAX_WAIT_MS : SSD1306_RETRY_MS;

		(void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
	}
}

static const SSD1306_Ops ssd1306_ops = {
	.set_line = SSD1306_SetLine,
	.is_online = SSD1306_IsOnline,
};

Result(SSD1306, SSD1306_Error) SSD1306_Init(const SSD1306_Config *config)
{
	if (config == NULL)
	{
		return RESULT_ERROR(SSD1306, SSD1306_Error, SSD1306_ERROR_NULLPTR);
	}

	if (config->hi2c == NULL)
	{
		return RESULT_ERROR(SSD1306, SSD1306_Error, SSD1306_ERROR_INVALID_CONFIG);
	}

	if (s_rt.in_use)
	{
		return RESULT_OK(SSD1306, SSD1306_Error, s_rt.display);
	}

	memset(&s_rt, 0, sizeof(s_rt));

	s_rt.display.config = *config;
	s_rt.display.ops = &ssd1306_ops;
	s_rt.supervisor_id = SUPERVISOR_INVALID_ID;

	I2C_Bus_Init();

	s_rt.done = xSemaphoreCreateBinary();
	if (s_rt.done == NULL)
	{
		return RESULT_ERROR(SSD1306, SSD1306_Error, SSD1306_ERROR_TASK_CREATE_FAILED);
	}

	s_rt.in_use = true;

	if (xTaskCreate(display_task, "oled", SSD1306_TASK_STACK_WORDS, &s_rt, tskIDLE_PRIORITY + 1U, &s_rt.task) != pdPASS)
	{
		s_rt.in_use = false;
		vSemaphoreDelete(s_rt.done);
		s_rt.done = NULL;
		return RESULT_ERROR(SSD1306, SSD1306_Error, SSD1306_ERROR_TASK_CREATE_FAILED);
	}

	s_rt.supervisor_id = Supervisor_Register(s_rt.task, SSD1306_SUPERVISOR_TIMEOUT_MS);

	return RESULT_OK(SSD1306, SSD1306_Error, s_rt.display);
}

bool SSD1306_SetLine(const SSD1306 *self, SSD1306_Line line, const char *text)
{
	(void)self;

	if (!s_rt.in_use || text == NULL || (unsigned)line >= SSD1306_LINE_COUNT)
	{
		return false;
	}

	char tmp[SSD1306_LINE_MAX_CHARS + 1U];

	strncpy(tmp, text, SSD1306_LINE_MAX_CHARS);
	tmp[SSD1306_LINE_MAX_CHARS] = '\0';

	bool changed = false;

	taskENTER_CRITICAL();
	if (strcmp(s_rt.lines[line], tmp) != 0)
	{
		memcpy(s_rt.lines[line], tmp, sizeof(tmp));
		s_rt.dirty = true;
		changed = true;
	}
	taskEXIT_CRITICAL();

	if (changed && s_rt.task != NULL)
	{
		xTaskNotifyGive(s_rt.task);
	}

	return true;
}

bool SSD1306_IsOnline(const SSD1306 *self)
{
	(void)self;

	return s_rt.in_use && s_rt.online;
}
