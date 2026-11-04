#include "app.h"
#include "link.h"
#include "logger.h"
#include "projdefs.h"
#include "result.h"
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_uart.h"
#include "supervisor.h"
#include "task.h"
#include "string.h"

#define LOGGER_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#define LINK_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
#define SUPERVISOR_TASK_PRIORITY (tskIDLE_PRIORITY + 3U)

static Link g_link;

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
		// TODO: handle it
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

	const LinkStatus link_status = Link_Init(&g_link, &link_cfg);
	if (link_status != LINK_OK)
	{
		LOG_E("Failed to initialize Link module.");
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
		// TODO: handle it
	}

	LOG_I("Supervisor if initialized.");
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
