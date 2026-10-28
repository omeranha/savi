#include <inttypes.h>
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
#include "esp_random.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "main.h"
#include "savi_proto.h"
#include "sensors.h"
#include <math.h>

#define EVENT_POLL_PERIOD_MS   50
#define SENSOR_SEND_PERIOD_MS  1000
#define LED_BLINK_PERIOD_MS    200
#define DISCOVERY_WINDOW_MS    600
#define HELLO_RETRY_MIN_MS     2000
#define HELLO_RETRY_MAX_MS     30000
/* The gateway has to reach the middleware over MQTT for this, so allow for it. */
#define ID_WAIT_MS             25000
#define LINK_PROBE_PERIOD_MS   300000
#define LINK_LOST_FAILS        10
#define CHANNEL_MAX            13
/* 120 polls x 50 ms, the demo drain step. */
#define DEMO_BATTERY_STEP_POLLS 120
#define NVS_NS                 "savi"
#define NVS_KEY_CHANNEL        "channel"
#define NVS_KEY_GATEWAY        "gateway"
#define NVS_KEY_ESP_ID         "esp_id"

/* Why the link task was woken. */
#define LINK_BIT_LOST          BIT0  /* sends stopped being acked: drop this gateway */
#define LINK_BIT_REPROBE       BIT1  /* probe again now */

_Static_assert(SAVI_BATTERY_PERCENT_UNKNOWN == SAVI_BATTERY_PCT_UNKNOWN, "savi_battery.h and savi_proto.h disagree on the unknown battery value");
_Static_assert(SAVI_MAC_LEN == ESP_NOW_ETH_ALEN, "savi_proto.h and esp_now.h disagree on the MAC length");

static const char *TAG = "sensor";

static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};
static const uint8_t s_zero_mac[ESP_NOW_ETH_ALEN] = {0};

typedef struct {
	uint8_t mac[ESP_NOW_ETH_ALEN];
	uint8_t channel;
	int8_t rssi;
} gateway_candidate_t;

static SemaphoreHandle_t s_send_lock;
static QueueHandle_t s_welcome_q;
static QueueHandle_t s_assign_q;
static TaskHandle_t s_link_task;
static volatile bool s_welcomed;
static volatile uint32_t s_esp_id;
static volatile uint8_t s_channel;
static volatile int s_send_fails;
static uint8_t s_own_mac[ESP_NOW_ETH_ALEN];
static uint8_t s_saved_channel;
static uint32_t s_saved_esp_id;
static uint8_t s_saved_gateway[ESP_NOW_ETH_ALEN];

static uint32_t bracelet_id(void)
{
	return s_esp_id;
}

/* Written by the link task, read by the sensor task and the receive callback. */
static portMUX_TYPE s_gateway_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_gateway_mac[ESP_NOW_ETH_ALEN];

static bool gateway_get(uint8_t *out)
{
	taskENTER_CRITICAL(&s_gateway_lock);
	memcpy(out, s_gateway_mac, ESP_NOW_ETH_ALEN);
	taskEXIT_CRITICAL(&s_gateway_lock);
	for (int i = 0; i < ESP_NOW_ETH_ALEN; i++) {
		if (out[i] != 0) {
			return true;
		}
	}
	return false;
}

static bool gateway_is(const uint8_t *mac)
{
	uint8_t current[ESP_NOW_ETH_ALEN];
	return gateway_get(current) && memcmp(current, mac, ESP_NOW_ETH_ALEN) == 0;
}

static void gateway_store(const uint8_t *mac)
{
	taskENTER_CRITICAL(&s_gateway_lock);
	memcpy(s_gateway_mac, mac, ESP_NOW_ETH_ALEN);
	taskEXIT_CRITICAL(&s_gateway_lock);
}

#if CONFIG_SAVI_DEMO_MODE
static float demo_random_float(float min, float max)
{
	return min + ((float)(esp_random() % 10001) / 10000.0f) * (max - min);
}

/* Drains 1% about every 6 s and wraps, so the gauge and the BATTERY_LOW path
 * can be exercised without the divider wired. */
static uint8_t demo_battery_pct(void)
{
	static int polls;
	return (uint8_t)(100 - (polls++ / DEMO_BATTERY_STEP_POLLS) % 101);
}

static sensor_readings_t demo_sensor_readings(void)
{
	sensor_readings_t readings = {
		.temperature_c = 0, // demo_random_float(36.0f, 39.5f),
		.hr_bpm = (uint8_t)(60 + esp_random() % 41),
		.spo2_pct = (uint8_t)(95 + esp_random() % 6),
		.battery_pct = demo_battery_pct(),
		.motion = 0, //(esp_random() % 20) == 0,
	};

	if (readings.motion) {
		readings.ax = demo_random_float(0.5f, 1.0f);
		if (esp_random() & 1) {
			readings.ax = -readings.ax;
		}
		readings.ay = demo_random_float(-0.2f, 0.2f);
		readings.az = demo_random_float(1.3f, 1.6f);
	} else {
		readings.ax = demo_random_float(-0.1f, 0.1f);
		readings.ay = demo_random_float(-0.1f, 0.1f);
		readings.az = demo_random_float(0.9f, 1.1f);
	}
	readings.gx = demo_random_float(-20.0f, 20.0f);
	readings.gy = demo_random_float(-20.0f, 20.0f);
	readings.gz = demo_random_float(-20.0f, 20.0f);
	return readings;
}
#endif

static sensor_readings_t read_sensors(void)
{
#if CONFIG_SAVI_DEMO_MODE
	return demo_sensor_readings();
#else
	return sensors_read();
#endif
}

/* NVS caches the last working gateway, its channel and the assigned esp_id, so a
 * reboot skips both the channel sweep and the trip to the middleware. */
static void link_state_load(void)
{
	nvs_handle_t nvs;
	esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &nvs);
	if (err != ESP_OK) {
		if (err != ESP_ERR_NVS_NOT_FOUND) {
			ESP_LOGW(TAG, "Cannot read NVS namespace %s: %s", NVS_NS, esp_err_to_name(err));
		}
		return;
	}

	uint32_t esp_id = 0;
	if (nvs_get_u32(nvs, NVS_KEY_ESP_ID, &esp_id) == ESP_OK && esp_id != 0) {
		s_esp_id = esp_id;
		s_saved_esp_id = esp_id;
	}
	uint8_t channel = 0;
	if (nvs_get_u8(nvs, NVS_KEY_CHANNEL, &channel) == ESP_OK && channel >= 1 && channel <= CHANNEL_MAX) {
		s_channel = channel;
		s_saved_channel = channel;
	}
	uint8_t mac[ESP_NOW_ETH_ALEN];
	size_t length = sizeof(mac);
	if (nvs_get_blob(nvs, NVS_KEY_GATEWAY, mac, &length) == ESP_OK && length == sizeof(mac)) {
		memcpy(s_saved_gateway, mac, sizeof(mac));
		gateway_store(mac);
	}
	nvs_close(nvs);
}

static void link_state_save(void)
{
	uint8_t mac[ESP_NOW_ETH_ALEN];
	gateway_get(mac);

	nvs_handle_t nvs;
	esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Cannot open NVS for writing: %s", esp_err_to_name(err));
		return;
	}

	esp_err_t set_id = nvs_set_u32(nvs, NVS_KEY_ESP_ID, s_esp_id);
	esp_err_t set_channel = nvs_set_u8(nvs, NVS_KEY_CHANNEL, s_channel);
	esp_err_t set_gateway = nvs_set_blob(nvs, NVS_KEY_GATEWAY, mac, sizeof(mac));
	esp_err_t commit = nvs_commit(nvs);
	nvs_close(nvs);

	if (set_id != ESP_OK || set_channel != ESP_OK || set_gateway != ESP_OK || commit != ESP_OK) {
		ESP_LOGE(TAG, "Cannot persist esp_id=%" PRIu32 " channel=%u, will ask again after reboot", s_esp_id, s_channel);
		return;
	}
	s_saved_esp_id = s_esp_id;
	s_saved_channel = s_channel;
	memcpy(s_saved_gateway, mac, sizeof(mac));
}

static void channel_apply(uint8_t channel)
{
	esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "Cannot switch to channel %u: %s", channel, esp_err_to_name(err));
		return;
	}
	s_channel = channel;
}

static void channel_advance(void)
{
	channel_apply((uint8_t)(s_channel % CHANNEL_MAX) + 1);
	ESP_LOGW(TAG, "Gateway silent, sweeping to channel %u", s_channel);
}

static void radio_listen(bool always_on)
{
	/* Modem sleep saves battery but can miss gateway packets, so stay awake while pairing. */
	esp_err_t err = esp_wifi_set_ps(always_on ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "Cannot change power save mode: %s", esp_err_to_name(err));
	}
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

/* Any event lights the LED and sounds the buzzer; they clear when none is active. */
static void indicators_set_alert(bool active)
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

static void espnow_send(const uint8_t *mac, const void *data, size_t len)
{
	if (xSemaphoreTake(s_send_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
		ESP_LOGW(TAG, "Send lock timeout");
		return;
	}

	esp_err_t err = esp_now_send(mac, (const uint8_t *)data, len);
	xSemaphoreGive(s_send_lock);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
	}
}

static void send_sensor_report(uint32_t esp_id, const sensor_readings_t *r)
{
	uint8_t gateway[ESP_NOW_ETH_ALEN];
	if (!gateway_get(gateway)) {
		return;
	}

	savi_sensor_data_msg_t msg = {
		.version = SAVI_PROTO_VERSION,
		.type = SAVI_MSG_SENSOR_DATA,
		.esp_id = esp_id,
		.temperature_c = r->temperature_c,
		.hr_bpm = r->hr_bpm,
		.spo2_pct = r->spo2_pct,
		.battery_pct = r->battery_pct,
		.ax = r->ax,
		.ay = r->ay,
		.az = r->az,
		.gx = r->gx,
		.gy = r->gy,
		.gz = r->gz,
	};

	ESP_LOGD(TAG, "TX id=%" PRIu32 " T=%.2fC HR=%u SpO2=%u bat=%u%% acc=%.2f,%.2f,%.2f", esp_id, msg.temperature_c, msg.hr_bpm, msg.spo2_pct, msg.battery_pct, msg.ax, msg.ay, msg.az);
	espnow_send(gateway, &msg, sizeof(msg));
}

static void send_sensor_event(uint32_t esp_id, uint8_t event_id, float value)
{
	uint8_t gateway[ESP_NOW_ETH_ALEN];
	if (!gateway_get(gateway)) {
		return;
	}

	savi_sensor_event_msg_t msg = {
		.version = SAVI_PROTO_VERSION,
		.type = SAVI_MSG_SENSOR_EVENT,
		.esp_id = esp_id,
		.event_id = event_id,
		.value = value,
	};

	ESP_LOGW(TAG, "TX event id=%" PRIu32 " %s value=%.2f", esp_id, savi_event_name(event_id), value);
	espnow_send(gateway, &msg, sizeof(msg));
}

static void sensor_task(void *arg)
{
	(void)arg;
	bool temp_high_latched = false;
	bool motion_latched = false;
	bool battery_low_latched = false;
	int send_acc_ms = 0;

	while (1) {
		sensor_readings_t r = read_sensors();
		uint32_t esp_id = bracelet_id();
		bool ready = esp_id != 0 && s_welcomed;

		bool temp_high = r.temperature_c >= TEMP_HIGH_THRESHOLD_C;
		if (temp_high && !temp_high_latched && ready) {
			send_sensor_event(esp_id, SAVI_EVENT_TEMP_HIGH, r.temperature_c);
		}
		temp_high_latched = temp_high;

		bool battery_low = r.battery_pct != SAVI_BATTERY_PERCENT_UNKNOWN && r.battery_pct <= CONFIG_SAVI_BATTERY_LOW_PCT;
		if (battery_low && !battery_low_latched && ready) {
			send_sensor_event(esp_id, SAVI_EVENT_BATTERY_LOW, (float)r.battery_pct);
		}
		battery_low_latched = battery_low;

		if (r.motion && !motion_latched && ready) {
			float magnitude = sqrtf(r.ax * r.ax + r.ay * r.ay + r.az * r.az);
			send_sensor_event(esp_id, SAVI_EVENT_MOTION, magnitude);
		}
		motion_latched = r.motion;

		/* The LED and buzzer answer to any event, not just motion. */
		indicators_set_alert(r.motion || temp_high || battery_low);

		send_acc_ms += EVENT_POLL_PERIOD_MS;
		if (send_acc_ms >= SENSOR_SEND_PERIOD_MS) {
			send_acc_ms = 0;
			if (ready) {
				send_sensor_report(esp_id, &r);
			} else if (esp_id == 0) {
				ESP_LOGI(TAG, "No esp_id yet, holding sample T=%.2fC HR=%u SpO2=%u", r.temperature_c, r.hr_bpm, r.spo2_pct);
			} else {
				ESP_LOGI(TAG, "No gateway yet, holding sample T=%.2fC HR=%u SpO2=%u", r.temperature_c, r.hr_bpm, r.spo2_pct);
			}
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
	channel_apply(s_channel);
	radio_listen(true);

	uint8_t mac[ESP_NOW_ETH_ALEN];
	esp_read_mac(mac, ESP_MAC_WIFI_STA);
	memcpy(s_own_mac, mac, sizeof(mac));
	ESP_LOGI(TAG, "Bracelet MAC %02X:%02X:%02X:%02X:%02X:%02X on channel %u", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], s_channel);
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
	if (info == NULL || info->src_addr == NULL) {
		return;
	}

	switch (savi_msg_type(data, len)) {
	case SAVI_MSG_WELCOME:
		if (savi_msg_valid(data, len, SAVI_MSG_WELCOME, sizeof(savi_welcome_msg_t))) {
			savi_welcome_msg_t msg;
			memcpy(&msg, data, sizeof(msg));
			/* The answer is broadcast, so check it is the one for this bracelet. */
			if (memcmp(msg.bracelet_mac, s_own_mac, ESP_NOW_ETH_ALEN) != 0) {
				break;
			}
			gateway_candidate_t candidate = {.channel = msg.channel};
			memcpy(candidate.mac, info->src_addr, ESP_NOW_ETH_ALEN);
			candidate.rssi = info->rx_ctrl != NULL ? (int8_t)info->rx_ctrl->rssi : 0;
			xQueueSend(s_welcome_q, &candidate, 0);
		}
		break;
	case SAVI_MSG_ASSIGN_ID:
		if (savi_msg_valid(data, len, SAVI_MSG_ASSIGN_ID, sizeof(savi_assign_id_msg_t))) {
			savi_assign_id_msg_t msg;
			memcpy(&msg, data, sizeof(msg));
			if (msg.esp_id != 0 && memcmp(msg.bracelet_mac, s_own_mac, ESP_NOW_ETH_ALEN) == 0) {
				xQueueSend(s_assign_q, &msg.esp_id, 0);
			}
		}
		break;
	default:
		ESP_LOGD(TAG, "Ignoring %d-byte packet", len);
		break;
	}
}

static void on_send(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
	(void)info;
	if (status == ESP_NOW_SEND_SUCCESS) {
		s_send_fails = 0;
		return;
	}
	if (++s_send_fails >= LINK_LOST_FAILS && s_link_task != NULL) {
		s_send_fails = 0;
		s_welcomed = false;
		xTaskNotify(s_link_task, LINK_BIT_LOST, eSetBits);
	}
}

static esp_err_t espnow_add_peer(const uint8_t *mac)
{
	esp_now_peer_info_t peer = {0};
	peer.channel = 0; /* follow whatever channel the radio is on */
	peer.ifidx = ESPNOW_WIFI_IF;
	peer.encrypt = false;
	memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);

	esp_err_t err = esp_now_add_peer(&peer);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Cannot add peer %02X:%02X:%02X:%02X:%02X:%02X: %s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], esp_err_to_name(err));
	}
	return err;
}

/* Drops the current gateway so the next probe is free to pick another one. */
static void gateway_forget(void)
{
	uint8_t mac[ESP_NOW_ETH_ALEN];
	if (!gateway_get(mac)) {
		return;
	}
	ESP_LOGW(TAG, "Gateway %02X:%02X:%02X:%02X:%02X:%02X stopped acking, looking for another one", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	gateway_store(s_zero_mac);
	esp_now_del_peer(mac);
}

static void gateway_select(const gateway_candidate_t *candidate)
{
	uint8_t previous[ESP_NOW_ETH_ALEN];
	bool switching = gateway_get(previous) && memcmp(previous, candidate->mac, ESP_NOW_ETH_ALEN) != 0;

	if (switching) {
		esp_now_del_peer(previous);
		ESP_LOGW(TAG, "Switching from gateway %02X:%02X:%02X:%02X:%02X:%02X", previous[0], previous[1], previous[2], previous[3], previous[4], previous[5]);
	}
	if (!esp_now_is_peer_exist(candidate->mac) && espnow_add_peer(candidate->mac) != ESP_OK) {
		return;
	}
	gateway_store(candidate->mac);
	s_welcomed = true;

	if (candidate->channel >= 1 && candidate->channel <= CHANNEL_MAX && candidate->channel != s_channel) {
		channel_apply(candidate->channel);
	}
	if (s_saved_channel != s_channel || memcmp(s_saved_gateway, candidate->mac, ESP_NOW_ETH_ALEN) != 0) {
		link_state_save();
	}
	ESP_LOGI(TAG, "Using gateway %02X:%02X:%02X:%02X:%02X:%02X on channel %u (%d dBm)", candidate->mac[0], candidate->mac[1], candidate->mac[2], candidate->mac[3], candidate->mac[4], candidate->mac[5], s_channel, candidate->rssi);
}

/* Every gateway in range answers the probe, so take the strongest one. The
 * current gateway wins on sight: switching only when it stops answering keeps a
 * bracelet from flapping between two gateways that cover the same room. */
static bool discover_gateway(gateway_candidate_t *best)
{
	TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(DISCOVERY_WINDOW_MS);
	gateway_candidate_t candidate;
	bool found = false;

	for (;;) {
		TickType_t now = xTaskGetTickCount();
		if ((int32_t)(deadline - now) <= 0) {
			break;
		}
		if (xQueueReceive(s_welcome_q, &candidate, deadline - now) != pdTRUE) {
			break;
		}
		if (!found || candidate.rssi > best->rssi || gateway_is(candidate.mac)) {
			*best = candidate;
			found = true;
		}
		if (gateway_is(candidate.mac)) {
			break;
		}
	}
	return found;
}

/* Only runs while esp_id is 0. The gateway relays this to the middleware, which
 * hands back the ID already tied to this MAC, or a fresh one if it is new. */
static void request_esp_id(void)
{
	uint8_t gateway[ESP_NOW_ETH_ALEN];
	if (!gateway_get(gateway)) {
		return;
	}

	savi_need_id_msg_t request = {.version = SAVI_PROTO_VERSION, .type = SAVI_MSG_NEED_ID};
	xQueueReset(s_assign_q);
	espnow_send(gateway, &request, sizeof(request));
	ESP_LOGI(TAG, "Asked %02X:%02X:%02X:%02X:%02X:%02X for an esp_id", gateway[0], gateway[1], gateway[2], gateway[3], gateway[4], gateway[5]);

	uint32_t esp_id = 0;
	if (xQueueReceive(s_assign_q, &esp_id, pdMS_TO_TICKS(ID_WAIT_MS)) != pdTRUE) {
		ESP_LOGW(TAG, "No esp_id came back, retrying on the next probe");
		return;
	}

	s_esp_id = esp_id;
	link_state_save();
	ESP_LOGI(TAG, "Assigned esp_id=%" PRIu32, esp_id);
}

static void link_task(void *arg)
{
	(void)arg;
	uint32_t backoff_ms = HELLO_RETRY_MIN_MS;
	uint32_t bits = 0;
	int channels_tried = 0;

	while (1) {
		if ((bits & LINK_BIT_LOST) != 0) {
			gateway_forget();
		}

		savi_hello_msg_t hello = {.version = SAVI_PROTO_VERSION, .type = SAVI_MSG_HELLO};
		radio_listen(true);
		/* Only answers to this probe count, never one left over from the last. */
		xQueueReset(s_welcome_q);
		espnow_send(s_broadcast_mac, &hello, sizeof(hello));

		gateway_candidate_t best;
		if (discover_gateway(&best)) {
			gateway_select(&best);
			backoff_ms = HELLO_RETRY_MIN_MS;
			channels_tried = 0;
			if (bracelet_id() == 0) {
				request_esp_id();
			}
			radio_listen(false);
			xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(LINK_PROBE_PERIOD_MS));
			continue;
		}

		if (s_welcomed) {
			/* Sends are still being acked, so the gateway is there and only the
			 * answer went missing. Stay where we are. */
			ESP_LOGW(TAG, "No answer to the probe, keeping the gateway on channel %u", s_channel);
			radio_listen(false);
			xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(LINK_PROBE_PERIOD_MS));
			continue;
		}

		bits = 0;
		channel_advance();
		if (++channels_tried < CHANNEL_MAX) {
			continue;
		}

		/* A full sweep found nothing: back off before burning more radio time. */
		channels_tried = 0;
		ESP_LOGW(TAG, "No gateway on any channel, retrying in %" PRIu32 " ms", backoff_ms);
		radio_listen(false);
		xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(backoff_ms));
		backoff_ms = backoff_ms >= HELLO_RETRY_MAX_MS / 2 ? HELLO_RETRY_MAX_MS : backoff_ms * 2;
	}
}

static esp_err_t espnow_init(void)
{
	ESP_ERROR_CHECK(esp_now_init());
	ESP_ERROR_CHECK(esp_now_register_send_cb(on_send));
	ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

	/* Needed to send the probe. The gateway peer is added by the link task once
	 * an answer tells it which gateway to use. */
	return espnow_add_peer(s_broadcast_mac);
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
	s_welcome_q = xQueueCreate(8, sizeof(gateway_candidate_t));
	s_assign_q = xQueueCreate(2, sizeof(uint32_t));
	configASSERT(s_send_lock && s_welcome_q && s_assign_q);

	s_channel = CONFIG_ESPNOW_CHANNEL;
	link_state_load();
#if CONFIG_SAVI_DEMO_MODE
	ESP_LOGW(TAG, "Demo mode: vitals are synthetic, no I2C sensor is read");
#else
	ESP_ERROR_CHECK(sensors_init());
#endif
	indicators_init();
	wifi_init();
	ESP_ERROR_CHECK(espnow_init());

	uint8_t saved[ESP_NOW_ETH_ALEN];
	if (s_esp_id != 0) {
		ESP_LOGI(TAG, "Restored esp_id=%" PRIu32 " from NVS", s_esp_id);
	} else {
		ESP_LOGI(TAG, "No esp_id in NVS, will ask the first gateway that answers");
	}
	if (gateway_get(saved)) {
		ESP_LOGI(TAG, "Last gateway was %02X:%02X:%02X:%02X:%02X:%02X on channel %u, probing for it first", saved[0], saved[1], saved[2], saved[3], saved[4], saved[5], s_channel);
	} else {
		ESP_LOGI(TAG, "No gateway in NVS, sweeping channels until one answers");
	}
	xTaskCreate(link_task, "link", 4096, NULL, 5, &s_link_task);
	xTaskCreate(sensor_task, "sensors", 4096, NULL, 5, NULL);
}
