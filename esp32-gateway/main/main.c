#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_netif_sntp.h"
#include "main.h"
#include "supabase_db.h"

#define RX_QUEUE_LEN 8
#define ASSIGN_CACHE_LEN 8
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "gateway";

typedef struct {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    uint16_t len;
    uint8_t data[sizeof(sensor_data_msg_t)];
} rx_packet_t;

static QueueHandle_t s_rx_queue;
static EventGroupHandle_t s_wifi_events;
static int s_retry;

typedef struct {
    uint8_t mac[ESP_NOW_ETH_ALEN];
    uint32_t id;
} assign_cache_t;

static assign_cache_t s_assigned[ASSIGN_CACHE_LEN];
static int s_assigned_n;

static uint32_t assigned_lookup(const uint8_t *mac)
{
    for (int i = 0; i < s_assigned_n; i++) {
        if (memcmp(s_assigned[i].mac, mac, ESP_NOW_ETH_ALEN) == 0) {
            return s_assigned[i].id;
        }
    }
    return 0;
}

static void assigned_store(const uint8_t *mac, uint32_t id)
{
    if (s_assigned_n >= ASSIGN_CACHE_LEN) {
        return;
    }
    memcpy(s_assigned[s_assigned_n].mac, mac, ESP_NOW_ETH_ALEN);
    s_assigned[s_assigned_n].id = id;
    s_assigned_n++;
}

static void ensure_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) {
        return;
    }
    esp_now_peer_info_t peer = {0};
    peer.channel = 0;
    peer.ifidx = ESPNOW_WIFI_IF;
    peer.encrypt = false;
    memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

static void assign_id_to_slave(const uint8_t *mac)
{
    uint32_t id = assigned_lookup(mac);
    if (id == 0) {
        if (supabase_db_alloc_esp_id(&id) != ESP_OK) {
            ESP_LOGE(TAG, "Could not allocate esp_id for " MACSTR, MAC2STR(mac));
            return;
        }
        assigned_store(mac, id);
    }

    assign_id_msg_t msg = {
        .type = MSG_ASSIGN_ID,
        .esp_id = id,
    };
    ensure_peer(mac);
    if (esp_now_send(mac, (uint8_t *)&msg, sizeof(msg)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send id %" PRIu32 " to " MACSTR, id, MAC2STR(mac));
        return;
    }
    ESP_LOGI(TAG, "Assigned esp_id %" PRIu32 " to " MACSTR, id, MAC2STR(mac));
}

static const char *event_name(uint8_t id)
{
    switch (id) {
        case EVENT_TEMP_HIGH:
            return "TEMP_HIGH";
        case EVENT_MOTION_DETECTED:
            return "MOTION";
        default:
            return "UNKNOWN";
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

static void wifi_init(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, CONFIG_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "WiFi connect failed");
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    ESP_ERROR_CHECK(esp_wifi_get_channel(&primary, &second));
    ESP_LOGI(TAG, "STA channel %u - flash the C6 with the same ESPNOW channel", primary);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "ESP-IDF MAC: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void time_sync_init(void)
{
    /* POSIX TZ offsets are westward, so UTC+3 means local time UTC-03:00. */
    if (setenv("TZ", "UTC+3", 1) != 0) {
        ESP_LOGE(TAG, "Failed to configure UTC-3 timezone");
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));

    while (true) {
        esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
        if (err == ESP_OK) {
            break;
        }
        if (err != ESP_ERR_TIMEOUT) {
            ESP_ERROR_CHECK(err);
        }
        ESP_LOGW(TAG, "Waiting for NTP time synchronization");
    }

    ESP_LOGI(TAG, "NTP synchronized; timezone is UTC-03:00");
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || data == NULL || len <= 0) {
        return;
    }

    rx_packet_t pkt = {0};
    memcpy(pkt.mac, info->src_addr, ESP_NOW_ETH_ALEN);
    pkt.len = (len > (int)sizeof(pkt.data)) ? sizeof(pkt.data) : (uint16_t)len;
    memcpy(pkt.data, data, pkt.len);

    if (xQueueSend(s_rx_queue, &pkt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "RX queue full");
    }
}

static void gateway_task(void *arg)
{
    (void)arg;
    rx_packet_t pkt;

    while (xQueueReceive(s_rx_queue, &pkt, portMAX_DELAY) == pdTRUE) {
        if (pkt.len < 1) {
            continue;
        }

        uint8_t type = pkt.data[0];

        if (type == MSG_NEED_ID) {
            assign_id_to_slave(pkt.mac);
        } else if (type == MSG_SENSOR_DATA && pkt.len >= sizeof(sensor_data_msg_t)) {
            sensor_data_msg_t msg;
            memcpy(&msg, pkt.data, sizeof(msg));
            if (msg.esp_id == 0) {
                assign_id_to_slave(pkt.mac);
                continue;
            }
            ESP_LOGI(TAG, "RX sensor id=%" PRIu32 "  T=%.2fC  HR=%u  SpO2=%u",
                     msg.esp_id, msg.temperature_c, msg.hr_bpm, msg.spo2_pct);
            if (supabase_db_update_bracelete(&msg) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to update braceletes for %" PRIu32, msg.esp_id);
            }
        } else if (type == MSG_SENSOR_EVENT && pkt.len >= sizeof(sensor_event_msg_t)) {
            sensor_event_msg_t evt;
            memcpy(&evt, pkt.data, sizeof(evt));
            if (evt.esp_id == 0) {
                assign_id_to_slave(pkt.mac);
                continue;
            }
            ESP_LOGW(TAG, "RX event id=%" PRIu32 "  %s  value=%.1f",
                     evt.esp_id, event_name(evt.event_id), evt.value);
            if (evt.event_id == EVENT_MOTION_DETECTED) {
                if (supabase_db_insert_alerta(evt.esp_id, "MOTION", evt.value) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to insert alerta for %" PRIu32, evt.esp_id);
                }
            }
        } else {
            ESP_LOGW(TAG, "RX unknown packet type=%u len=%u", type, pkt.len);
        }
    }
}

static esp_err_t espnow_init(void)
{
    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_packet_t));
    if (s_rx_queue == NULL) {
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

    xTaskCreate(gateway_task, "gateway", 8192, NULL, 4, NULL);
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
    time_sync_init();
    ESP_ERROR_CHECK(supabase_db_init());
    ESP_ERROR_CHECK(espnow_init());
    ESP_LOGI(TAG, "Gateway ready");

}
