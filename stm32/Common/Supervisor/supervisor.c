/*
*	Supervisor = a software watchdog helper.
*	Each important task registers here and calls Supervisor_CheckIn() from time to time ("I am alive").
*	The supervisor task checks all of them. If one task is silent for too long,
*	we stop feeding the hardware watchdog (IWDG), and the MCU resets.
*/

#include "supervisor.h"
#include <stdatomic.h>
#include <stdint.h>
#include "logger.h"
#include "portmacro.h"

/*
*	Data for one monitored task.
*	last_checkin = the last time the task said "I am alive".
*	stack_reported_min = lowest free stack we already reported (to not repeat logs).
*/
typedef struct {
	TaskHandle_t 				handle;
	TickType_t 					timeout_ticks;
	volatile TickType_t last_checkin;
	UBaseType_t 				stack_reported_min;
	atomic_bool 				is_active;
} MonitoredTask;

/*
*	Global state of the supervisor.
*/
static MonitoredTask 			s_tasks[SUPERVISOR_MAX_TASKS];
static atomic_uint 				s_count;
static IWDG_HandleTypeDef *s_hiwdg;
static TickType_t 				s_period_ticks;
static UBaseType_t 				s_stack_min_free;
static volatile bool 			s_inited;

#define SUPERVISOR_FAIL(err) RESULT_ERROR(SupervisorError, SupervisorError, (err))

/*
*	s_count can be bigger than the array size (when too many tasks try to register).
*	So we limit it to the array size.
*/
static uint32_t registered_count(void)
{
	const uint32_t n = atomic_load(&s_count);
	
	return (n < SUPERVISOR_MAX_TASKS) ? n : SUPERVISOR_MAX_TASKS;
}

/*
*	Print why the MCU restarted (watchdog, power on, reset pin...). Then clear the flags.
*/
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

/*
*	Check that the watchdog time is long enough for our check period.
*/
static void check_iwdg_timeout(uint32_t period_ms)
{
	// The internal clock of the watchdog is not exact, so we use the worst (slowest) case.
	const uint32_t divider = 4U << s_hiwdg->Init.Prescaler;
	const uint32_t min_ms = (uint32_t)(((uint64_t)(s_hiwdg->Init.Reload + 1U) * divider * 1000U) / 47000U);

	if (min_ms < (2U * period_ms))
	{
		LOG_E("supervisor: IWDG timeout is only %lu ms (worst case), need >= %lu ms. Raise the IWDG prescaler in CubeMX", (unsigned long)min_ms, (unsigned long)(2U * period_ms));
	}
}

/*
*	Look at all tasks. Return true only if all of them checked in on time.
*/
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
		// How long the task was silent. Unsigned math works even when the tick counter wraps around.
		const TickType_t age = xTaskGetTickCount() - last;

		if (age > t->timeout_ticks)
		{
			is_all_ok = false;

			LOG_E("supervisor: task '%s' silent for %lu ms (limit %lu ms)", pcTaskGetName(t->handle), (unsigned long)(age * portTICK_PERIOD_MS), (unsigned long)(t->timeout_ticks * portTICK_PERIOD_MS));
		}
	}

	return is_all_ok;
}

/*
*	Warn if a task has very little free stack left.
*/
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

		// "High water mark" = the smallest free stack size that the task ever had.
		const UBaseType_t free_words = uxTaskGetStackHighWaterMark(t->handle);

		if ((free_words < s_stack_min_free) && (free_words < t->stack_reported_min))
		{
			t->stack_reported_min = free_words;

			LOG_W("supervisor: task '%s' stack low, %lu words free", pcTaskGetName(t->handle), (unsigned long)free_words);
		}
	}
}

/*
*	One check cycle: check tasks and stacks, then feed the watchdog only if all is OK.
*/
static void supervisor_step(void)
{
	const bool tasks_ok = check_tasks();
	check_stacks();

	if (tasks_ok)
	{
		// Feed the watchdog. If we do not do this in time, the MCU resets.
		HAL_IWDG_Refresh(s_hiwdg);
	}
	else
	{
		LOG_E("supervisor: IWDG not refreshed, reset expected");
	}
}

/*
*	vTaskDelayUntil gives a stable period (it does not drift).
*/
static void supervisor_task(void *arg)
{
	TickType_t wake = xTaskGetTickCount();

	while (true)
	{
		vTaskDelayUntil(&wake, s_period_ticks);

		supervisor_step();
	}
}

/*
*	Start the supervisor. Call this one time.
*/
Result(SupervisorError, SupervisorError) Supervisor_Init(const SupervisorConfig *cfg)
{
	// First: do not start twice. Then: check the config values.
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
	check_iwdg_timeout(cfg->check_period_ms);
	// Feed the watchdog once, so the supervisor task has time to start.
	HAL_IWDG_Refresh(s_hiwdg);

	if (xTaskCreate(supervisor_task, "Supervisor", cfg->task_stack_words, NULL, cfg->task_priority, NULL) != pdPASS)
	{
		return SUPERVISOR_FAIL(SUPERVISOR_ERROR_TASK_CREATE_FAILED);
	}

	s_inited = true;

	return RESULT_OK(SupervisorError, SupervisorError, SUPERVISOR_ERROR_NONE);
}

/*
*	A task calls this to join the monitoring. timeout_ms = max silence time.
*	NULL as task means "the current task".
*/
SupervisorTaskID Supervisor_Register(TaskHandle_t task, uint32_t timeout_ms)
{
	if (task == NULL)
	{
		task = xTaskGetCurrentTaskHandle();
	}

	if ((task == NULL) || (timeout_ms == 0U))
	{
		return SUPERVISOR_INVALID_ID;
	}

	// Atomic add: two tasks can never get the same id.
	const uint32_t id = atomic_fetch_add(&s_count, 1U);
	if (id >= SUPERVISOR_MAX_TASKS)
	{
		return SUPERVISOR_INVALID_ID;
	}

	// Fill all fields first...
	MonitoredTask *t = &s_tasks[id];
	t->handle = task;
	t->timeout_ticks = pdMS_TO_TICKS(timeout_ms);
	t->last_checkin = xTaskGetTickCount();
	t->stack_reported_min = (UBaseType_t)-1;
	// ...and set is_active LAST. So the checker never sees half-ready data.
	atomic_store_explicit(&t->is_active, true, memory_order_release);

	return (SupervisorTaskID)id;
}

/*
*	Tasks call this to say "I am alive".
*/
void Supervisor_CheckIn(SupervisorTaskID id)
{
	if (id < SUPERVISOR_MAX_TASKS)
	{
		s_tasks[id].last_checkin = xTaskGetTickCount();
	}
}

/*
*	A stack overflow breaks memory. We cannot trust the system, so we reset right now.
*/
void Supervisor_StackOverflowHook(TaskHandle_t task, char *task_name)
{
	__disable_irq();
	NVIC_SystemReset();
}
