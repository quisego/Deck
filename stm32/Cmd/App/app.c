/*
*	Main application for the STM32 board.
*	It starts all modules (logger, link, supervisor, buttons, sensors) and connects them.
*	Sensor data goes to the OLED screen and to the ESP32.
*/

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

/*
*	Task priorities. A bigger number means a more important task.
*/
#define LOGGER_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define LINK_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define SUPERVISOR_TASK_PRIORITY (tskIDLE_PRIORITY + 3U)

/*
*	Pins for the two buttons.
*/
#define BTN_A_PORT GPIOB
#define BTN_A_PIN GPIO_PIN_0
#define BTN_B_PORT GPIOB
#define BTN_B_PIN GPIO_PIN_1

/*
*	The button is "pressed" when the pin is HIGH.
*/
#define BTN_ACTIVE_LEVEL BUTTON_ACTIVE_HIGH
/*
*	Debounce: a real button makes many fast on/off changes when you press it.
*	We ignore changes that are shorter than this time.
*/
#define BTN_DEBOUNCE_MS 20U
/*
*	Max time between two clicks. Also used as the long press time.
*/
#define BTN_CLICK_TIMEOUT_MS 300U

/*
*	How often we read the clock chip (DS1307).
*/
#define RTC_POLL_MS 250U
/*
*	How often we measure temperature, humidity and pressure.
*/
#define ENV_PERIOD_MS 1000U
/*
*	We send new sensor data only when a value changes by at least this step.
*	"X100" means the value is multiplied by 100 (no float numbers needed).
*/
#define ENV_TEMP_STEP_X100 10U  /* 0.1 C  */
#define ENV_HUM_STEP_X100 100U  /* 1 %RH  */
#define ENV_PRESS_STEP_PA 100U  /* 1 hPa  */

/*
*	Global objects. Each one is a module or a device.
*/
static Link g_link;
static Button g_button_a;
static Button g_button_b;
static SSD1306 g_oled;
static DS1307 g_rtc;
static BME280 g_bme;

/*
*	Called by the button module (press, release, click, long press).
*	We encode the event to bytes and send it to the ESP32.
*/
static void on_button_event(uint16_t message_id, const ProtoButtonEvent *event, void *ctx)
{
	(void)ctx;

	uint8_t buf[PROTO_BUTTON_EVENT_SIZE];
	size_t len = 0U;

	// If encoding fails, we log an error and stop.
	if (ProtoMsg_EncodeButtonEvent(event, buf, sizeof(buf), &len) != PROTOCOL_OK)
	{
		LOG_E("Button event encode failed.");
		return;
	}

	LOG_D("Button 0x%x msg 0x%x", (unsigned)event->button_id, (unsigned)message_id);

	// The last argument 0 means "do not wait". If the send queue is full, we drop the message.
	if (Link_Send(&g_link, message_id, buf, (uint16_t)len, 0) != LINK_OK)
	{
		LOG_W("Button event 0x%x dropped.", (unsigned)message_id);
	}
}

/*
*	Create one button. Result holds a value or an error (see result.h).
*/
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

/*
*	Called when the clock gives a new time. We show it on the OLED and send it to the ESP32.
*/
static void on_rtc_event(uint16_t message_id, const ProtoRtcTime *time, void *ctx)
{
	(void)ctx;

	char line[SSD1306_LINE_MAX_CHARS + 1U];

	// Make text for the screen, for example "Mon 10.11.2026".
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

/*
*	ESP32 -> STM32: the ESP32 asks us to set a new time in the clock chip.
*/
static void on_rtc_set_message(const LinkMessage *msg, void *ctx)
{
	(void)ctx;

	ProtoRtcTime time;

	// Bad data -> write a warning and ignore it.
	if (ProtoMsg_DecodeRtcTime(msg->payload, msg->length, &time) != PROTOCOL_OK)
	{
		LOG_W("Bad RTC set payload (len=%u).", (unsigned)msg->length);
		return;
	}

	// This only puts the new time in a queue. The RTC task writes it to the chip later.
	const DS1307_Error err = DS1307_SetTime(&g_rtc, &time);

	if (err != DS1307_ERROR_NONE)
	{
		LOG_W("RTC set rejected: %d", (int)err);
		return;
	}

	LOG_I("RTC set: %04u-%02u-%02u %02u:%02u:%02u", (unsigned)time.year, (unsigned)time.month, (unsigned)time.date,
		(unsigned)time.hours, (unsigned)time.minutes, (unsigned)time.seconds);
}

/*
*	Called when the sensor has new data. Show it on the OLED and send it to the ESP32.
*/
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
/*
*	Status task: every 5 seconds we send "I am alive" information to the ESP32.
*/
#define STATUS_PERIOD_MS 5000U
#define STATUS_TASK_STACK_WORDS 320U

static void status_task(void *arg)
{
	(void)arg;

	for (;;)
	{
		const ProtoSysStatus status = {
			.uptime_ms = HAL_GetTick(),
			// Heap = memory for FreeRTOS. "min" is the lowest free size we ever had.
			.heap_free = (uint32_t)xPortGetFreeHeapSize(),
			.heap_min = (uint32_t)xPortGetMinimumEverFreeHeapSize(),
			// Flags: one bit for each device. The bit is 1 when the device works.
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

		// Sleep. Other tasks can run now.
		vTaskDelay(pdMS_TO_TICKS(STATUS_PERIOD_MS));
	}
}

/*
*	Start the screen, the clock and the sensor. Each module creates its own task.
*	If one device fails, we only log it. The others still work.
*/
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

		// Show "--" until the real data comes.
		(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_DATE, "--- --.--.----");
		(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_TIME, "--:--:--");
		(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_TEMPERATURE, "T: --.- C");
		(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_HUMIDITY, "RH: -- %");
		(void)SSD1306_SetLine(&g_oled, SSD1306_LINE_PRESSURE, "P: ---- hPa");
	}

	// Set the callback BEFORE init, so we do not miss the first event.
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

	// The BME280 sensor uses SPI. Pin PA4 is the chip select (CS).
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

/*
*	Start order: logger first (to see errors), then link, supervisor, buttons, sensors.
*/
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
	// Empty on purpose: without the logger we cannot print the error anyway.
	if (logger_result.is_error)
	{
	}

	LOG_I("Logger is initialized.");

	// Settings for the UART link to the ESP32: queue size, task stacks, timeouts.
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

	// Listen for "set RTC time" messages from the ESP32.
	if (Link_Subscribe(&g_link, (uint16_t)MSG_ID_RTC_SET, on_rtc_set_message, NULL) != LINK_OK)
	{
		LOG_E("Failed to subscribe to RTC set message.");
	}

	// The supervisor checks that tasks are alive. If a task hangs, the MCU resets.
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

/*
*	HAL calls these functions from interrupts. We pass the call to our modules.
*	Keep them short.
*/

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
	Button_EXTI_Callback(GPIO_Pin);
}

/*
*	I2C DMA transfer is finished (used by the OLED).
*/
void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
	SSD1306_I2C_TxCpltCallback(hi2c);
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
	SSD1306_I2C_ErrorCallback(hi2c);
}

/*
*	UART send is finished. Logger and Link both use UART, and each one checks if it is its UART.
*/
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
	Logger_UART_TxCpltCallback(huart);
	Link_UART_TxCpltCallback(huart);
}

/*
*	First half of the RX DMA buffer is full.
*/
void HAL_UART_RxHalfCpltCallback(UART_HandleTypeDef *huart)
{
	Link_UART_RxHalfCpltCallback(huart);
}

/*
*	The whole RX DMA buffer is full (DMA starts again from the beginning).
*/
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	Link_UART_RxCpltCallback(huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
	Logger_UART_ErrorCallback(huart);
	Link_UART_ErrorCallback(huart);
}

/*
*	FreeRTOS calls this when a task stack overflows.
*/
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
	Supervisor_StackOverflowHook(xTask, pcTaskName);
}
