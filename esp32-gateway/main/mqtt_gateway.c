#include "mqtt_gateway.h"

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#define MQTT_CONNECTED_BIT       BIT0
#define MQTT_ID_SUBSCRIBED_BIT   BIT1
#define MQTT_ID_RESPONSE_BIT     BIT2
#define MQTT_ID_TIMEOUT_MS       20000
#define MQTT_ID_PAYLOAD_MAX      128
#define MAC_STRING_LEN           18
#define TIMESTAMP_MAX            40

/* Anything before 2025 means SNTP has not landed yet, so there is no usable time. */
#define CLOCK_VALID_EPOCH_MS     1735689600000LL

#define MQTT_TELEMETRY_TOPIC     "savi/telemetry"
#define MQTT_ALERT_TOPIC         "savi/alert"
#define MQTT_STATUS_TOPIC        "savi/gateway"
#define MQTT_OFFLINE_TOPIC       "savi/gateway/offline"
#define MQTT_ID_REQUEST_TOPIC    "savi/id/request"
#define MQTT_ID_RESPONSE_TOPIC   "savi/id/response"

static const char *TAG = "mqtt_gateway";

static esp_mqtt_client_handle_t s_client;
static EventGroupHandle_t s_mqtt_events;
static SemaphoreHandle_t s_id_mutex;
static int s_id_subscribe_message_id = -1;
static bool s_waiting_for_id;
static char s_pending_mac[MAC_STRING_LEN];
static uint32_t s_response_id;
static char s_own_mac[MAC_STRING_LEN];
static char s_offline_payload[64];

static esp_err_t mac_to_string(const uint8_t mac[ESP_NOW_ETH_ALEN], char *out, size_t out_len)
{
	int length = snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	return length == 17 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* ISO-8601 with the offset from CONFIG_SAVI_UTC_OFFSET_MINUTES. Returns
 * ESP_ERR_INVALID_STATE before NTP lands, and the caller then leaves the field
 * out so PostgreSQL stamps the row instead of the reading being lost. */
static esp_err_t now_timestamp(char *out, size_t out_len)
{
	struct timeval now;
	if (gettimeofday(&now, NULL) != 0) {
		return ESP_FAIL;
	}

	int64_t now_ms = (int64_t)now.tv_sec * 1000 + now.tv_usec / 1000;
	if (now_ms < CLOCK_VALID_EPOCH_MS) {
		return ESP_ERR_INVALID_STATE;
	}

	const int offset_minutes = CONFIG_SAVI_UTC_OFFSET_MINUTES;
	time_t shifted_seconds = (time_t)(now_ms / 1000) + (time_t)offset_minutes * 60;
	struct tm shifted;
	if (gmtime_r(&shifted_seconds, &shifted) == NULL) {
		return ESP_FAIL;
	}

	size_t length = strftime(out, out_len, "%Y-%m-%dT%H:%M:%S", &shifted);
	if (length != 19) {
		return ESP_FAIL;
	}
	int written = snprintf(out + length, out_len - length, ".%03d%+03d:%02d", (int)(now_ms % 1000), offset_minutes / 60, abs(offset_minutes) % 60);
	return written == 10 ? ESP_OK : ESP_FAIL;
}

/* Adds atualizado_em when the clock is good, and says nothing when it is not. */
static bool add_timestamp(cJSON *json)
{
	char timestamp[TIMESTAMP_MAX];
	if (now_timestamp(timestamp, sizeof(timestamp)) != ESP_OK) {
		return true;
	}
	return cJSON_AddStringToObject(json, "atualizado_em", timestamp) != NULL;
}

static esp_err_t publish_json(const char *topic, const cJSON *json)
{
	if ((xEventGroupGetBits(s_mqtt_events) & MQTT_CONNECTED_BIT) == 0) {
		ESP_LOGE(TAG, "Cannot publish to %s: MQTT is disconnected", topic);
		return ESP_ERR_INVALID_STATE;
	}

	char *payload = cJSON_PrintUnformatted(json);
	if (payload == NULL) {
		return ESP_ERR_NO_MEM;
	}

	int message_id = esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
	cJSON_free(payload);
	if (message_id < 0) {
		ESP_LOGE(TAG, "MQTT publish to %s failed", topic);
		return ESP_FAIL;
	}
	return ESP_OK;
}

static void handle_id_response(const char *payload)
{
	cJSON *json = cJSON_Parse(payload);
	if (json == NULL) {
		ESP_LOGW(TAG, "Ignoring malformed MQTT ID response");
		return;
	}

	const cJSON *mac = cJSON_GetObjectItemCaseSensitive(json, "mac");
	const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "esp_id");
	if (!cJSON_IsString(mac) || !cJSON_IsNumber(id) ||
		id->valuedouble < 1 || id->valuedouble > UINT32_MAX ||
		(double)(uint32_t)id->valuedouble != id->valuedouble) {
		ESP_LOGW(TAG, "Ignoring invalid MQTT ID response");
		cJSON_Delete(json);
		return;
	}

	xSemaphoreTake(s_id_mutex, portMAX_DELAY);
	if (s_waiting_for_id && strcasecmp(mac->valuestring, s_pending_mac) == 0) {
		s_response_id = (uint32_t)id->valuedouble;
		xEventGroupSetBits(s_mqtt_events, MQTT_ID_RESPONSE_BIT);
	} else {
		ESP_LOGD(TAG, "Ignoring MQTT ID response for %s", mac->valuestring);
	}
	xSemaphoreGive(s_id_mutex);
	cJSON_Delete(json);
}

static bool is_id_response_topic(const esp_mqtt_event_handle_t event)
{
	return event->topic != NULL &&
		   event->topic_len == (int)strlen(MQTT_ID_RESPONSE_TOPIC) &&
		   memcmp(event->topic, MQTT_ID_RESPONSE_TOPIC, event->topic_len) == 0;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
	(void)handler_args;
	(void)base;
	if (event_data == NULL) {
		return;
	}
	esp_mqtt_event_handle_t event = event_data;

	switch ((esp_mqtt_event_id_t)event_id) {
	case MQTT_EVENT_CONNECTED:
		xEventGroupSetBits(s_mqtt_events, MQTT_CONNECTED_BIT);
		xEventGroupClearBits(s_mqtt_events, MQTT_ID_SUBSCRIBED_BIT);
		s_id_subscribe_message_id = esp_mqtt_client_subscribe(s_client, MQTT_ID_RESPONSE_TOPIC, 1);
		if (s_id_subscribe_message_id < 0) {
			ESP_LOGE(TAG, "Failed to subscribe to %s", MQTT_ID_RESPONSE_TOPIC);
		}
		ESP_LOGI(TAG, "Connected to MQTT broker");
		break;
	case MQTT_EVENT_DISCONNECTED:
		xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT | MQTT_ID_SUBSCRIBED_BIT);
		ESP_LOGW(TAG, "Disconnected from MQTT broker");
		break;
	case MQTT_EVENT_SUBSCRIBED:
		if (event->msg_id == s_id_subscribe_message_id) {
			xEventGroupSetBits(s_mqtt_events, MQTT_ID_SUBSCRIBED_BIT);
			ESP_LOGI(TAG, "Subscribed to %s", MQTT_ID_RESPONSE_TOPIC);
		}
		break;
	case MQTT_EVENT_DATA:
		/* ID responses are a few dozen bytes, so they arrive in one piece. */
		if (is_id_response_topic(event) && event->data != NULL &&
			event->current_data_offset == 0 && event->data_len == event->total_data_len &&
			event->data_len > 0 && event->data_len < MQTT_ID_PAYLOAD_MAX) {
			char payload[MQTT_ID_PAYLOAD_MAX];
			memcpy(payload, event->data, (size_t)event->data_len);
			payload[event->data_len] = '\0';
			handle_id_response(payload);
		} else if (is_id_response_topic(event)) {
			ESP_LOGW(TAG, "Discarding an oversized or split MQTT ID response");
		}
		break;
	case MQTT_EVENT_ERROR:
		ESP_LOGE(TAG, "MQTT client reported a transport error");
		break;
	default:
		break;
	}
}

esp_err_t mqtt_gateway_init(void)
{
	if (strncmp(CONFIG_MQTT_BROKER_URI, "mqtt://", 7) != 0 && strncmp(CONFIG_MQTT_BROKER_URI, "mqtts://", 8) != 0) {
		ESP_LOGE(TAG, "Configure an MQTT broker URI (mqtt:// or mqtts://) in menuconfig");
		return ESP_ERR_INVALID_ARG;
	}

	s_mqtt_events = xEventGroupCreate();
	s_id_mutex = xSemaphoreCreateMutex();
	if (s_mqtt_events == NULL || s_id_mutex == NULL) {
		return ESP_ERR_NO_MEM;
	}

	uint8_t mac[ESP_NOW_ETH_ALEN];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	if (mac_to_string(mac, s_own_mac, sizeof(s_own_mac)) != ESP_OK) {
		return ESP_ERR_INVALID_SIZE;
	}
	/* The broker publishes this for us when we stop answering keepalives, which
	 * is what clears is_connected without the middleware polling anything. */
	int written = snprintf(s_offline_payload, sizeof(s_offline_payload), "{\"mac\":\"%s\"}", s_own_mac);
	if (written <= 0 || written >= (int)sizeof(s_offline_payload)) {
		return ESP_ERR_INVALID_SIZE;
	}

	const esp_mqtt_client_config_t config = {
		.broker.address.uri = CONFIG_MQTT_BROKER_URI,
		.broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
		.credentials.username = CONFIG_MQTT_USERNAME,
		.credentials.authentication.password = CONFIG_MQTT_PASSWORD,
		.session.keepalive = 60,
		.session.last_will = {
			.topic = MQTT_OFFLINE_TOPIC,
			.msg = s_offline_payload,
			.msg_len = written,
			.qos = 1,
			.retain = 0,
		},
	};
	s_client = esp_mqtt_client_init(&config);
	if (s_client == NULL) {
		return ESP_ERR_NO_MEM;
	}
	ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
	ESP_ERROR_CHECK(esp_mqtt_client_start(s_client));
	return ESP_OK;
}

esp_err_t mqtt_gateway_publish_telemetry(const savi_sensor_data_msg_t *msg, const uint8_t mac[ESP_NOW_ETH_ALEN])
{
	if (msg == NULL || mac == NULL || !isfinite(msg->temperature_c) || !isfinite(msg->ax) ||
		!isfinite(msg->ay) || !isfinite(msg->az) || !isfinite(msg->gx) ||
		!isfinite(msg->gy) || !isfinite(msg->gz)) {
		return ESP_ERR_INVALID_ARG;
	}

	char mac_string[MAC_STRING_LEN];
	if (mac_to_string(mac, mac_string, sizeof(mac_string)) != ESP_OK) {
		return ESP_ERR_INVALID_SIZE;
	}

	cJSON *json = cJSON_CreateObject();
	if (json == NULL) {
		return ESP_ERR_NO_MEM;
	}
	bool added =
		cJSON_AddStringToObject(json, "mac", mac_string) != NULL &&
		cJSON_AddNumberToObject(json, "esp_id", msg->esp_id) != NULL &&
		add_timestamp(json) &&
		cJSON_AddNumberToObject(json, "temperatura", msg->temperature_c) != NULL &&
		cJSON_AddNumberToObject(json, "bpm", msg->hr_bpm) != NULL &&
		cJSON_AddNumberToObject(json, "spo2", msg->spo2_pct) != NULL &&
		cJSON_AddNumberToObject(json, "acc_x", msg->ax) != NULL &&
		cJSON_AddNumberToObject(json, "acc_y", msg->ay) != NULL &&
		cJSON_AddNumberToObject(json, "acc_z", msg->az) != NULL &&
		cJSON_AddNumberToObject(json, "gyro_x", msg->gx) != NULL &&
		cJSON_AddNumberToObject(json, "gyro_y", msg->gy) != NULL &&
		cJSON_AddNumberToObject(json, "gyro_z", msg->gz) != NULL;
	/* bateria is left out when the bracelet has no gauge, so Supabase keeps its last value. */
	if (added && savi_battery_pct_valid(msg->battery_pct)) {
		added = cJSON_AddNumberToObject(json, "bateria", msg->battery_pct) != NULL;
	}
	esp_err_t err = added ? publish_json(MQTT_TELEMETRY_TOPIC, json) : ESP_ERR_NO_MEM;
	cJSON_Delete(json);
	return err;
}

esp_err_t mqtt_gateway_publish_alert(const savi_sensor_event_msg_t *evt, const uint8_t mac[ESP_NOW_ETH_ALEN])
{
	if (evt == NULL || mac == NULL || !isfinite(evt->value)) {
		return ESP_ERR_INVALID_ARG;
	}

	char mac_string[MAC_STRING_LEN];
	if (mac_to_string(mac, mac_string, sizeof(mac_string)) != ESP_OK) {
		return ESP_ERR_INVALID_SIZE;
	}

	cJSON *json = cJSON_CreateObject();
	if (json == NULL) {
		return ESP_ERR_NO_MEM;
	}
	bool added = cJSON_AddStringToObject(json, "mac", mac_string) != NULL &&
				 cJSON_AddNumberToObject(json, "esp_id", evt->esp_id) != NULL &&
				 cJSON_AddStringToObject(json, "tipo", savi_event_name(evt->event_id)) != NULL &&
				 cJSON_AddNumberToObject(json, "valor", evt->value) != NULL;
	esp_err_t err = added ? publish_json(MQTT_ALERT_TOPIC, json) : ESP_ERR_NO_MEM;
	cJSON_Delete(json);
	return err;
}

esp_err_t mqtt_gateway_publish_status(uint8_t battery_pct, int bracelet_count)
{
	if (bracelet_count < 0) {
		return ESP_ERR_INVALID_ARG;
	}

	cJSON *json = cJSON_CreateObject();
	if (json == NULL) {
		return ESP_ERR_NO_MEM;
	}
	bool added = cJSON_AddStringToObject(json, "mac", s_own_mac) != NULL &&
				 cJSON_AddStringToObject(json, "local", CONFIG_SAVI_GATEWAY_LOCAL) != NULL &&
				 cJSON_AddNumberToObject(json, "braceletes", bracelet_count) != NULL &&
				 add_timestamp(json);
	/* Same rule as the bracelet: no gauge, no column written. */
	if (added && savi_battery_pct_valid(battery_pct)) {
		added = cJSON_AddNumberToObject(json, "battery_percentage", battery_pct) != NULL;
	}
	esp_err_t err = added ? publish_json(MQTT_STATUS_TOPIC, json) : ESP_ERR_NO_MEM;
	cJSON_Delete(json);
	return err;
}

esp_err_t mqtt_gateway_request_esp_id(const uint8_t mac[ESP_NOW_ETH_ALEN], uint32_t *esp_id)
{
	if (mac == NULL || esp_id == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	EventBits_t bits = xEventGroupWaitBits(s_mqtt_events, MQTT_CONNECTED_BIT | MQTT_ID_SUBSCRIBED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(MQTT_ID_TIMEOUT_MS));
	if ((bits & (MQTT_CONNECTED_BIT | MQTT_ID_SUBSCRIBED_BIT)) != (MQTT_CONNECTED_BIT | MQTT_ID_SUBSCRIBED_BIT)) {
		ESP_LOGE(TAG, "MQTT broker is not ready to allocate an ID");
		return ESP_ERR_TIMEOUT;
	}

	char mac_string[MAC_STRING_LEN];
	if (mac_to_string(mac, mac_string, sizeof(mac_string)) != ESP_OK) {
		return ESP_ERR_INVALID_SIZE;
	}
	cJSON *json = cJSON_CreateObject();
	if (json == NULL) {
		return ESP_ERR_NO_MEM;
	}
	if (cJSON_AddStringToObject(json, "mac", mac_string) == NULL) {
		cJSON_Delete(json);
		return ESP_ERR_NO_MEM;
	}

	xEventGroupClearBits(s_mqtt_events, MQTT_ID_RESPONSE_BIT);
	xSemaphoreTake(s_id_mutex, portMAX_DELAY);
	s_waiting_for_id = true;
	s_response_id = 0;
	memcpy(s_pending_mac, mac_string, sizeof(s_pending_mac));
	xSemaphoreGive(s_id_mutex);

	esp_err_t err = publish_json(MQTT_ID_REQUEST_TOPIC, json);
	cJSON_Delete(json);
	if (err != ESP_OK) {
		xSemaphoreTake(s_id_mutex, portMAX_DELAY);
		s_waiting_for_id = false;
		xSemaphoreGive(s_id_mutex);
		return err;
	}

	bits = xEventGroupWaitBits(s_mqtt_events, MQTT_ID_RESPONSE_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(MQTT_ID_TIMEOUT_MS));
	xSemaphoreTake(s_id_mutex, portMAX_DELAY);
	s_waiting_for_id = false;
	if ((bits & MQTT_ID_RESPONSE_BIT) != 0 && s_response_id != 0) {
		*esp_id = s_response_id;
		err = ESP_OK;
	} else {
		ESP_LOGE(TAG, "Timed out waiting for an ID response for %s", mac_string);
		err = ESP_ERR_TIMEOUT;
	}
	xSemaphoreGive(s_id_mutex);
	return err;
}
