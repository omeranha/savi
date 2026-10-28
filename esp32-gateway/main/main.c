#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "main.h"
#include "mqtt_gateway.h"
#include "savi_battery.h"
#include "savi_proto.h"

#define RX_QUEUE_LEN           16
#define ID_QUEUE_LEN           4
#define WIFI_CONNECTED_BIT     BIT0
#define WIFI_FAIL_BIT          BIT1
#define WIFI_RETRY_DELAY_MS    10000
#define NTP_WAIT_MS            10000
/* One self-report per minute, as specified. */
#define GATEWAY_REPORT_PERIOD_MS 60000
/* A bracelet reports every second, so this many silent seconds means it left. */
#define BRACELET_STALE_MS      90000
#define BRACELET_SLOTS         16

static const char *TAG = "gateway";

static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

typedef struct {
	uint8_t mac[ESP_NOW_ETH_ALEN];
	uint16_t len;
	uint8_t data[sizeof(savi_sensor_data_msg_t)];
} rx_packet_t;

typedef struct {
	uint8_t mac[ESP_NOW_ETH_ALEN];
	int64_t last_seen_us;
	bool used;
} bracelet_slot_t;

static QueueHandle_t s_rx_queue;
static QueueHandle_t s_id_queue;
static EventGroupHandle_t s_wifi_events;
static int s_retry;

/* Who is actually streaming to this gateway. Updated from the packet task and
 * read by the report task, so it needs the lock. */
static portMUX_TYPE s_bracelet_lock = portMUX_INITIALIZER_UNLOCKED;
static bracelet_slot_t s_braceletes[BRACELET_SLOTS];

/* Counts a bracelet as connected here only when it unicasts to us: a HELLO is
 * broadcast to every gateway in range and proves nothing about who it chose. */
static void bracelet_seen(const uint8_t *mac)
{
	int64_t now = esp_timer_get_time();
	int free_slot = -1;

	taskENTER_CRITICAL(&s_bracelet_lock);
	for (int i = 0; i < BRACELET_SLOTS; i++) {
		if (s_braceletes[i].used && memcmp(s_braceletes[i].mac, mac, ESP_NOW_ETH_ALEN) == 0) {
			s_braceletes[i].last_seen_us = now;
			taskEXIT_CRITICAL(&s_bracelet_lock);
			return;
		}
		if (!s_braceletes[i].used && free_slot < 0) {
			free_slot = i;
		}
	}
	if (free_slot >= 0) {
		memcpy(s_braceletes[free_slot].mac, mac, ESP_NOW_ETH_ALEN);
		s_braceletes[free_slot].last_seen_us = now;
		s_braceletes[free_slot].used = true;
	}
	taskEXIT_CRITICAL(&s_bracelet_lock);

	if (free_slot < 0) {
		ESP_LOGW(TAG, "All %d bracelet slots are busy, " MACSTR " is not counted", BRACELET_SLOTS, MAC2STR(mac));
	}
}

static int bracelet_count(void)
{
	int64_t now = esp_timer_get_time();
	int count = 0;

	taskENTER_CRITICAL(&s_bracelet_lock);
	for (int i = 0; i < BRACELET_SLOTS; i++) {
		if (!s_braceletes[i].used) {
			continue;
		}
		if ((now - s_braceletes[i].last_seen_us) / 1000 > BRACELET_STALE_MS) {
			s_braceletes[i].used = false;  /* moved to another gateway, or switched off */
			continue;
		}
		count++;
	}
	taskEXIT_CRITICAL(&s_bracelet_lock);
	return count;
}

/* The gateway only ever transmits broadcasts, and plaintext unicast is received
 * without the sender being a peer, so this is the only peer it needs. */
static esp_err_t add_broadcast_peer(void)
{
	if (esp_now_is_peer_exist(s_broadcast_mac)) {
		return ESP_OK;
	}

	esp_now_peer_info_t peer = {0};
	peer.channel = 0;
	peer.ifidx = ESPNOW_WIFI_IF;
	peer.encrypt = false;
	memcpy(peer.peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);

	esp_err_t err = esp_now_add_peer(&peer);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Cannot add the broadcast peer: %s", esp_err_to_name(err));
	}
	return err;
}

static uint8_t live_channel(void)
{
	uint8_t primary = 0;
	wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
	if (esp_wifi_get_channel(&primary, &second) != ESP_OK) {
		return 0;
	}
	return primary;
}

/* Tells one bracelet that this gateway is here and on which channel. Broadcast,
 * because the bracelet does not know this gateway yet; bracelet_mac says whose
 * probe it answers. */
static void welcome_bracelet(const uint8_t *mac)
{
	savi_welcome_msg_t msg = {
		.version = SAVI_PROTO_VERSION,
		.type = SAVI_MSG_WELCOME,
		.channel = live_channel(),
	};
	memcpy(msg.bracelet_mac, mac, ESP_NOW_ETH_ALEN);

	esp_err_t err = esp_now_send(s_broadcast_mac, (const uint8_t *)&msg, sizeof(msg));
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Cannot welcome " MACSTR ": %s", MAC2STR(mac), esp_err_to_name(err));
		return;
	}
	ESP_LOGI(TAG, "Answered the probe from " MACSTR " on channel %u", MAC2STR(mac), msg.channel);
}

/* Identity allocation blocks on MQTT, so it never runs on the packet path. */
static void id_task(void *arg)
{
	(void)arg;
	uint8_t mac[ESP_NOW_ETH_ALEN];

	while (xQueueReceive(s_id_queue, mac, portMAX_DELAY) == pdTRUE) {
		uint32_t esp_id = 0;
		esp_err_t err = mqtt_gateway_request_esp_id(mac, &esp_id);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "No esp_id for " MACSTR ": %s", MAC2STR(mac), esp_err_to_name(err));
			continue;
		}

		savi_assign_id_msg_t msg = {
			.version = SAVI_PROTO_VERSION,
			.type = SAVI_MSG_ASSIGN_ID,
			.esp_id = esp_id,
		};
		memcpy(msg.bracelet_mac, mac, ESP_NOW_ETH_ALEN);
		if (esp_now_send(s_broadcast_mac, (const uint8_t *)&msg, sizeof(msg)) != ESP_OK) {
			ESP_LOGE(TAG, "Cannot hand esp_id %" PRIu32 " to " MACSTR, esp_id, MAC2STR(mac));
			continue;
		}
		ESP_LOGI(TAG, "Gave esp_id %" PRIu32 " to " MACSTR, esp_id, MAC2STR(mac));
	}
	vTaskDelete(NULL);
}

/* The gateway's own row in Supabase: where it is, its battery and how many
 * bracelets are currently streaming to it. */
static void report_task(void *arg)
{
	(void)arg;

	while (1) {
		vTaskDelay(pdMS_TO_TICKS(GATEWAY_REPORT_PERIOD_MS));
		int count = bracelet_count();
		uint8_t battery = savi_battery_percent();
		esp_err_t err = mqtt_gateway_publish_status(battery, count);
		if (err != ESP_OK) {
			ESP_LOGW(TAG, "Could not report gateway status: %s", esp_err_to_name(err));
			continue;
		}
		ESP_LOGI(TAG, "Reported %d bracelet(s) connected, battery %u%%", count, battery);
	}
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	(void)arg;
	if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
		if (s_retry < CONFIG_WIFI_MAX_RETRY) {
			esp_wifi_connect();
			s_retry++;
			ESP_LOGW(TAG, "WiFi retry %d", s_retry);
		} else {
			xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
		}
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
		ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&event->ip_info.ip));
		s_retry = 0;
		xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
	}
}

/* Keep retrying instead of rebooting: the gateway is unattended. */
static void wifi_wait_connected(void)
{
	while (1) {
		EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
		if (bits & WIFI_CONNECTED_BIT) {
			return;
		}
		ESP_LOGE(TAG, "WiFi did not connect in %d attempts, retrying in %d s", CONFIG_WIFI_MAX_RETRY, WIFI_RETRY_DELAY_MS / 1000);
		xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
		vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
		s_retry = 0;
		esp_wifi_connect();
	}
}

static void wifi_init(void)
{
	s_wifi_events = xEventGroupCreate();
	configASSERT(s_wifi_events);

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

	wifi_config_t wifi_config = {0};
	strncpy((char *)wifi_config.sta.ssid, CONFIG_WIFI_SSID, sizeof(wifi_config.sta.ssid));
	strncpy((char *)wifi_config.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
	wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

	wifi_wait_connected();

	ESP_LOGI(TAG, "Associated on channel %u; bracelets sweep channels to find it", live_channel());
	uint8_t mac[ESP_NOW_ETH_ALEN];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	ESP_LOGI(TAG, "Gateway MAC " MACSTR, MAC2STR(mac));
}

/* Telemetry is dropped while the clock is invalid, so sync in the background. */
static void time_sync_task(void *arg)
{
	(void)arg;
	int attempt = 0;
	while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NTP_WAIT_MS)) != ESP_OK) {
		ESP_LOGW(TAG, "NTP not synchronized after attempt %d, telemetry stays paused", ++attempt);
	}
	ESP_LOGI(TAG, "NTP synchronized, UTC offset is %d minutes", CONFIG_SAVI_UTC_OFFSET_MINUTES);
	vTaskDelete(NULL);
}

static void time_sync_start(void)
{
	/* POSIX TZ offsets are westward, so UTC-03:00 becomes "UTC+3". */
	char tz_spec[16];
	int offset_minutes = CONFIG_SAVI_UTC_OFFSET_MINUTES;
	snprintf(tz_spec, sizeof(tz_spec), "UTC%+d:%02d", -offset_minutes / 60, abs(offset_minutes) % 60);
	if (setenv("TZ", tz_spec, 1) != 0) {
		ESP_LOGE(TAG, "Cannot set the timezone to %s", tz_spec);
	}
	tzset();

	esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
	ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
	xTaskCreate(time_sync_task, "time_sync", 3072, NULL, 3, NULL);
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
	if (info == NULL || info->src_addr == NULL || savi_msg_type(data, len) == 0) {
		return;
	}

	rx_packet_t pkt = {0};
	memcpy(pkt.mac, info->src_addr, ESP_NOW_ETH_ALEN);
	pkt.len = (len > (int)sizeof(pkt.data)) ? (uint16_t)sizeof(pkt.data) : (uint16_t)len;
	memcpy(pkt.data, data, pkt.len);

	if (xQueueSend(s_rx_queue, &pkt, 0) != pdTRUE) {
		ESP_LOGW(TAG, "RX queue full, dropped a packet from " MACSTR, MAC2STR(pkt.mac));
	}
}

static void gateway_task(void *arg)
{
	(void)arg;
	rx_packet_t pkt;

	while (xQueueReceive(s_rx_queue, &pkt, portMAX_DELAY) == pdTRUE) {
		switch (savi_msg_type(pkt.data, pkt.len)) {
		case SAVI_MSG_HELLO:
			welcome_bracelet(pkt.mac);
			break;
		case SAVI_MSG_NEED_ID:
			if (xQueueSend(s_id_queue, pkt.mac, 0) != pdTRUE) {
				ESP_LOGW(TAG, "ID queue is full, dropping the request from " MACSTR, MAC2STR(pkt.mac));
			}
			break;
		case SAVI_MSG_SENSOR_DATA: {
			if (!savi_msg_valid(pkt.data, pkt.len, SAVI_MSG_SENSOR_DATA, sizeof(savi_sensor_data_msg_t))) {
				ESP_LOGW(TAG, "Truncated telemetry from " MACSTR " (%u bytes)", MAC2STR(pkt.mac), pkt.len);
				break;
			}
			savi_sensor_data_msg_t msg;
			memcpy(&msg, pkt.data, sizeof(msg));
			bracelet_seen(pkt.mac);
			ESP_LOGD(TAG, "RX telemetry id=%" PRIu32 " from " MACSTR " T=%.2fC HR=%u SpO2=%u bat=%u", msg.esp_id, MAC2STR(pkt.mac), msg.temperature_c, msg.hr_bpm, msg.spo2_pct, msg.battery_pct);
			esp_err_t err = mqtt_gateway_publish_telemetry(&msg, pkt.mac);
			if (err != ESP_OK) {
				ESP_LOGW(TAG, "Dropped telemetry from " MACSTR ": %s", MAC2STR(pkt.mac), esp_err_to_name(err));
			}
			break;
		}
		case SAVI_MSG_SENSOR_EVENT: {
			if (!savi_msg_valid(pkt.data, pkt.len, SAVI_MSG_SENSOR_EVENT, sizeof(savi_sensor_event_msg_t))) {
				ESP_LOGW(TAG, "Truncated event from " MACSTR " (%u bytes)", MAC2STR(pkt.mac), pkt.len);
				break;
			}
			savi_sensor_event_msg_t evt;
			memcpy(&evt, pkt.data, sizeof(evt));
			bracelet_seen(pkt.mac);
			if (!savi_event_is_known(evt.event_id)) {
				ESP_LOGW(TAG, "Ignoring unknown event %u from " MACSTR, evt.event_id, MAC2STR(pkt.mac));
				break;
			}
			ESP_LOGW(TAG, "RX event %s from " MACSTR " value=%.2f", savi_event_name(evt.event_id), MAC2STR(pkt.mac), evt.value);
			esp_err_t err = mqtt_gateway_publish_alert(&evt, pkt.mac);
			if (err != ESP_OK) {
				ESP_LOGE(TAG, "Dropped %s alert from " MACSTR ": %s", savi_event_name(evt.event_id), MAC2STR(pkt.mac), esp_err_to_name(err));
			}
			break;
		}
		default:
			/* Another gateway's discovery broadcasts land here: not ours to act on. */
			ESP_LOGD(TAG, "Ignoring %u-byte packet from " MACSTR, pkt.len, MAC2STR(pkt.mac));
			break;
		}
	}
}

static esp_err_t espnow_init(void)
{
	s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_packet_t));
	s_id_queue = xQueueCreate(ID_QUEUE_LEN, ESP_NOW_ETH_ALEN);
	if (s_rx_queue == NULL || s_id_queue == NULL) {
		return ESP_ERR_NO_MEM;
	}

	ESP_ERROR_CHECK(esp_now_init());
	ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

	esp_err_t err = add_broadcast_peer();
	if (err != ESP_OK) {
		return err;
	}

	if (xTaskCreate(gateway_task, "gateway", 8192, NULL, 4, NULL) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	if (xTaskCreate(id_task, "id_alloc", 6144, NULL, 3, NULL) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	if (xTaskCreate(report_task, "gw_report", 4096, NULL, 3, NULL) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}
	return ESP_OK;
}

void app_main(void)
{
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	wifi_init();
	time_sync_start();
	savi_battery_init();
	ESP_ERROR_CHECK(mqtt_gateway_init());
	ESP_ERROR_CHECK(espnow_init());
	ESP_LOGI(TAG, "Gateway ready at %s", CONFIG_SAVI_GATEWAY_LOCAL);
}
