#ifndef MQTT_GATEWAY_H
#define MQTT_GATEWAY_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_now.h"
#include "main.h"
#include "savi_proto.h"

esp_err_t mqtt_gateway_init(void);

/* Relays one bracelet packet. The bracelet MAC comes from the ESP-NOW frame
 * header, so a bracelet cannot report under another one's identity. */
esp_err_t mqtt_gateway_publish_telemetry(const savi_sensor_data_msg_t *msg, const uint8_t mac[ESP_NOW_ETH_ALEN]);
esp_err_t mqtt_gateway_publish_alert(const savi_sensor_event_msg_t *evt, const uint8_t mac[ESP_NOW_ETH_ALEN]);

/* The gateway's own minute report: battery and how many bracelets are streaming
 * to it right now. */
esp_err_t mqtt_gateway_publish_status(uint8_t battery_pct, int bracelet_count);

/* Blocks on the middleware, so call it from the ID task and never from the
 * packet path. Returns the esp_id already tied to this MAC, or a new one. */
esp_err_t mqtt_gateway_request_esp_id(const uint8_t mac[ESP_NOW_ETH_ALEN], uint32_t *esp_id);

#endif
