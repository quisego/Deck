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

/*
	button_id = (індекс порту << 4) | номер піна (рахує STM32, див. Button_IdFromConfig).
	Мають збігатися з пінами в app.c на STM32: PB0 -> 0x10, PB1 -> 0x11.
*/
#define BTN_ID_A 0x10U
#define BTN_ID_B 0x11U

typedef struct
{
	uint8_t id;
	const char *name;
	bool pressed; /* 1 - натиснута, 0 - відпущена */
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

/* Викликається з RX-задачі Link для всіх MSG_ID_BUTTON_* */
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
    .tx_task_stack_words = 4096, /* ESP-IDF: це БАЙТИ, не слова */
    .rx_task_stack_words = 6144, /* callback-и (ESP_LOGI) виконуються в цій задачі */
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

  /* Підписка на кожен ID окремо */
  for (uint16_t id = (uint16_t)PROTO_MSG_ID_BUTTON_FIRST; id <= (uint16_t)PROTO_MSG_ID_BUTTON_LAST; id++)
  {
    if (Link_Subscribe(&g_link, id, on_button_message, NULL) != LINK_OK)
    {
      ESP_LOGE(TAG, "Failed to subscribe to message 0x%04x", (unsigned)id);
    }
  }

  ESP_LOGI(TAG, "Waiting for button events...");

  while (1)
  {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
