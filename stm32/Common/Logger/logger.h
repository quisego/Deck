#ifndef PROJ_LOGGER_H
#define PROJ_LOGGER_H

#include "FreeRTOS.h"
#include "result.h"
#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LOGGER_COMPILE_LEVEL
#define LOGGER_COMPILE_LEVEL LOG_DEBUG
#endif

#ifndef LOGGER_MAX_MESSAGE_LENGTH_LIMIT
#define LOGGER_MAX_MESSAGE_LENGTH_LIMIT 512U
#endif

#ifndef LOGGER_TX_BATCH
#define LOGGER_TX_BATCH 4U
#endif

typedef enum {
	LOG_DEBUG = 0,
	LOG_INFO,
	LOG_WARN,
	LOG_ERROR,
} LogLevel;

typedef struct {
	LogLevel 						min_log_level;
	UART_HandleTypeDef 	*huart;
	uint16_t 						queue_length;
	uint16_t 						max_message_length;
	uint16_t 						task_stack_words;
	UBaseType_t 				task_priority;
} LoggerConfig;

typedef enum {
	LOGGER_ERROR_NONE = 0,
	LOGGER_ERROR_INVALID_CONFIG,
	LOGGER_ERROR_ALREADY_INITIALIZED,
	LOGGER_ERROR_NO_MEMORY,
	LOGGER_ERROR_TASK_CREATE_FAILED,
} LoggerError;

DEFINE_RESULT(LoggerError, LoggerError);

Result(LoggerError, LoggerError) Logger_Init(const LoggerConfig *config);

void Logger_Log(LogLevel level, const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

void Logger_UART_TxCpltCallback(UART_HandleTypeDef *huart);
void Logger_UART_ErrorCallback(UART_HandleTypeDef *huart);
uint32_t Logger_GetDroppedCount(void);

#define LOGGER_FILE (__builtin_strrchr(__FILE__, '/') ? __builtin_strrchr(__FILE__, '/') + 1 : __FILE__)

#define LOG(level, ...) 																				\
	do 																														\
	{ 																														\
		if ((int)(level) >= (int)LOGGER_COMPILE_LEVEL) 							\
		{                     																			\
			Logger_Log((level), LOGGER_FILE, __LINE__, __VA_ARGS__);	\
		} 																													\
	} while (0)

#define LOG_D(...) LOG(LOG_DEBUG, __VA_ARGS__)
#define LOG_I(...) LOG(LOG_INFO, __VA_ARGS__)
#define LOG_W(...) LOG(LOG_WARN, __VA_ARGS__)
#define LOG_E(...) LOG(LOG_ERROR, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
