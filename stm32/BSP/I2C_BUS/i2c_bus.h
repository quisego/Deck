#ifndef BSP_I2C_BUS_H
#define BSP_I2C_BUS_H

#include "FreeRTOS.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void I2C_Bus_Init(void);
bool I2C_Bus_Lock(TickType_t timeout);
void I2C_Bus_Unlock(void);

#ifdef __cplusplus
}
#endif

#endif
