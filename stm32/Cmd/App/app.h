#ifndef PROJ_APP_H
#define PROJ_APP_H

#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	UART_HandleTypeDef *logger_huart;
	UART_HandleTypeDef *link_huart;
	IWDG_HandleTypeDef *supervisor_hiwdg;
} AppConfig;

void App_Init(const AppConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif
