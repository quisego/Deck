#include "supervisor.h"
#include <stdatomic.h>
#include "logger.h"

typedef struct {
	TaskHandle_t 				handle;
	TickType_t 					timeout_ticks;
	volatile TickType_t last_checkin;
	UBaseType_t 				stack_reported_min;
	atomic_bool 				is_active;
} MonitoredTask;

static MonitoredTask 			s_tasks[SUPERVISOR_MAX_TASKS];
static atomic_uint 				s_count;
static IWDG_HandleTypeDef *s_hiwdg;
static TickType_t 				s_period_ticks;
static UBaseType_t 				s_stack_min_free;
static volatile bool 			s_inited;

#define SUPERVISOR_FAIL(err) RESULT_ERROR(SupervisorError, SupervisorError, (err))

static uint32_t registered_count(void)
{
	const uint32_t n = atomic_load(&s_count);
	
	return (n < SUPERVISOR_MAX_TASKS) ? n : SUPERVISOR_MAX_TASKS;
}

static void log_reset_reason(void)
{
	if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST))
	{
		LOG_W("supervisor: reset reason - IWDG (watchdog)");
	}
	else if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST))
	{
		LOG_W("supervisor: reset reason - WWDG");
	}
	else if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))
	{
		LOG_W("supervisor: reset reason - software reset");
	}
	else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST))
	{
		LOG_I("supervisor: reset reason - power on");
	}
	else if (__HAL_RCC_GET_FLAG(RCC_FLAG_BORRST))
	{
		LOG_W("supervisor: reset reason - brown out");
	}
	else if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))
	{
		LOG_I("supervisor: reset reason - reset pin");
	}

	__HAL_RCC_CLEAR_RESET_FLAGS();
}

static bool check_tasks(void)
{
	bool is_all_ok = true;
	const uint32_t n = registered_count();

	for (uint32_t i = 0; i < n; i++)
	{
		MonitoredTask *t = &s_tasks[i];

		if (!atomic_load_explicit(&t->is_active, memory_order_acquire))
		{
			continue;
		}

		const TickType_t last = t->last_checkin;
		const TickType_t age = xTaskGetTickCount() - last;

		if (age > t->timeout_ticks)
		{
			is_all_ok = false;

			LOG_E("supervisor: task '%s' silent for %lu ms", pcTaskGetName(t->handle), (unsigned long)(age * portTICK_PERIOD_MS));
		}
	}

	return is_all_ok;
}

static void check_stacks(void)
{
	const uint32_t n = registered_count();

	for (uint32_t i = 0; i< n; i++)
	{
		MonitoredTask *t = &s_tasks[i];

		if (!atomic_load_explicit(&t->is_active, memory_order_acquire))
		{
			continue;
		}

		const UBaseType_t free_words = uxTaskGetStackHighWaterMark(t->handle);

		if ((free_words < s_stack_min_free) && (free_words < t->stack_reported_min))
		{
			t->stack_reported_min = free_words;

			LOG_W("supervisor: task '%s' stack low, %lu words free", pcTaskGetName(t->handle), (unsigned long)free_words);
		}
	}
}

static void supervisor_step(void)
{
	const bool tasks_ok = check_tasks();
	check_stacks();

	if (tasks_ok)
	{
		HAL_IWDG_Refresh(s_hiwdg);
	}
	else
	{
		LOG_E("supervisor: IWDG not refreshed, reset expected");
	}
}

static void supervisor_task(void *arg)
{
	TickType_t wake = xTaskGetTickCount();

	while (true)
	{
		vTaskDelayUntil(&wake, s_period_ticks);

		supervisor_step();
	}
}

Result(SupervisorError, SupervisorError) Supervisor_Init(const SupervisorConfig *cfg)
{
	if (s_inited)
	{
		return SUPERVISOR_FAIL(SUPERVISOR_ERROR_ALREADY_INITIALIZED);
	}

	if (
		(cfg == NULL)
		|| (cfg->hiwdg == NULL)
		|| (cfg->check_period_ms == 0U)
		|| (pdMS_TO_TICKS(cfg->check_period_ms) == 0U)
		|| (cfg->task_stack_words == 0U)
	) {
		return SUPERVISOR_FAIL(SUPERVISOR_ERROR_INVALID_CONFIG);
	}

	s_hiwdg = cfg->hiwdg;
	s_period_ticks = pdMS_TO_TICKS(cfg->check_period_ms);
	s_stack_min_free = cfg->stack_min_free_words;

	log_reset_reason();
	HAL_IWDG_Refresh(s_hiwdg);

	if (xTaskCreate(supervisor_task, "Supervisor", cfg->task_stack_words, NULL, cfg->task_priority, NULL) != pdPASS)
	{
		return SUPERVISOR_FAIL(SUPERVISOR_ERROR_TASK_CREATE_FAILED);
	}

	s_inited = true;

	return RESULT_OK(SupervisorError, SupervisorError, SUPERVISOR_ERROR_NONE);
}

SupervisorTaskID Supervisor_Register(TaskHandle_t task, uint32_t timeout_ms)
{
	if (task == NULL)
	{
		task = xTaskGetCurrentTaskHandle();
	}

	if ((task == NULL) || (timeout_ms = 0U))
	{
		return SUPERVISOR_INVALID_ID;
	}

	const uint32_t id = atomic_fetch_add(&s_count, 1U);
	if (id >= SUPERVISOR_MAX_TASKS)
	{
		return SUPERVISOR_INVALID_ID;
	}

	MonitoredTask *t = &s_tasks[id];
	t->handle = task;
	t->timeout_ticks = pdMS_TO_TICKS(timeout_ms);
	t->last_checkin = xTaskGetTickCount();
	t->stack_reported_min = (UBaseType_t)-1;
	atomic_store_explicit(&t->is_active, true, memory_order_release);

	return (SupervisorTaskID)id;
}

void Supervisor_CheckIn(SupervisorTaskID id)
{
	if (id < SUPERVISOR_MAX_TASKS)
	{
		s_tasks[id].last_checkin = xTaskGetTickCount();
	}
}

void Supervisor_StackOverflowHook(TaskHandle_t task, char *task_name)
{
	__disable_irq();
	NVIC_SystemReset();
}
