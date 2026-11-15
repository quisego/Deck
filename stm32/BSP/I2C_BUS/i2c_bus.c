/*
*	Small helper: a mutex (lock) for the I2C bus.
*	The OLED and the RTC chip use the same I2C wires.
*	Only one task can use the bus at one time, so tasks must take the lock first.
*/

#include "i2c_bus.h"
#include "semphr.h"

static SemaphoreHandle_t s_mutex;

/*
*	Create the mutex one time. It is safe to call this function many times.
*/
void I2C_Bus_Init(void)
{
	if (s_mutex == NULL)
	{
		s_mutex = xSemaphoreCreateMutex();
	}
}

/*
*	Try to take the lock. Wait up to "timeout" ticks. Returns true if we got it.
*/
bool I2C_Bus_Lock(TickType_t timeout)
{
	return (s_mutex != NULL) && (xSemaphoreTake(s_mutex, timeout) == pdTRUE);
}

/*
*	Give the lock back so another task can use the bus.
*/
void I2C_Bus_Unlock(void)
{
	if (s_mutex != NULL)
	{
		(void)xSemaphoreGive(s_mutex);
	}
}
