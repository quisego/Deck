#include "bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

/*
	Topics (BASE = CONFIG_APP_MQTT_BASE_TOPIC):

	BASE/status                  "online" / "offline" (LWT)           retained  — bridge ESP32
	BASE/esp/state               {"uptime_s","heap_free","heap_min","rssi"}  retained, once for 30 s
	BASE/stm/online              "1" / "0" (STM32 silence > APP_STM_TIMEOUT_S)  retained
	BASE/stm/state               {"uptime_s","heap_free","heap_min","oled","rtc","bme"}  retained
	BASE/sensor/env              {"temperature_c","humidity_pct","pressure_hpa"}  retained, only once when changed
	BASE/sensor/temperature|humidity|pressure    (retained)
	BASE/rtc/time                {"iso":"2026-11-07T12:00:00","weekday":7}  retained, once for N s
	BASE/button/<A|B>/event      {"event","clicks","duration_ms","stm_ts_ms"}
	BASE/button/<A|B>/state      "1" / "0"  retained

	Команди (підписка):
	BASE/cmd/rtc/set             "YYYY-MM-DD HH:MM:SS"  -> write time to DS1307
	BASE/cmd/rtc/sync            будь-який payload      -> write time ESP32 (SNTP) у DS1307
	BASE/cmd/rtc/result          "ok" / "error"  (answer)
*/

#define BASE CONFIG_APP_MQTT_BASE_TOPIC
#define TOPIC_MAX 96
#define JSON_MAX 256
#define BRIDGE_TASK_STACK 4096
#define BRIDGE_PERIOD_MS 5000
#define ESP_STATE_EVERY_TICKS 6 /* 6 * 5 s = 30 s */

static const char *TAG = "bridge";

static Link *s_link;
static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static SemaphoreHandle_t s_lock;

static bool s_stm_online;
static int64_t s_last_stm_ms;

static char s_cache_env[JSON_MAX];
static char s_cache_stm[JSON_MAX];

static char s_will_topic[TOPIC_MAX];
static bool s_sntp_started;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int64_t now_ms(void)
{
	return esp_timer_get_time() / 1000;
}

static void make_topic(char *out, size_t size, const char *suffix)
{
	(void)snprintf(out, size, "%s/%s", BASE, suffix);
}

static void publish(const char *suffix, const char *payload, int qos, bool retain)
{
	if (s_client == NULL || !s_connected)
	{
		return;
	}

	char topic[TOPIC_MAX];

	make_topic(topic, sizeof(topic), suffix);

	if (esp_mqtt_client_enqueue(s_client, topic, payload, 0, qos, retain, true) < 0)
	{
		ESP_LOGW(TAG, "enqueue failed: %s", topic);
	}
}

static void cache_set(char *dst, const char *src)
{
	xSemaphoreTake(s_lock, portMAX_DELAY);
	strlcpy(dst, src, JSON_MAX);
	xSemaphoreGive(s_lock);
}

static void touch_stm(void)
{
	bool flipped = false;

	xSemaphoreTake(s_lock, portMAX_DELAY);
	s_last_stm_ms = now_ms();
	if (!s_stm_online)
	{
		s_stm_online = true;
		flipped = true;
	}
	xSemaphoreGive(s_lock);

	if (flipped)
	{
		ESP_LOGI(TAG, "STM32 online");
		publish("stm/online", "1", 1, true);
	}
}

/* ------------------------------------------------------------------ */
/* STM32 -> MQTT                                                       */
/* ------------------------------------------------------------------ */

void Bridge_OnButton(uint16_t msg_id, const char *name, const ProtoButtonEvent *ev)
{
	touch_stm();

	const char *event = "unknown";

	switch (msg_id)
	{
	case MSG_ID_BUTTON_PRESSED:    event = "pressed";    break;
	case MSG_ID_BUTTON_RELEASED:   event = "released";   break;
	case MSG_ID_BUTTON_CLICKS:     event = "clicks";     break;
	case MSG_ID_BUTTON_LONG_PRESS: event = "long_press"; break;
	default: break;
	}

	char suffix[32];
	char json[JSON_MAX];

	(void)snprintf(suffix, sizeof(suffix), "button/%s/event", name);
	(void)snprintf(json, sizeof(json), "{\"event\":\"%s\",\"clicks\":%u,\"duration_ms\":%u,\"stm_ts_ms\":%u}",
		event, (unsigned)ev->click_count, (unsigned)ev->duration_ms, (unsigned)ev->timestamp_ms);
	publish(suffix, json, 1, false);

	if (msg_id == MSG_ID_BUTTON_PRESSED || msg_id == MSG_ID_BUTTON_RELEASED)
	{
		(void)snprintf(suffix, sizeof(suffix), "button/%s/state", name);
		publish(suffix, (msg_id == MSG_ID_BUTTON_PRESSED) ? "1" : "0", 1, true);
	}
}

void Bridge_OnRtc(const ProtoRtcTime *t)
{
	static uint32_t counter;

	touch_stm();

	if ((counter++ % (uint32_t)CONFIG_APP_MQTT_RTC_PERIOD_S) != 0U)
	{
		return;
	}

	char json[JSON_MAX];

	(void)snprintf(json, sizeof(json), "{\"iso\":\"%04u-%02u-%02uT%02u:%02u:%02u\",\"weekday\":%u}",
		(unsigned)t->year, (unsigned)t->month, (unsigned)t->date,
		(unsigned)t->hours, (unsigned)t->minutes, (unsigned)t->seconds, (unsigned)t->weekday);
	publish("rtc/time", json, 0, true);
}

static void format_env_json(const ProtoEnvData *e, char *json, size_t size)
{
	const int t = e->temperature_c_x100;
	const int ta = (t < 0) ? -t : t;

	(void)snprintf(json, size, "{\"temperature_c\":%s%d.%02d,\"humidity_pct\":%u.%02u,\"pressure_hpa\":%u.%02u}",
		(t < 0) ? "-" : "", ta / 100, ta % 100,
		(unsigned)(e->humidity_pct_x100 / 100U), (unsigned)(e->humidity_pct_x100 % 100U),
		(unsigned)(e->pressure_pa / 100U), (unsigned)(e->pressure_pa % 100U));
}

void Bridge_OnEnv(const ProtoEnvData *e)
{
	touch_stm();

	char json[JSON_MAX];
	char val[24];
	const int t = e->temperature_c_x100;
	const int ta = (t < 0) ? -t : t;

	format_env_json(e, json, sizeof(json));
	cache_set(s_cache_env, json);
	publish("sensor/env", json, 1, true);

	(void)snprintf(val, sizeof(val), "%s%d.%02d", (t < 0) ? "-" : "", ta / 100, ta % 100);
	publish("sensor/temperature", val, 1, true);

	(void)snprintf(val, sizeof(val), "%u.%02u", (unsigned)(e->humidity_pct_x100 / 100U), (unsigned)(e->humidity_pct_x100 % 100U));
	publish("sensor/humidity", val, 1, true);

	(void)snprintf(val, sizeof(val), "%u.%02u", (unsigned)(e->pressure_pa / 100U), (unsigned)(e->pressure_pa % 100U));
	publish("sensor/pressure", val, 1, true);
}

void Bridge_OnStmStatus(const ProtoSysStatus *s)
{
	touch_stm();

	char json[JSON_MAX];

	(void)snprintf(json, sizeof(json),
		"{\"uptime_s\":%u,\"heap_free\":%u,\"heap_min\":%u,\"oled\":%s,\"rtc\":%s,\"bme\":%s}",
		(unsigned)(s->uptime_ms / 1000U), (unsigned)s->heap_free, (unsigned)s->heap_min,
		(s->flags & PROTO_STATUS_FLAG_OLED) ? "true" : "false",
		(s->flags & PROTO_STATUS_FLAG_RTC) ? "true" : "false",
		(s->flags & PROTO_STATUS_FLAG_BME) ? "true" : "false");
	cache_set(s_cache_stm, json);
	publish("stm/state", json, 1, true);
}

/* ------------------------------------------------------------------ */
/* cmd MQTT -> STM32                                 		              */
/* ------------------------------------------------------------------ */

bool Bridge_SendRtcSet(uint16_t year, uint8_t month, uint8_t date, uint8_t hours, uint8_t minutes, uint8_t seconds)
{
	if (s_link == NULL)
	{
		return false;
	}

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

	return Link_Send(s_link, (uint16_t)MSG_ID_RTC_SET, buf, (uint16_t)len, pdMS_TO_TICKS(20)) == LINK_OK;
}

static bool sync_rtc_from_system(void)
{
	const time_t now = time(NULL);
	struct tm tm;

	localtime_r(&now, &tm);

	if (tm.tm_year < (2024 - 1900))
	{
		return false;
	}

	return Bridge_SendRtcSet((uint16_t)(tm.tm_year + 1900), (uint8_t)(tm.tm_mon + 1), (uint8_t)tm.tm_mday,
		(uint8_t)tm.tm_hour, (uint8_t)tm.tm_min, (uint8_t)tm.tm_sec);
}

static bool topic_is(const esp_mqtt_event_handle_t ev, const char *suffix)
{
	char full[TOPIC_MAX];

	make_topic(full, sizeof(full), suffix);

	return ev->topic_len == (int)strlen(full) && memcmp(ev->topic, full, (size_t)ev->topic_len) == 0;
}

static void handle_command(const esp_mqtt_event_handle_t ev)
{
	if (ev->current_data_offset != 0 || ev->data_len != ev->total_data_len || ev->data_len >= 40)
	{
		return;
	}

	char payload[40];

	memcpy(payload, ev->data, (size_t)ev->data_len);
	payload[ev->data_len] = '\0';

	if (topic_is(ev, "cmd/rtc/set"))
	{
		unsigned y, mo, d, h, mi, s;
		bool ok = false;

		if (sscanf(payload, "%u-%u-%u%*[ T]%u:%u:%u", &y, &mo, &d, &h, &mi, &s) == 6)
		{
			ok = Bridge_SendRtcSet((uint16_t)y, (uint8_t)mo, (uint8_t)d, (uint8_t)h, (uint8_t)mi, (uint8_t)s);
		}

		ESP_LOGI(TAG, "cmd rtc/set '%s' -> %s", payload, ok ? "ok" : "error");
		publish("cmd/rtc/result", ok ? "ok" : "error", 1, false);
	}
	else if (topic_is(ev, "cmd/rtc/sync"))
	{
		const bool ok = sync_rtc_from_system();

		ESP_LOGI(TAG, "cmd rtc/sync -> %s", ok ? "ok" : "error (SNTP not ready)");
		publish("cmd/rtc/result", ok ? "ok" : "error", 1, false);
	}
}

/* ------------------------------------------------------------------ */
/* MQTT                                                                */
/* ------------------------------------------------------------------ */

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t event_id, void *event_data)
{
	(void)args;
	(void)base;

	const esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

	switch ((esp_mqtt_event_id_t)event_id)
	{
	case MQTT_EVENT_CONNECTED:
	{
		ESP_LOGI(TAG, "MQTT connected");
		s_connected = true;

		char topic[TOPIC_MAX];

		publish("status", "online", 1, true);

		make_topic(topic, sizeof(topic), "cmd/rtc/set");
		(void)esp_mqtt_client_subscribe_single(s_client, topic, 1);
		make_topic(topic, sizeof(topic), "cmd/rtc/sync");
		(void)esp_mqtt_client_subscribe_single(s_client, topic, 1);

		char json[JSON_MAX];

		xSemaphoreTake(s_lock, portMAX_DELAY);
		const bool stm_online = s_stm_online;
		strlcpy(json, s_cache_env, sizeof(json));
		xSemaphoreGive(s_lock);
		if (json[0] != '\0')
		{
			publish("sensor/env", json, 1, true);
		}

		xSemaphoreTake(s_lock, portMAX_DELAY);
		strlcpy(json, s_cache_stm, sizeof(json));
		xSemaphoreGive(s_lock);
		if (json[0] != '\0')
		{
			publish("stm/state", json, 1, true);
		}

		publish("stm/online", stm_online ? "1" : "0", 1, true);
		break;
	}

	case MQTT_EVENT_DISCONNECTED:
		ESP_LOGW(TAG, "MQTT disconnected");
		s_connected = false;
		break;

	case MQTT_EVENT_DATA:
		handle_command(ev);
		break;

	case MQTT_EVENT_ERROR:
		ESP_LOGW(TAG, "MQTT error");
		break;

	default:
		break;
	}
}

static void mqtt_start(void)
{
	make_topic(s_will_topic, sizeof(s_will_topic), "status");

	const char *user = CONFIG_APP_MQTT_USER;
	const char *pass = CONFIG_APP_MQTT_PASSWORD;

	const esp_mqtt_client_config_t cfg = {
		.broker.address.uri = CONFIG_APP_MQTT_URI,
		.credentials.client_id = CONFIG_APP_MQTT_CLIENT_ID,
		.credentials.username = (user[0] != '\0') ? user : NULL,
		.credentials.authentication.password = (pass[0] != '\0') ? pass : NULL,
		.session.keepalive = 30,
		.session.last_will = {
			.topic = s_will_topic,
			.msg = "offline",
			.msg_len = 7,
			.qos = 1,
			.retain = true,
		},
		.network.reconnect_timeout_ms = 5000,
	};

	s_client = esp_mqtt_client_init(&cfg);
	if (s_client == NULL)
	{
		ESP_LOGE(TAG, "esp_mqtt_client_init failed");
		return;
	}

	ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
	ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
}

/* ------------------------------------------------------------------ */
/* Wi-Fi + SNTP                                                        */
/* ------------------------------------------------------------------ */

#if CONFIG_APP_RTC_SYNC_ON_SNTP
static void sntp_synced_cb(struct timeval *tv)
{
	(void)tv;

	const bool ok = sync_rtc_from_system();

	ESP_LOGI(TAG, "SNTP synced -> RTC set %s", ok ? "sent" : "failed");
}
#endif

static void sntp_start_once(void)
{
	if (s_sntp_started)
	{
		return;
	}

	s_sntp_started = true;

	(void)setenv("TZ", CONFIG_APP_TZ, 1);
	tzset();

	esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");

#if CONFIG_APP_RTC_SYNC_ON_SNTP
	cfg.sync_cb = sntp_synced_cb;
#endif

	ESP_ERROR_CHECK(esp_netif_sntp_init(&cfg));
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	(void)arg;
	(void)data;

	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
	{
		(void)esp_wifi_connect();
	}
	else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
	{
		ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
		(void)esp_wifi_connect();
	}
	else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
	{
		ESP_LOGI(TAG, "WiFi got IP");
		sntp_start_once();
	}
}

static void wifi_start(void)
{
	esp_err_t err = nvs_flash_init();

	if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	ESP_ERROR_CHECK(esp_netif_init());

	err = esp_event_loop_create_default();
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
	{
		ESP_ERROR_CHECK(err);
	}

	(void)esp_netif_create_default_wifi_sta();

	const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();

	ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

	wifi_config_t wc = { 0 };

	strlcpy((char *)wc.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(wc.sta.ssid));
	strlcpy((char *)wc.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof(wc.sta.password));
	wc.sta.threshold.authmode = (CONFIG_APP_WIFI_PASSWORD[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
	ESP_ERROR_CHECK(esp_wifi_start());
}

/* ------------------------------------------------------------------ */
/* Task: timeout STM32 + state ESP32         			                		*/
/* ------------------------------------------------------------------ */

static void publish_esp_state(void)
{
	wifi_ap_record_t ap;
	const int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
	char json[JSON_MAX];

	(void)snprintf(json, sizeof(json), "{\"uptime_s\":%u,\"heap_free\":%u,\"heap_min\":%u,\"rssi\":%d}",
		(unsigned)(now_ms() / 1000), (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(), rssi);
	publish("esp/state", json, 0, true);
}

static void bridge_task(void *arg)
{
	(void)arg;

	uint32_t tick = 0U;

	for (;;)
	{
		vTaskDelay(pdMS_TO_TICKS(BRIDGE_PERIOD_MS));
		tick++;

		bool flipped = false;

		xSemaphoreTake(s_lock, portMAX_DELAY);
		if (s_stm_online && (now_ms() - s_last_stm_ms) > ((int64_t)CONFIG_APP_STM_TIMEOUT_S * 1000))
		{
			s_stm_online = false;
			flipped = true;
		}
		xSemaphoreGive(s_lock);

		if (flipped)
		{
			ESP_LOGW(TAG, "STM32 offline (no messages for %d s)", CONFIG_APP_STM_TIMEOUT_S);
			publish("stm/online", "0", 1, true);
		}

		if ((tick % ESP_STATE_EVERY_TICKS) == 0U)
		{
			publish_esp_state();
		}
	}
}

/* ------------------------------------------------------------------ */
/* Public                                                              */
/* ------------------------------------------------------------------ */

void Bridge_Init(Link *link)
{
	s_link = link;
	s_lock = xSemaphoreCreateMutex();

	if (s_lock == NULL)
	{
		ESP_LOGE(TAG, "mutex create failed");
		return;
	}

	wifi_start();
	mqtt_start();

	if (xTaskCreate(bridge_task, "bridge", BRIDGE_TASK_STACK, NULL, 3, NULL) != pdPASS)
	{
		ESP_LOGE(TAG, "bridge task create failed");
	}
}
