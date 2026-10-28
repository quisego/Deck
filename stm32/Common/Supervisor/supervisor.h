#ifndef PROJ_SUPERVISOR_H
#define PROJ_SUPERVISOR_H

#include <stdbool.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "result.h"
#include "stm32f4xx_hal.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SUPERVISOR_MAX_TASKS
#define SUPERVISOR_MAX_TASKS 8U
#endif

#define SUPERVISOR_INVALID_ID 0xFFU

typedef uint8_t SupervisorTaskID;

typedef struct {
	IWDG_HandleTypeDef 	*hiwdg;
	uint16_t 						check_period_ms;
	uint16_t 						stack_min_free_words;
	uint16_t 						task_stack_words;
	UBaseType_t 				task_priority;
} SupervisorConfig;

typedef enum {
	SUPERVISOR_ERROR_NONE = 0,
	SUPERVISOR_ERROR_INVALID_CONFIG,
	SUPERVISOR_ERROR_ALREADY_INITIALIZED,
	SUPERVISOR_ERROR_TASK_CREATE_FAILED,
} SupervisorError;

DEFINE_RESULT(SupervisorError, SupervisorError);

Result(SupervisorError, SupervisorError) Supervisor_Init(const SupervisorConfig *cfg);

SupervisorTaskID Supervisor_Register(TaskHandle_t task, uint32_t timeout_ms);

void Supervisor_CheckIn(SupervisorTaskID id);

void Supervisor_StackOverflowHook(TaskHandle_t task, char *task_name);

#ifdef __cplusplus
}
#endif

#endif
