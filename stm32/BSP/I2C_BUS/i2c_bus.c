#include "i2c_bus.h"
#include "semphr.h"

static SemaphoreHandle_t s_mutex;

void I2C_Bus_Init(void)
{
	if (s_mutex == NULL)
	{
		s_mutex = xSemaphoreCreateMutex();
	}
}

bool I2C_Bus_Lock(TickType_t timeout)
{
	return (s_mutex != NULL) && (xSemaphoreTake(s_mutex, timeout) == pdTRUE);
}

void I2C_Bus_Unlock(void)
{
	if (s_mutex != NULL)
	{
		(void)xSemaphoreGive(s_mutex);
	}
}
