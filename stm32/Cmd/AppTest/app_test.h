#ifndef PROJ_APP_TEST_H
#define PROJ_APP_TEST_H

#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	UART_HandleTypeDef *huart;
	IWDG_HandleTypeDef *hiwdg;
} AppTestConfig;

void AppTest_Start(const AppTestConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif
