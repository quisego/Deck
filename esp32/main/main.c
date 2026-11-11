/*
*	ESP32 main file. It receives messages from the STM32 over UART,
*	prints them to the log, and gives them to the "bridge" (Wi-Fi + MQTT).
*/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bridge.h"
#include "link.h"
#include "msg.h"

/*
*	Name for ESP_LOGx messages.
*/
static const char *TAG = "app";

static Link g_link;

/*
*	Button ids. They must be the same as the ids made on the STM32 (port + pin).
*/
#define BTN_ID_A 0x10U
#define BTN_ID_B 0x11U

/*
*	Information about one button: id, name and the last known state.
*/
typedef struct
{
	uint8_t id;
	const char *name;
	bool pressed;
} ButtonSlot;

static ButtonSlot s_buttons[] = {
	{ .id = BTN_ID_A, .name = "A", .pressed = false },
	{ .id = BTN_ID_B, .name = "B", .pressed = false },
};

/*
*	Number of buttons in the array.
*/
#define BUTTON_SLOTS (sizeof(s_buttons) / sizeof(s_buttons[0]))

/*
*	Find a button by its id. Returns NULL if it is unknown.
*/
static ButtonSlot *find_slot(uint8_t id)
{
	for (size_t i = 0; i < BUTTON_SLOTS; i++)
	{
		if (s_buttons[i].id == id)
		{
			return &s_buttons[i];
		}
	}

	return NULL;
}

/*
*	Print the state of all known buttons in one line, for example "A=1 B=0".
*	Works for any number of slots in s_buttons.
*/
static void log_state(void)
{
	char line[96];
	size_t pos = 0U;

	line[0] = '\0';

	for (size_t i = 0; i < BUTTON_SLOTS; i++)
	{
		const int n = snprintf(&line[pos], sizeof(line) - pos, "%s%s=%d",
			(i == 0U) ? "" : " ", s_buttons[i].name, s_buttons[i].pressed ? 1 : 0);

		// Stop if the line is full (the rest is cut, this is only a log).
		if (n < 0 || (size_t)n >= (sizeof(line) - pos))
		{
			break;
		}

		pos += (size_t)n;
	}

	ESP_LOGI(TAG, "%s", line);
}

/*
*	Callback: STM32 sent a button event. Decode it, update the state, log it, send to MQTT.
*/
static void on_button_message(const LinkMessage *msg, void *ctx)
{
	(void)ctx;

	ProtoButtonEvent ev;

	// Wrong payload size or content -> warning and stop.
	if (ProtoMsg_DecodeButtonEvent(msg->payload, msg->length, &ev) != PROTOCOL_OK)
	{
		ESP_LOGW(TAG, "Bad button payload (id=0x%04x, len=%u)", (unsigned)msg->id, (unsigned)msg->length);
		return;
	}

	ButtonSlot *slot = find_slot(ev.button_id);

	// Known button: use its name. Unknown button: name is "0xNN", and we still forward the event to the PC.
	// The PC needs this to "learn" a button that this firmware does not know yet.
	char name_buf[8];
	const char *name;

	if (slot != NULL)
	{
		name = slot->name;
	}
	else
	{
		(void)snprintf(name_buf, sizeof(name_buf), "0x%02X", (unsigned)ev.button_id);
		name = name_buf;

		// One warning per physical press is enough (not for release, clicks, long press).
		if (msg->id == MSG_ID_BUTTON_PRESSED)
		{
			ESP_LOGW(TAG, "Unknown button id %s, forwarding without local state", name);
		}
	}

	// The message id tells which event it is. The local state is kept only for known buttons.
	switch (msg->id)
	{
	case MSG_ID_BUTTON_PRESSED:
		ESP_LOGI(TAG, "Button %s: 1 (t=%u ms)", name, (unsigned)ev.timestamp_ms);
		if (slot != NULL)
		{
			slot->pressed = true;
			log_state();
		}
		break;

	case MSG_ID_BUTTON_RELEASED:
		ESP_LOGI(TAG, "Button %s: 0 (held %u ms)", name, (unsigned)ev.duration_ms);
		if (slot != NULL)
		{
			slot->pressed = false;
			log_state();
		}
		break;

	case MSG_ID_BUTTON_CLICKS:
		ESP_LOGI(TAG, "Button %s: %u click(s)", name, (unsigned)ev.click_count);
		break;

	case MSG_ID_BUTTON_LONG_PRESS:
		ESP_LOGI(TAG, "Button %s: long press", name);
		break;

	default:
		break;
	}

	Bridge_OnButton(msg->id, name, &ev);
}

/*
*	Callback: new time from the STM32 clock.
*/
static void on_rtc_message(const LinkMessage *msg, void *ctx)
{
	(void)ctx;

	ProtoRtcTime t;

	if (ProtoMsg_DecodeRtcTime(msg->payload, msg->length, &t) != PROTOCOL_OK)
	{
		ESP_LOGW(TAG, "Bad RTC payload (len=%u)", (unsigned)msg->length);
		return;
	}

	ESP_LOGD(TAG, "RTC: %04u-%02u-%02u (wd=%u) %02u:%02u:%02u",
		(unsigned)t.year, (unsigned)t.month, (unsigned)t.date, (unsigned)t.weekday,
		(unsigned)t.hours, (unsigned)t.minutes, (unsigned)t.seconds);

	Bridge_OnRtc(&t);
}

/*
*	Callback: new temperature, humidity and pressure. Values come multiplied by 100.
*/
static void on_env_message(const LinkMessage *msg, void *ctx)
{
	(void)ctx;

	ProtoEnvData e;

	if (ProtoMsg_DecodeEnvData(msg->payload, msg->length, &e) != PROTOCOL_OK)
	{
		ESP_LOGW(TAG, "Bad ENV payload (len=%u)", (unsigned)msg->length);
		return;
	}

	const int t = e.temperature_c_x100;
	// Use the absolute value so we can print "-" and the digits separately (also for -0.5).
	const int t_abs = (t < 0) ? -t : t;

	ESP_LOGI(TAG, "ENV: T=%s%d.%02d C, RH=%u.%02u %%, P=%u.%02u hPa",
		(t < 0) ? "-" : "", t_abs / 100, t_abs % 100,
		(unsigned)(e.humidity_pct_x100 / 100U), (unsigned)(e.humidity_pct_x100 % 100U),
		(unsigned)(e.pressure_pa / 100U), (unsigned)(e.pressure_pa % 100U));

	Bridge_OnEnv(&e);
}

/*
*	Callback: STM32 health information (uptime, memory, which devices work).
*/
static void on_status_message(const LinkMessage *msg, void *ctx)
{
	(void)ctx;

	ProtoSysStatus s;

	if (ProtoMsg_DecodeSysStatus(msg->payload, msg->length, &s) != PROTOCOL_OK)
	{
		ESP_LOGW(TAG, "Bad STATUS payload (len=%u)", (unsigned)msg->length);
		return;
	}

	ESP_LOGI(TAG, "STM32: up %u s, heap free %u (min %u), oled=%d rtc=%d bme=%d",
		(unsigned)(s.uptime_ms / 1000U), (unsigned)s.heap_free, (unsigned)s.heap_min,
		(s.flags & PROTO_STATUS_FLAG_OLED) ? 1 : 0,
		(s.flags & PROTO_STATUS_FLAG_RTC) ? 1 : 0,
		(s.flags & PROTO_STATUS_FLAG_BME) ? 1 : 0);

	Bridge_OnStmStatus(&s);
}

/*
*	Small helper: subscribe and log an error if it fails.
*/
static void subscribe(uint16_t id, void (*cb)(const LinkMessage *msg, void *ctx))
{
	if (Link_Subscribe(&g_link, id, cb, NULL) != LINK_OK)
	{
		ESP_LOGE(TAG, "Failed to subscribe to message 0x%04x", (unsigned)id);
	}
}

/*
*	Program start on ESP32.
*/
void app_main(void)
{
  ESP_LOGI(TAG, "Initializing Link module...");

	// UART1 with pins 4 (TX) and 5 (RX), speed 115200. Must match the STM32 UART settings.
  const LinkConfig link_cfg = {
    .uart_num = UART_NUM_1,
    .tx_pin = 4,
    .rx_pin = 5,
    .baud_rate = 115200,
    .rx_buffer_size = 1024,
    .tx_buffer_size = 1024,
    .tx_queue_length = 8,
    .max_payload = 128,
    .tx_task_stack_words = 4096,
    .rx_task_stack_words = 6144,
    .tx_task_priority = 5,
    .rx_task_priority = 5,
    .tx_timeout_ticks = pdMS_TO_TICKS(100),
    .rx_poll_ticks = pdMS_TO_TICKS(50),
  };

  if (Link_Init(&g_link, &link_cfg) != LINK_OK)
  {
    ESP_LOGE(TAG, "Failed to initialize Link module!");
    return;
  }

  ESP_LOGI(TAG, "Link module initialized successfully.");

	// Start Wi-Fi, MQTT and the bridge task.
  Bridge_Init(&g_link);

	// Subscribe to all button message ids (from the first to the last one).
  for (uint16_t id = (uint16_t)PROTO_MSG_ID_BUTTON_FIRST; id <= (uint16_t)PROTO_MSG_ID_BUTTON_LAST; id++)
  {
    subscribe(id, on_button_message);
  }

  subscribe((uint16_t)MSG_ID_RTC_TIME, on_rtc_message);
  subscribe((uint16_t)MSG_ID_ENV_DATA, on_env_message);
  subscribe((uint16_t)MSG_ID_SYS_STATUS, on_status_message);

  ESP_LOGI(TAG, "Waiting for events...");

	// Nothing to do here. All work is done by tasks and callbacks.
  while (1)
  {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
