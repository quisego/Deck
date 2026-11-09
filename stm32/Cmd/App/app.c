#include "app.h"
#include "bme280.h"
#include "button.h"
#include "button_events.h"
#include "ds1307.h"
#include "link.h"
#include "logger.h"
#include "projdefs.h"
#include "msg.h"
#include "result.h"
#include "ssd1306.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_def.h"
#include "stm32f4xx_hal_i2c.h"
#include "stm32f4xx_hal_uart.h"
#include "supervisor.h"
#include "task.h"

#define LOGGER_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define LINK_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define SUPERVISOR_TASK_PRIORITY (tskIDLE_PRIORITY + 3U)

#define BTN_A_PORT GPIOB
#define BTN_A_PIN GPIO_PIN_0
#define BTN_B_PORT GPIOB
#define BTN_B_PIN GPIO_PIN_1

#define BTN_ACTIVE_LEVEL BUTTON_ACTIVE_HIGH
#define BTN_DEBOUNCE_MS 20U
#define BTN_CLICK_TIMEOUT_MS 300U

#define RTC_POLL_MS 250U
#define ENV_PERIOD_MS 1000U
#define ENV_TEMP_STEP_X100 10U  /* 0.1 C  */
#define ENV_HUM_STEP_X100 100U  /* 1 %RH  */
#define ENV_PRESS_STEP_PA 100U  /* 1 hPa  */

static Link g_link;
static Button g_button_a;
static Button g_button_b;
static SSD1306 g_oled;
static DS1307 g_rtc;
static BME280 g_bme;

/* ------------------------------------------------------------------ */
/* Buttons                                                             */
/* ------------------------------------------------------------------ */

static void on_button_event(uint16_t message_id, const ProtoButtonEvent *event, void *ctx)
{
	(void)ctx;

	uint8_t buf[PROTO_BUTTON_EVENT_SIZE];
	size_t len = 0U;

	if (ProtoMsg_EncodeButtonEvent(event, buf, sizeof(buf), &len) != PROTOCOL_OK)
	{
		LOG_E("Button event encode failed.");
		return;
	}

	LOG_D("Button 0x%x msg 0x%x", (unsigned)event->button_id, (unsigned)message_id);

	if (Link_Send(&g_link, message_id, buf, (uint16_t)len, 0) != LINK_OK)
	{
		LOG_W("Button event 0x%x dropped.", (unsigned)message_id);
	}
}

static bool app_button_init(Button *out, GPIO_TypeDef *port, uint16_t pin)
{
	const Button_Config cfg = {
		.port = port,
		.pin = pin,
		.debounce_ms = BTN_DEBOUNCE_MS,
		.click_timeout_ms = BTN_CLICK_TIMEOUT_MS,
		.active_level = BTN_ACTIVE_LEVEL,
	};

	const Result(Button, Button_Error) result = Button_Init(&cfg);

	if (result.is_error)
	{
		LOG_E("Button 0x%x init failed: %d", (unsigned)Button_IdFromConfig(&cfg), (int)result.error);
		return false;
	}

	*out = result.value;

	LOG_I("Button 0x%x initialized.", (unsigned)Button_IdFromConfig(&cfg));

	return true;
}

/* ------------------------------------------------------------------ */
/* RTC: every second -> OLED + ESP32                                  */
/* ------------------------------------------------------------------ */

static void on_rtc_event(uint16_t message_id, const ProtoRtcTime *time, void *ctx)
{
	(void)ctx;

	char line[SSD1306_LINE_MAX_CHARS + 1U];

	DS1307_DateFormat(time, line, sizeof(line));
	(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_DATE, line);

	DS1307_TimeFormat(time, line, sizeof(line));
	(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_TIME, line);

	uint8_t buf[PROTO_RTC_TIME_SIZE];
	size_t len = 0U;

	if (ProtoMsg_EncodeRtcTime(time, buf, sizeof(buf), &len) != PROTOCOL_OK)
	{
		LOG_E("RTC encode failed.");
		return;
	}

	if (Link_Send(&g_link, message_id, buf, (uint16_t)len, 0) != LINK_OK)
	{
		LOG_W("RTC message dropped.");
	}
}

/* ESP32 -> STM32: DS1307 */
static void on_rtc_set_message(const LinkMessage *msg, void *ctx)
{
	(void)ctx;

	ProtoRtcTime time;

	if (ProtoMsg_DecodeRtcTime(msg->payload, msg->length, &time) != PROTOCOL_OK)
	{
		LOG_W("Bad RTC set payload (len=%u).", (unsigned)msg->length);
		return;
	}

	const DS1307_Error err = DS1307_SetTime(&g_rtc, &time);

	if (err != DS1307_ERROR_NONE)
	{
		LOG_W("RTC set rejected: %d", (int)err);
		return;
	}

	LOG_I("RTC set: %04u-%02u-%02u %02u:%02u:%02u", (unsigned)time.year, (unsigned)time.month, (unsigned)time.date,
		(unsigned)time.hours, (unsigned)time.minutes, (unsigned)time.seconds);
}

/* ------------------------------------------------------------------ */
/* BME280: -> OLED + ESP32                      							        */
/* ------------------------------------------------------------------ */

static void on_env_event(uint16_t message_id, const ProtoEnvData *data, void *ctx)
{
	(void)ctx;

	char line[SSD1306_LINE_MAX_CHARS + 1U];

	BME280_FormatTemperature(data, line, sizeof(line));
	(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_TEMPERATURE, line);

	BME280_FormatHumidity(data, line, sizeof(line));
	(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_HUMIDITY, line);

	BME280_FormatPressure(data, line, sizeof(line));
	(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_PRESSURE, line);

	uint8_t buf[PROTO_ENV_DATA_SIZE];
	size_t len = 0U;

	if (ProtoMsg_EncodeEnvData(data, buf, sizeof(buf), &len) != PROTOCOL_OK)
	{
		LOG_E("ENV encode failed.");
		return;
	}

	if (Link_Send(&g_link, message_id, buf, (uint16_t)len, 0) != LINK_OK)
	{
		LOG_W("ENV message dropped.");
	}
}

/* ------------------------------------------------------------------ */
/* STM32 -> ESP32																					            */
/* ------------------------------------------------------------------ */

#define STATUS_PERIOD_MS 5000U
#define STATUS_TASK_STACK_WORDS 320U

static void status_task(void *arg)
{
	(void)arg;

	for (;;)
	{
		const ProtoSysStatus status = {
			.uptime_ms = HAL_GetTick(),
			.heap_free = (uint32_t)xPortGetFreeHeapSize(),
			.heap_min = (uint32_t)xPortGetMinimumEverFreeHeapSize(),
			.flags = (SSD1306_IsOnline(&g_oled) ? PROTO_STATUS_FLAG_OLED : 0UL)
				| (DS1307_IsOnline(&g_rtc) ? PROTO_STATUS_FLAG_RTC : 0UL)
				| (BME280_IsOnline(&g_bme) ? PROTO_STATUS_FLAG_BME : 0UL),
		};

		uint8_t buf[PROTO_SYS_STATUS_SIZE];
		size_t len = 0U;

		if (ProtoMsg_EncodeSysStatus(&status, buf, sizeof(buf), &len) == PROTOCOL_OK)
		{
			if (Link_Send(&g_link, (uint16_t)MSG_ID_SYS_STATUS, buf, (uint16_t)len, 0) != LINK_OK)
			{
				LOG_W("Status message dropped.");
			}
		}

		vTaskDelay(pdMS_TO_TICKS(STATUS_PERIOD_MS));
	}
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

static void app_sensors_init(const AppConfig *cfg)
{
	const SSD1306_Config oled_cfg = {
		.hi2c = cfg->i2c_h,
		.i2c_addr = SSD1306_I2C_ADDR,
	};

	const Result(SSD1306, SSD1306_Error) oled_result = SSD1306_Init(&oled_cfg);

	if (oled_result.is_error)
	{
		LOG_E("SSD1306 init failed: %d", (int)oled_result.error);
	}
	else
	{
		g_oled = oled_result.value;
		LOG_I("SSD1306 initialized.");

		(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_DATE, "--- --.--.----");
		(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_TIME, "--:--:--");
		(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_TEMPERATURE, "T: --.- C");
		(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_HUMIDITY, "RH: -- %");
		(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_PRESSURE, "P: ---- hPa");
	}

	DS1307_SetEventCallback(on_rtc_event, NULL);

	const DS1307_Config rtc_cfg = {
		.hi2c = cfg->i2c_h,
		.addr = DS1307_I2C_ADDR,
		.poll_ms = RTC_POLL_MS,
	};

	const Result(DS1307, DS1307_Error) rtc_result = DS1307_Init(&rtc_cfg);

	if (rtc_result.is_error)
	{
		LOG_E("DS1307 init failed: %d", (int)rtc_result.error);
	}
	else
	{
		g_rtc = rtc_result.value;
		LOG_I("DS1307 initialized.");
	}

	BME280_SetEventCallback(on_env_event, NULL);

	const BME280_Config bme_cfg = {
		.hspi = cfg->spi_h,
		.cs_port = GPIOA,
		.cs_pin = GPIO_PIN_4,
		.period_ms = ENV_PERIOD_MS,
		.temperature_step_x100 = ENV_TEMP_STEP_X100,
		.humidity_step_x100 = ENV_HUM_STEP_X100,
		.pressure_step_pa = ENV_PRESS_STEP_PA,
	};

	const Result(BME280, BME280_Error) bme_result = BME280_Init(&bme_cfg);

	if (bme_result.is_error)
	{
		LOG_E("BME280 init failed: %d", (int)bme_result.error);
	}
	else
	{
		g_bme = bme_result.value;
		LOG_I("BME280 initialized.");
	}
}

void App_Init(const AppConfig *cfg)
{
	const LoggerConfig logger_cfg = {
		.huart = cfg->logger_huart,
		.min_log_level = LOG_DEBUG,
		.queue_length = 16,
		.max_message_length = 128,
		.task_stack_words = 384,
		.task_priority = LOGGER_TASK_PRIORITY,
	};

	const Result(LoggerError, LoggerError) logger_result = Logger_Init(&logger_cfg);
	if (logger_result.is_error)
	{
	}

	LOG_I("Logger is initialized.");

	const LinkConfig link_cfg = {
		.huart = cfg->link_huart,
		.tx_queue_length = 8,
		.max_payload = 128,
		.tx_task_stack_words = 256,
		.rx_task_stack_words = 256,
		.tx_task_priority = LINK_TASK_PRIORITY,
		.rx_task_priority = LINK_TASK_PRIORITY,
		.tx_timeout_ticks = pdMS_TO_TICKS(100),
		.rx_poll_ticks = pdMS_TO_TICKS(50),
	};

	if (Link_Init(&g_link, &link_cfg) != LINK_OK)
	{
		LOG_E("Failed to initialize Link module.");
		return;
	}

	LOG_I("Link module initialized.");

	if (Link_Subscribe(&g_link, (uint16_t)MSG_ID_RTC_SET, on_rtc_set_message, NULL) != LINK_OK)
	{
		LOG_E("Failed to subscribe to RTC set message.");
	}

	const SupervisorConfig supervisor_cfg = {
		.hiwdg = cfg->supervisor_hiwdg,
		.check_period_ms = 500,
		.stack_min_free_words = 32,
		.task_stack_words = 256,
		.task_priority = SUPERVISOR_TASK_PRIORITY,
	};

	const Result(SupervisorError, SupervisorError) supervisor_result = Supervisor_Init(&supervisor_cfg);
	if (supervisor_result.is_error)
	{
		LOG_E("Failed to initialize Supervisor.");
		return;
	}

	LOG_I("Supervisor is initialized.");

	Button_SetEventCallback(on_button_event, NULL);

	(void)app_button_init(&g_button_a, BTN_A_PORT, BTN_A_PIN);
	(void)app_button_init(&g_button_b, BTN_B_PORT, BTN_B_PIN);

	app_sensors_init(cfg);

	if (xTaskCreate(status_task, "status", STATUS_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1U, NULL) != pdPASS)
	{
		LOG_E("Status task create failed.");
	}
}

/* ------------------------------------------------------------------ */
/* HAL callbacks                                                       */
/* ------------------------------------------------------------------ */

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
	Button_EXTI_Callback(GPIO_Pin);
}

void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
	SSD1306_I2C_TxCpltCallback(hi2c);
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
	SSD1306_I2C_ErrorCallback(hi2c);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
	Logger_UART_TxCpltCallback(huart);
	Link_UART_TxCpltCallback(huart);
}

void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart)
{
	Link_UART_RxHalfCpltCallback(huart);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	Link_UART_RxCpltCallback(huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	Logger_UART_ErrorCallback(huart);
	Link_UART_ErrorCallback(huart);
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	Supervisor_StackOverflowHook(xTask, pcTaskName);
}
