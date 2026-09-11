#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

#if CONFIG_ESPNOW_WIFI_MODE_STATION
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF   WIFI_IF_STA
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF   WIFI_IF_AP
#endif

typedef enum {
    MSG_SENSOR_DATA = 1,
    MSG_SENSOR_EVENT = 2,
    MSG_NEED_ID = 3,
    MSG_ASSIGN_ID = 4,
} msg_type_t;

typedef enum {
    EVENT_TEMP_HIGH = 1,
    EVENT_MOTION_DETECTED = 2,
} sensor_event_id_t;

typedef struct {
    uint8_t type;           /* MSG_SENSOR_DATA */
    uint32_t esp_id;
    float temperature_c;    /* MAX30205 */
    uint8_t hr_bpm;         /* MAX30102 */
    uint8_t spo2_pct;       /* MAX30102 */
    float ax;               /* BMI270, g */
    float ay;
    float az;
    float gx;               /* BMI270, dps */
    float gy;
    float gz;
} __attribute__((packed)) sensor_data_msg_t;

typedef struct {
    uint8_t type;           /* MSG_SENSOR_EVENT */
    uint32_t esp_id;
    uint8_t event_id;       /* sensor_event_id_t */
    float value;
} __attribute__((packed)) sensor_event_msg_t;

typedef struct {
    uint8_t type;           /* MSG_ASSIGN_ID */
    uint32_t esp_id;
} __attribute__((packed)) assign_id_msg_t;

#endif
