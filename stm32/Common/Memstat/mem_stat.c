#include "mem_stat.h"
#include "FreeRTOS.h"
#include "task.h"
#include "logger.h"

#define MEMSTAT_TASK_STACK_WORDS 256U

extern uint32_t _end;
extern uint32_t _estack;
extern uint32_t _Min_Stack_Size;

static uint32_t s_period_ms;

void MemStat_Log(void)
{
	const uint32_t heap_total = (uint32_t)configTOTAL_HEAP_SIZE;
	const uint32_t heap_free = (uint32_t)xPortGetFreeHeapSize();
	const uint32_t heap_min = (uint32_t)xPortGetMinimumEverFreeHeapSize();
	const uint32_t tail = (uint32_t)&_estack - (uint32_t)&_end;
	const uint32_t msp_reserved = (uint32_t)&_Min_Stack_Size;

	LOG_I("Heap FreeRTOS: free %u, min-ever %u, total %u B",
		(unsigned)heap_free, (unsigned)heap_min, (unsigned)heap_total);
	LOG_I("RAM tail (MSP+libc): %u B, of them MSP reserved %u B",
		(unsigned)tail, (unsigned)msp_reserved);
}

static void memstat_task(void *arg)
{
	(void)arg;

	TickType_t period = pdMS_TO_TICKS(s_period_ms);

	if (period == 0U)
	{
		period = 1U;
	}

	for (;;)
	{
		MemStat_Log();
		vTaskDelay(period);
	}
}

bool MemStat_Start(uint32_t period_ms)
{
	s_period_ms = period_ms;

	return xTaskCreate(memstat_task, "memstat", MEMSTAT_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 1U, NULL) == pdPASS;
}
