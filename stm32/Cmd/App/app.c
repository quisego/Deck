#include "app.h"
#include "button.h"
#include "button_events.h"
#include "link.h"
#include "logger.h"
#include "projdefs.h"
#include "msg.h"
#include "result.h"
#include "stm32f4xx_hal.h"
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

static Link g_link;
static Button g_button_a;
static Button g_button_b;

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

	if (!app_button_init(&g_button_a, BTN_A_PORT, BTN_A_PIN))
	{
		return;
	}

	if (!app_button_init(&g_button_b, BTN_B_PORT, BTN_B_PIN))
	{
		return;
	}
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
	Button_EXTI_Callback(GPIO_Pin);
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
