#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "main.h"
#include "sensors.h"
#include <math.h>

#define EVENT_POLL_PERIOD_MS   50
#define SENSOR_SEND_PERIOD_MS  1000
#define LED_BLINK_PERIOD_MS    200
#define ID_REQUEST_PERIOD_MS   2000
#define NVS_NS                 "savi"
#define NVS_KEY_ESP_ID         "esp_id"

static const char *TAG = "sensor";
static const bool s_send_reports_without_master = true;

static const uint8_t s_master_mac[ESP_NOW_ETH_ALEN] = {
	0x88, 0x57, 0x21, 0xAE, 0xAD, 0x94
};

static SemaphoreHandle_t s_send_lock;
static uint32_t s_esp_id;
static QueueHandle_t s_assign_q;

static uint32_t bracelet_id(void)
{
	return s_esp_id;
}

static esp_err_t esp_id_load(void)
{
	nvs_handle_t nvs;
	esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &nvs);
	if (err != ESP_OK) {
		s_esp_id = 0;
		return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
	}
	uint32_t id = 0;
	err = nvs_get_u32(nvs, NVS_KEY_ESP_ID, &id);
	nvs_close(nvs);
	if (err == ESP_ERR_NVS_NOT_FOUND) {
		s_esp_id = 0;
		return ESP_OK;
	}
	if (err == ESP_OK) {
		s_esp_id = id;
	}
	return err;
}

static esp_err_t esp_id_save(uint32_t id)
{
	nvs_handle_t nvs;
	ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &nvs));
	ESP_ERROR_CHECK(nvs_set_u32(nvs, NVS_KEY_ESP_ID, id));
	ESP_ERROR_CHECK(nvs_commit(nvs));
	nvs_close(nvs);
	s_esp_id = id;
	return ESP_OK;
}

static void indicators_init(void)
{
	gpio_config_t io = {
		.pin_bit_mask = (1ULL << PIN_LED) | (1ULL << PIN_BUZZER),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	ESP_ERROR_CHECK(gpio_config(&io));
	gpio_set_level(PIN_LED, 0);
	gpio_set_level(PIN_BUZZER, 0);
}

static void indicators_set_motion(bool active)
{
	static bool led_on;
	static int blink_ms;
	if (!active) {
		led_on = false;
		blink_ms = 0;
		gpio_set_level(PIN_LED, 0);
		gpio_set_level(PIN_BUZZER, 0);
		return;
	}

	gpio_set_level(PIN_BUZZER, 1);
	blink_ms += EVENT_POLL_PERIOD_MS;
	if (blink_ms >= LED_BLINK_PERIOD_MS) {
		blink_ms = 0;
		led_on = !led_on;
		gpio_set_level(PIN_LED, led_on);
	} else if (!led_on && blink_ms == EVENT_POLL_PERIOD_MS) {
		led_on = true;
		gpio_set_level(PIN_LED, 1);
	}
}

static void send_to_master(const void *data, size_t len)
{
	if (xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
		ESP_LOGW(TAG, "Send lock timeout");
		return;
	}

	esp_err_t err = esp_now_send(s_master_mac, (const uint8_t *)data, len);
	xSemaphoreGive(s_send_lock);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
	}
}

static void send_sensor_report(const sensor_readings_t *r)
{
	if (bracelet_id() == 0 && !s_send_reports_without_master) {
		return;
	}

	sensor_data_msg_t msg = {
		.type = MSG_SENSOR_DATA,
		.esp_id = bracelet_id(),
		.temperature_c = r->temperature_c,
		.hr_bpm = r->hr_bpm,
		.spo2_pct = r->spo2_pct,
		.ax = r->ax,
		.ay = r->ay,
		.az = r->az,
		.gx = r->gx,
		.gy = r->gy,
		.gz = r->gz,
	};

	ESP_LOGI(TAG, "TX id=%u  T=%.2fC  HR=%u  SpO2=%u  acc=%.2f,%.2f,%.2f", msg.esp_id, msg.temperature_c, msg.hr_bpm, msg.spo2_pct, msg.ax, msg.ay, msg.az);
	send_to_master(&msg, sizeof(msg));
}

static void sensor_task(void *arg)
{
	(void)arg;
	bool temp_high_latched = false;
	bool motion_latched = false;
	int send_acc_ms = 0;

	while (1) {
		sensor_readings_t r = sensors_read();
		bool temp_high = r.temperature_c >= TEMP_HIGH_THRESHOLD_C;
		if (temp_high && !temp_high_latched && bracelet_id() != 0) {
			sensor_event_msg_t evt = {
				.type = MSG_SENSOR_EVENT,
				.esp_id = bracelet_id(),
				.event_id = EVENT_TEMP_HIGH,
				.value = r.temperature_c,
			};
			ESP_LOGW(TAG, "TX event id=%u TEMP_HIGH  value=%.2fC", evt.esp_id, evt.value);
			send_to_master(&evt, sizeof(evt));
		}
		temp_high_latched = temp_high;
		indicators_set_motion(r.motion);
		if (r.motion && !motion_latched && bracelet_id() != 0) {
			float mag = sqrtf(r.ax * r.ax + r.ay * r.ay + r.az * r.az);
			sensor_event_msg_t evt = {
				.type = MSG_SENSOR_EVENT,
				.esp_id = bracelet_id(),
				.event_id = EVENT_MOTION_DETECTED,
				.value = mag,
			};
			ESP_LOGW(TAG, "TX event id=%u MOTION  |a|=%.2fg", evt.esp_id, mag);
			send_to_master(&evt, sizeof(evt));
		}
		motion_latched = r.motion;
		send_acc_ms += EVENT_POLL_PERIOD_MS;
		if (send_acc_ms >= SENSOR_SEND_PERIOD_MS) {
			send_acc_ms = 0;
			send_sensor_report(&r);
		}
		vTaskDelay(pdMS_TO_TICKS(EVENT_POLL_PERIOD_MS));
	}
}

static void wifi_init(void)
{
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
	ESP_ERROR_CHECK(esp_wifi_start());
	ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
	/* Stay awake until an ID is assigned so MSG_ASSIGN_ID is not missed. */
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
	uint8_t mac[6];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	ESP_LOGI(TAG, "ESP-IDF MAC: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void radio_runtime_ps(void)
{
	/* After pairing the slave only transmits; modem sleep between packets. */
	ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
	(void)info;
	if (data == NULL || len < (int)sizeof(assign_id_msg_t)) {
		return;
	}
	if (data[0] != MSG_ASSIGN_ID) {
		return;
	}
	assign_id_msg_t msg;
	memcpy(&msg, data, sizeof(msg));
	if (msg.esp_id != 0) {
		xQueueSend(s_assign_q, &msg.esp_id, 0);
	}
}

static void id_request_task(void *arg)
{
	(void)arg;
	uint8_t need = MSG_NEED_ID;
	while (bracelet_id() == 0) {
		ESP_LOGI(TAG, "No ESP ID yet, requesting from master");
		send_to_master(&need, sizeof(need));

		uint32_t id = 0;
		if (xQueueReceive(s_assign_q, &id, pdMS_TO_TICKS(ID_REQUEST_PERIOD_MS)) == pdTRUE && id != 0) {
			ESP_ERROR_CHECK(esp_id_save(id));
			ESP_LOGI(TAG, "Assigned ESP ID %u", id);
			radio_runtime_ps();
			break;
		}
	}
	vTaskDelete(NULL);
}

static void on_send(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
	if (info == NULL) {
		return;
	}
	ESP_LOGD(TAG, "Send status %d", (int)status);
}

static esp_err_t espnow_init(void)
{
	ESP_ERROR_CHECK(esp_now_init());
	ESP_ERROR_CHECK(esp_now_register_send_cb(on_send));
	ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

	esp_now_peer_info_t peer = {0};
	peer.channel = CONFIG_ESPNOW_CHANNEL;
	peer.ifidx = ESPNOW_WIFI_IF;
	peer.encrypt = false;
	memcpy(peer.peer_addr, s_master_mac, ESP_NOW_ETH_ALEN);
	ESP_ERROR_CHECK(esp_now_add_peer(&peer));
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

	s_send_lock = xSemaphoreCreateMutex();
	s_assign_q = xQueueCreate(4, sizeof(uint32_t));
	configASSERT(s_send_lock && s_assign_q);
	ESP_ERROR_CHECK(esp_id_load());
	ESP_ERROR_CHECK(sensors_init());
	indicators_init();
	wifi_init();
	ESP_ERROR_CHECK(espnow_init());
	if (s_esp_id == 0) {
		ESP_LOGI(TAG, "Bracelet ESP ID is unset; waiting for master");
		xTaskCreate(id_request_task, "id_request", 3072, NULL, 5, NULL);
	} else {
		ESP_LOGI(TAG, "Bracelet ESP ID: %u", s_esp_id);
		radio_runtime_ps();
	}
	xTaskCreate(sensor_task, "sensors", 4096, NULL, 5, NULL);
}
