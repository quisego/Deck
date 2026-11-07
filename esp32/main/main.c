#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "msg.h"

static const char *TAG = "app";

static Link g_link;

#define BTN_ID_A 0x10U
#define BTN_ID_B 0x11U

#define SET_RTC_ON_BOOT 1

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

#define BUTTON_SLOTS (sizeof(s_buttons) / sizeof(s_buttons[0]))

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

static void log_state(void)
{
	ESP_LOGI(TAG, "A=%d B=%d", s_buttons[0].pressed ? 1 : 0, s_buttons[1].pressed ? 1 : 0);
}

static void on_button_message(const LinkMessage *msg, void *ctx)
{
	(void)ctx;

	ProtoButtonEvent ev;

	if (ProtoMsg_DecodeButtonEvent(msg->payload, msg->length, &ev) != PROTOCOL_OK)
	{
		ESP_LOGW(TAG, "Bad button payload (id=0x%04x, len=%u)", (unsigned)msg->id, (unsigned)msg->length);
		return;
	}

	ButtonSlot *slot = find_slot(ev.button_id);

	if (slot == NULL)
	{
		ESP_LOGW(TAG, "Unknown button id 0x%02x", (unsigned)ev.button_id);
		return;
	}

	switch (msg->id)
	{
	case MSG_ID_BUTTON_PRESSED:
		slot->pressed = true;
		ESP_LOGI(TAG, "Button %s: 1 (t=%u ms)", slot->name, (unsigned)ev.timestamp_ms);
		log_state();
		break;

	case MSG_ID_BUTTON_RELEASED:
		slot->pressed = false;
		ESP_LOGI(TAG, "Button %s: 0 (held %u ms)", slot->name, (unsigned)ev.duration_ms);
		log_state();
		break;

	case MSG_ID_BUTTON_CLICKS:
		ESP_LOGI(TAG, "Button %s: %u click(s)", slot->name, (unsigned)ev.click_count);
		break;

	case MSG_ID_BUTTON_LONG_PRESS:
		ESP_LOGI(TAG, "Button %s: long press", slot->name);
		break;

	default:
		break;
	}
}

static void on_rtc_message(const LinkMessage *msg, void *ctx)
{
	(void)ctx;

	ProtoRtcTime t;

	if (ProtoMsg_DecodeRtcTime(msg->payload, msg->length, &t) != PROTOCOL_OK)
	{
		ESP_LOGW(TAG, "Bad RTC payload (len=%u)", (unsigned)msg->length);
		return;
	}

	ESP_LOGI(TAG, "RTC: %04u-%02u-%02u (wd=%u) %02u:%02u:%02u",
		(unsigned)t.year, (unsigned)t.month, (unsigned)t.date, (unsigned)t.weekday,
		(unsigned)t.hours, (unsigned)t.minutes, (unsigned)t.seconds);
}

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
	const int t_abs = (t < 0) ? -t : t;

	ESP_LOGI(TAG, "ENV: T=%s%d.%02d C, RH=%u.%02u %%, P=%u.%02u hPa",
		(t < 0) ? "-" : "", t_abs / 100, t_abs % 100,
		(unsigned)(e.humidity_pct_x100 / 100U), (unsigned)(e.humidity_pct_x100 % 100U),
		(unsigned)(e.pressure_pa / 100U), (unsigned)(e.pressure_pa % 100U));
}

static bool send_rtc_set(uint16_t year, uint8_t month, uint8_t date, uint8_t hours, uint8_t minutes, uint8_t seconds)
{
	const ProtoRtcTime t = {
		.year = year,
		.month = month,
		.date = date,
		.weekday = 0U,
		.hours = hours,
		.minutes = minutes,
		.seconds = seconds,
	};

	uint8_t buf[PROTO_RTC_TIME_SIZE];
	size_t len = 0U;

	if (ProtoMsg_EncodeRtcTime(&t, buf, sizeof(buf), &len) != PROTOCOL_OK)
	{
		return false;
	}

	return Link_Send(&g_link, (uint16_t)MSG_ID_RTC_SET, buf, (uint16_t)len, pdMS_TO_TICKS(100)) == LINK_OK;
}

void app_main(void)
{
  ESP_LOGI(TAG, "Initializing Link module...");

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

  for (uint16_t id = (uint16_t)PROTO_MSG_ID_BUTTON_FIRST; id <= (uint16_t)PROTO_MSG_ID_BUTTON_LAST; id++)
  {
    if (Link_Subscribe(&g_link, id, on_button_message, NULL) != LINK_OK)
    {
      ESP_LOGE(TAG, "Failed to subscribe to message 0x%04x", (unsigned)id);
    }
  }

  if (Link_Subscribe(&g_link, (uint16_t)MSG_ID_RTC_TIME, on_rtc_message, NULL) != LINK_OK)
  {
    ESP_LOGE(TAG, "Failed to subscribe to RTC_TIME");
  }

  if (Link_Subscribe(&g_link, (uint16_t)MSG_ID_ENV_DATA, on_env_message, NULL) != LINK_OK)
  {
    ESP_LOGE(TAG, "Failed to subscribe to ENV_DATA");
  }

#if SET_RTC_ON_BOOT
  if (!send_rtc_set(2026, 11, 7, 12, 0, 0))
  {
    ESP_LOGE(TAG, "Failed to send RTC set");
  }
#else
  (void)send_rtc_set;
#endif

  ESP_LOGI(TAG, "Waiting for events...");

  while (1)
  {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
