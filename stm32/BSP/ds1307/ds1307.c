/*
*	Driver for the DS1307 real-time clock chip (I2C).
*	One task reads the time often, and calls the callback when the time changes.
*	Setting the time is done by the same task through a queue.
*/

#include "ds1307.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "i2c_bus.h"
#include "supervisor.h"
#include <stdio.h>
#include <string.h>

#define DS1307_TASK_STACK_WORDS        512U
#define DS1307_SUPERVISOR_TIMEOUT_MS   3000U
#define DS1307_LOCK_TIMEOUT_MS         200U
#define DS1307_SET_QUEUE_LENGTH        2U

/*
*	Internal state of the driver.
*/
typedef struct
{
	DS1307 rtc;
	QueueHandle_t set_queue;
	ProtoRtcTime last;
	volatile bool has_last;
	volatile bool online;
	volatile bool in_use;
	volatile SupervisorTaskID supervisor_id;
} DS1307_Runtime;

/*
*	Only ONE clock chip is supported.
*/
static DS1307_Runtime s_rt;
static DS1307_EventCallback s_event_cb;
static void *s_event_ctx;

/*
*	Weekday names. Index 1 = Sunday ... 7 = Saturday. Index 0 means "unknown".
*/
static const char *const WEEKDAY[8] = { "---", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

/*
*	The chip stores numbers in BCD. Example: 0x45 means 45 (each hex digit is a decimal digit).
*/
static uint8_t bcd_to_dec(uint8_t bcd)
{
	return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0FU));
}

static uint8_t dec_to_bcd(uint8_t dec)
{
	return (uint8_t)(((dec / 10U) << 4) | (dec % 10U));
}

/*
*	Check that both digits are 0..9.
*/
static bool bcd_valid(uint8_t bcd)
{
	return ((bcd >> 4) <= 9U) && ((bcd & 0x0FU) <= 9U);
}

/*
*	Leap year rule: divisible by 4, but not by 100, unless divisible by 400.
*/
static bool is_leap(uint16_t y)
{
	return ((y % 4U) == 0U && (y % 100U) != 0U) || ((y % 400U) == 0U);
}

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
	static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

	if (month == 2U && is_leap(year))
	{
		return 29U;
	}

	return days[month - 1U];
}

/*
*	Check that a date and time make sense (year 2000..2099, real day for the month, etc.).
*/
static bool time_valid(const ProtoRtcTime *t)
{
	return t->year >= 2000U && t->year <= 2099U
		&& t->month >= 1U && t->month <= 12U
		&& t->date >= 1U && t->date <= days_in_month(t->year, t->month)
		&& t->hours <= 23U && t->minutes <= 59U && t->seconds <= 59U;
}

/*
*	Calculate the weekday from the date (a known short formula). Result: 1 = Sunday ... 7 = Saturday.
*/
static uint8_t weekday_from_date(uint16_t year, uint8_t month, uint8_t date)
{
	static const uint8_t t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
	uint16_t y = year;

	if (month < 3U)
	{
		y--;
	}

	const uint16_t dow = (uint16_t)((y + y / 4U - y / 100U + y / 400U + t[month - 1U] + date) % 7U);

	return (uint8_t)(dow + 1U);
}

static void emit(const ProtoRtcTime *time)
{
	if (s_event_cb != NULL)
	{
		s_event_cb((uint16_t)MSG_ID_RTC_TIME, time, s_event_ctx);
	}
}

void DS1307_SetEventCallback(DS1307_EventCallback callback, void *ctx)
{
	s_event_cb = callback;
	s_event_ctx = ctx;
}

/*
*	Read 7 bytes from the chip (seconds ... year) and convert them. Returns false if it fails.
*/
static bool read_time(const DS1307_Config *cfg, ProtoRtcTime *out, bool *halted)
{
	uint8_t raw[7];
	HAL_StatusTypeDef status;

	// Take the I2C lock first, because the OLED uses the same bus.
	if (!I2C_Bus_Lock(pdMS_TO_TICKS(DS1307_LOCK_TIMEOUT_MS)))
	{
		return false;
	}

	status = HAL_I2C_Mem_Read(cfg->hi2c, cfg->addr, DS1307_REG_SECONDS, I2C_MEMADD_SIZE_8BIT, raw, sizeof(raw), DS1307_I2C_TIMEOUT);
	I2C_Bus_Unlock();

	if (status != HAL_OK)
	{
		return false;
	}

	// Bit CH (clock halt) = 1 means the clock oscillator is stopped.
	*halted = (raw[0] & DS1307_CH_BIT) != 0U;

	const uint8_t sec = (uint8_t)(raw[0] & 0x7FU);
	const uint8_t min = raw[1];
	uint8_t hrs = raw[2];

	// Bad BCD digits mean bad data. Ignore this reading.
	if (!bcd_valid(sec) || !bcd_valid(min) || !bcd_valid(raw[4]) || !bcd_valid(raw[5]) || !bcd_valid(raw[6]))
	{
		return false;
	}

	// The chip can use 12h or 24h mode. We always convert to 24h.
	if ((hrs & DS1307_HOURS_12H_BIT) != 0U)
	{
		const bool pm = (hrs & DS1307_HOURS_PM_BIT) != 0U;
		const uint8_t h12 = bcd_to_dec((uint8_t)(hrs & 0x1FU)) % 12U;

		hrs = (uint8_t)(h12 + (pm ? 12U : 0U));
	}
	else
	{
		if (!bcd_valid((uint8_t)(hrs & 0x3FU)))
		{
			return false;
		}
		hrs = bcd_to_dec((uint8_t)(hrs & 0x3FU));
	}

	out->seconds = bcd_to_dec(sec);
	out->minutes = bcd_to_dec(min);
	out->hours = hrs;
	out->weekday = (uint8_t)(raw[3] & 0x07U);
	out->date = bcd_to_dec(raw[4]);
	out->month = bcd_to_dec(raw[5]);
	out->year = (uint16_t)(2000U + bcd_to_dec(raw[6]));

	// Last check: all values must be in a normal range.
	return out->month >= 1U && out->month <= 12U && out->date >= 1U && out->date <= 31U
		&& out->hours <= 23U && out->minutes <= 59U && out->seconds <= 59U
		&& out->weekday >= 1U && out->weekday <= 7U;
}

/*
*	Write all 7 time registers at once.
*/
static bool write_time(const DS1307_Config *cfg, const ProtoRtcTime *t)
{
	const uint8_t raw[7] = {
		(uint8_t)(dec_to_bcd(t->seconds) & 0x7FU),
		dec_to_bcd(t->minutes),
		(uint8_t)(dec_to_bcd(t->hours) & 0x3FU),
		t->weekday,
		dec_to_bcd(t->date),
		dec_to_bcd(t->month),
		dec_to_bcd((uint8_t)(t->year % 100U)),
	};
	HAL_StatusTypeDef status;

	if (!I2C_Bus_Lock(pdMS_TO_TICKS(DS1307_LOCK_TIMEOUT_MS)))
	{
		return false;
	}

	status = HAL_I2C_Mem_Write(cfg->hi2c, cfg->addr, DS1307_REG_SECONDS, I2C_MEMADD_SIZE_8BIT, (uint8_t *)raw, sizeof(raw), DS1307_I2C_TIMEOUT);
	I2C_Bus_Unlock();

	return status == HAL_OK;
}

/*
*	If the clock is halted (CH bit = 1), clear the bit to start it.
*/
static void start_oscillator(const DS1307_Config *cfg)
{
	uint8_t sec = 0U;

	if (!I2C_Bus_Lock(pdMS_TO_TICKS(DS1307_LOCK_TIMEOUT_MS)))
	{
		return;
	}

	if (HAL_I2C_Mem_Read(cfg->hi2c, cfg->addr, DS1307_REG_SECONDS, I2C_MEMADD_SIZE_8BIT, &sec, 1U, DS1307_I2C_TIMEOUT) == HAL_OK
		&& (sec & DS1307_CH_BIT) != 0U)
	{
		sec &= (uint8_t)(~DS1307_CH_BIT);
		(void)HAL_I2C_Mem_Write(cfg->hi2c, cfg->addr, DS1307_REG_SECONDS, I2C_MEMADD_SIZE_8BIT, &sec, 1U, DS1307_I2C_TIMEOUT);
	}

	I2C_Bus_Unlock();
}

/*
*	Main loop: read time -> send an event if it changed -> check in -> wait for a "set time" command.
*/
static void ds1307_task(void *arg)
{
	DS1307_Runtime *rt = (DS1307_Runtime *)arg;
	const DS1307_Config *cfg = &rt->rtc.config;

	TickType_t poll_ticks = pdMS_TO_TICKS(cfg->poll_ms);
	if (poll_ticks == 0U)
	{
		poll_ticks = 1U;
	}

	// Send the first time (and the time after a set) even if it did not change.
	bool force_emit = true;

	for (;;)
	{
		ProtoRtcTime now;
		bool halted = false;

		if (read_time(cfg, &now, &halted))
		{
			rt->online = true;

			if (halted)
			{
				start_oscillator(cfg);
			}

			// Send an event only when the time is different from the last one.
			if (force_emit || !rt->has_last || memcmp(&now, &rt->last, sizeof(now)) != 0)
			{
				taskENTER_CRITICAL();
				rt->last = now;
				rt->has_last = true;
				taskEXIT_CRITICAL();

				force_emit = false;
				emit(&now);
			}
		}
		else
		{
			// Read failed: the chip is offline.
			rt->online = false;
		}

		if (rt->supervisor_id != SUPERVISOR_INVALID_ID)
		{
			Supervisor_CheckIn(rt->supervisor_id);
		}

		ProtoRtcTime req;

		// This is the sleep AND the command check: wait for a "set time" request until the poll time ends.
		if (xQueueReceive(rt->set_queue, &req, poll_ticks) == pdPASS)
		{
			(void)write_time(cfg, &req);
			force_emit = true;
		}
	}
}

static const DS1307_Ops ds1307_ops = {
	.get_last = DS1307_GetLast,
	.set_time = DS1307_SetTime,
	.is_online = DS1307_IsOnline,
};

/*
*	Start the driver: check config, create the queue and the task, register in the supervisor.
*/
Result(DS1307, DS1307_Error) DS1307_Init(const DS1307_Config *config)
{
	if (config == NULL)
	{
		return RESULT_ERROR(DS1307, DS1307_Error, DS1307_ERROR_NULLPTR);
	}

	if (config->hi2c == NULL || config->poll_ms == 0U)
	{
		return RESULT_ERROR(DS1307, DS1307_Error, DS1307_ERROR_INVALID_ARG);
	}

	// Already started? Return the same object.
	if (s_rt.in_use)
	{
		return RESULT_OK(DS1307, DS1307_Error, s_rt.rtc);
	}

	memset(&s_rt, 0, sizeof(s_rt));

	s_rt.rtc.config = *config;
	s_rt.rtc.ops = &ds1307_ops;
	s_rt.supervisor_id = SUPERVISOR_INVALID_ID;

	I2C_Bus_Init();

	s_rt.set_queue = xQueueCreate(DS1307_SET_QUEUE_LENGTH, sizeof(ProtoRtcTime));
	if (s_rt.set_queue == NULL)
	{
		return RESULT_ERROR(DS1307, DS1307_Error, DS1307_ERROR_TASK_CREATE_FAILED);
	}

	s_rt.in_use = true;

	TaskHandle_t handle = NULL;

	if (xTaskCreate(ds1307_task, "rtc", DS1307_TASK_STACK_WORDS, &s_rt, tskIDLE_PRIORITY + 1U, &handle) != pdPASS)
	{
		s_rt.in_use = false;
		vQueueDelete(s_rt.set_queue);
		s_rt.set_queue = NULL;
		return RESULT_ERROR(DS1307, DS1307_Error, DS1307_ERROR_TASK_CREATE_FAILED);
	}

	s_rt.supervisor_id = Supervisor_Register(handle, DS1307_SUPERVISOR_TIMEOUT_MS);

	return RESULT_OK(DS1307, DS1307_Error, s_rt.rtc);
}

/*
*	Get the last known time. Returns false if there is none yet.
*/
bool DS1307_GetLast(const DS1307 *self, ProtoRtcTime *out)
{
	(void)self;

	if (!s_rt.in_use || out == NULL || !s_rt.has_last)
	{
		return false;
	}

	taskENTER_CRITICAL();
	*out = s_rt.last;
	taskEXIT_CRITICAL();

	return true;
}

/*
*	Ask the driver to set a new time. It only checks the data and puts it in a queue.
*	The task writes it to the chip later.
*/
DS1307_Error DS1307_SetTime(const DS1307 *self, const ProtoRtcTime *time)
{
	(void)self;

	if (!s_rt.in_use)
	{
		return DS1307_ERROR_UNINITIALIZED;
	}

	if (time == NULL)
	{
		return DS1307_ERROR_NULLPTR;
	}

	if (!time_valid(time))
	{
		return DS1307_ERROR_INVALID_ARG;
	}

	ProtoRtcTime req = *time;

	// We calculate the weekday ourselves. The caller does not need to send it.
	req.weekday = weekday_from_date(req.year, req.month, req.date);

	// Do not wait: if the queue is full, return an error.
	if (xQueueSend(s_rt.set_queue, &req, 0) != pdPASS)
	{
		return DS1307_ERROR_QUEUE_FULL;
	}

	return DS1307_ERROR_NONE;
}

/*
*	Text helpers for the OLED, for example "12:30:05" and "Mon 10.11.2026".
*/
void DS1307_TimeFormat(const ProtoRtcTime *time, char *buf, size_t size)
{
	(void)snprintf(buf, size, "%02u:%02u:%02u", (unsigned)time->hours, (unsigned)time->minutes, (unsigned)time->seconds);
}

void DS1307_DateFormat(const ProtoRtcTime *time, char *buf, size_t size)
{
	const char *wd = (time->weekday >= 1U && time->weekday <= 7U) ? WEEKDAY[time->weekday] : WEEKDAY[0];

	(void)snprintf(buf, size, "%s %02u.%02u.%04u", wd, (unsigned)time->date, (unsigned)time->month, (unsigned)time->year);
}

bool DS1307_IsOnline(const DS1307 *self)
{
	(void)self;

	return s_rt.in_use && s_rt.online;
}
