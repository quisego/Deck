#ifndef PROJ_LOGGER_H
#define PROJ_LOGGER_H

#include "FreeRTOS.h"
#include "result.h"
#include "stm32f4xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
	Log levels less than it cut on complilation stage.
*/
#ifndef LOGGER_COMPILE_LEVEL
#define LOGGER_COMPILE_LEVEL LOG_DEBUG
#endif

#ifndef LOGGER_MAX_MESSAGE_LENGTH_LIMIT
#define LOGGER_MAX_MESSAGE_LENGTH_LIMIT 512U
#endif

/*
	Size of transfer buffer.
	How much messages task can merge in one DMA (more = less interrupts).
*/
#ifndef LOGGER_TX_BATCH
#define LOGGER_TX_BATCH 4U
#endif

/*
	Size of argumnets after line fmt in one LOG_x.
*/
#define LOGGER_MAX_ARGS 6U

typedef enum {
	LOG_DEBUG = 0,
	LOG_INFO,
	LOG_WARN,
	LOG_ERROR,
} LogLevel;

/*
	One event argument = one machine word (32bits on Cortex-M4)
*/
typedef uintptr_t LogArg;

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

void Logger_Log(LogLevel level, const char *file, int line, const char *fmt, LogArg a0, LogArg a1, LogArg a2, LogArg a3, LogArg a4, LogArg a5);

void Logger_UART_TxCpltCallback(UART_HandleTypeDef *huart);
void Logger_UART_ErrorCallback(UART_HandleTypeDef *huart);
uint32_t Logger_GetDroppedCount(void);

/*
	Name of file without path.
*/
#define LOGGER_FILE (__builtin_strrchr(__FILE__, '/') ? __builtin_strrchr(__FILE__, '/') + 1 : __FILE__)

/*
	Void function with format arg.
	For compiler check of format and arguments.
*/
static inline void logger_format_check(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static inline void logger_format_check(const char *fmt, ...) { (void)fmt; }

/*
	Argument error.
*/
extern LogArg logger_bad_arg(void) __attribute__((error("LOG: argument must be <= 32 bits (no float/double/long long)")));

/*
	Transform argument in LogArg and reject type sizes more then word.
*/
#define LOGGER_ARG(a) _Generic((a),			\
	float: logger_bad_arg(),							\
	double: logger_bad_arg(),							\
	long double: logger_bad_arg(),				\
	long long: logger_bad_arg(),					\
	unsigned long long: logger_bad_arg(), \
	default: (LogArg)(a))

/*
	Count of arguments (to 12).
	Works for 0 arguments too.
*/
#define LOGGER_NARGS(...) LOGGER_NARGS_(0, ##__VA_ARGS__, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define LOGGER_NARGS_(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, N, ...) N

/*
	6 arguments
*/
#define LOGGER_ARGS6(...) LOGGER_TAKE6(0, ##__VA_ARGS__, 0, 0, 0, 0, 0, 0, 0)
#define LOGGER_TAKE6(_d, a0, a1, a2, a3, a4, a5, ...) LOGGER_ARG(a0), LOGGER_ARG(a1), LOGGER_ARG(a2), LOGGER_ARG(a3), LOGGER_ARG(a4), LOGGER_ARG(a5)

#define LOG(level, fmt, ...)																															\
	do																																											\
	{																																												\
		_Static_assert(LOGGER_NARGS(__VA_ARGS__) <= LOGGER_MAX_ARGS, "LOG: at most 6 args");	\
		if ((int)(level) >= (int)LOGGER_COMPILE_LEVEL)																				\
		{																																											\
			if (0)																																							\
			{																																										\
				logger_format_check(fmt, ##__VA_ARGS__);																					\
			}																																										\
			Logger_Log((level), LOGGER_FILE, __LINE__, "" fmt, LOGGER_ARGS6(__VA_ARGS__));			\
		}																																											\
	} while (0)

#define LOG_D(...) LOG(LOG_DEBUG, __VA_ARGS__)
#define LOG_I(...) LOG(LOG_INFO, __VA_ARGS__)
#define LOG_W(...) LOG(LOG_WARN, __VA_ARGS__)
#define LOG_E(...) LOG(LOG_ERROR, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
