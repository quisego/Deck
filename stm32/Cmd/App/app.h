#ifndef PROJ_APP_H
#define PROJ_APP_H

#include "stm32f4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	UART_HandleTypeDef 	*logger_huart;
	UART_HandleTypeDef 	*link_huart;
	IWDG_HandleTypeDef 	*supervisor_hiwdg;
	SPI_HandleTypeDef 	*spi_h;
	I2C_HandleTypeDef		*i2c_h;
} AppConfig;

void App_Init(const AppConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif
