#ifndef SAVI_PROTO_H
#define SAVI_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ESP-NOW wire protocol shared by the SAVI bracelet (esp32-c6) and the gateway
 * (esp32-gateway). Both ends validate the version byte, so bump the version on
 * any layout change and reflash both sides together.
 *
 * Two separate conversations, on purpose:
 *
 *   discovery   HELLO -> WELCOME. Answered by the gateway itself in microseconds,
 *               so a bracelet can find a gateway, or replace one that went out of
 *               range, without the middleware being involved at all.
 *   identity    NEED_ID -> ASSIGN_ID. Only when the bracelet has no esp_id. The
 *               gateway has to ask the middleware over MQTT, which is slow, so
 *               this is kept off the failover path. Once an esp_id is in NVS the
 *               bracelet never asks again.
 *
 * Gateway replies are broadcast. A bracelet learns a gateway's MAC from the frame
 * header, and ESP-NOW only lets you unicast to a peer you already hold, so a
 * broadcast reply carrying bracelet_mac avoids any peer bookkeeping on the
 * gateway: it keeps one peer, the broadcast address, no matter how many bracelets
 * it serves.
 *
 * Nothing here is encrypted. ESP-NOW cannot encrypt broadcast, and its unicast
 * encryption needs a key shared by every device, which is no secret once it ships
 * in firmware. Vitals travel in the clear on this hop.
 */
#define SAVI_PROTO_VERSION 7

#define SAVI_MAC_LEN 6

/* battery_pct when there is no battery reading (no divider wired, or ADC error). */
#define SAVI_BATTERY_PCT_UNKNOWN 0xFF

typedef enum {
	SAVI_MSG_SENSOR_DATA = 1,
	SAVI_MSG_SENSOR_EVENT = 2,
	SAVI_MSG_HELLO = 3,
	SAVI_MSG_WELCOME = 4,
	SAVI_MSG_NEED_ID = 5,
	SAVI_MSG_ASSIGN_ID = 6,
} savi_msg_type_t;

typedef enum {
	SAVI_EVENT_TEMP_HIGH = 1,
	SAVI_EVENT_MOTION = 2,
	SAVI_EVENT_BATTERY_LOW = 3,
} savi_event_id_t;

typedef struct {
	uint8_t version;
	uint8_t type;
} __attribute__((packed)) savi_header_t;

typedef struct {
	uint8_t version;
	uint8_t type;            /* SAVI_MSG_SENSOR_DATA */
	uint32_t esp_id;
	float temperature_c;     /* MAX30205 */
	uint8_t hr_bpm;          /* MAX30102 */
	uint8_t spo2_pct;        /* MAX30102 */
	uint8_t battery_pct;     /* ADC divider, SAVI_BATTERY_PCT_UNKNOWN when unavailable */
	float ax;                /* BMI270, g */
	float ay;
	float az;
	float gx;                /* BMI270, dps */
	float gy;
	float gz;
} __attribute__((packed)) savi_sensor_data_msg_t;

typedef struct {
	uint8_t version;
	uint8_t type;            /* SAVI_MSG_SENSOR_EVENT */
	uint32_t esp_id;
	uint8_t event_id;        /* savi_event_id_t */
	float value;
} __attribute__((packed)) savi_sensor_event_msg_t;

/* Probes for any gateway in range. */
typedef struct {
	uint8_t version;
	uint8_t type;            /* SAVI_MSG_HELLO */
} __attribute__((packed)) savi_hello_msg_t;

/* Answers one HELLO: a gateway is here, on this channel. */
typedef struct {
	uint8_t version;
	uint8_t type;            /* SAVI_MSG_WELCOME */
	uint8_t channel;         /* WiFi channel the gateway is live on */
	uint8_t bracelet_mac[SAVI_MAC_LEN];
} __attribute__((packed)) savi_welcome_msg_t;

/* Sent only while esp_id is 0. The gateway forwards the request to the
 * middleware, which owns the IDs. */
typedef struct {
	uint8_t version;
	uint8_t type;            /* SAVI_MSG_NEED_ID */
} __attribute__((packed)) savi_need_id_msg_t;

typedef struct {
	uint8_t version;
	uint8_t type;            /* SAVI_MSG_ASSIGN_ID */
	uint32_t esp_id;         /* never 0 */
	uint8_t bracelet_mac[SAVI_MAC_LEN];
} __attribute__((packed)) savi_assign_id_msg_t;

static inline const char *savi_event_name(uint8_t event_id)
{
	switch (event_id) {
	case SAVI_EVENT_TEMP_HIGH:
		return "TEMP_HIGH";
	case SAVI_EVENT_MOTION:
		return "MOTION";
	case SAVI_EVENT_BATTERY_LOW:
		return "BATTERY_LOW";
	default:
		return "UNKNOWN";
	}
}

static inline bool savi_event_is_known(uint8_t event_id)
{
	return event_id == SAVI_EVENT_TEMP_HIGH || event_id == SAVI_EVENT_MOTION ||
		   event_id == SAVI_EVENT_BATTERY_LOW;
}

static inline bool savi_battery_pct_valid(uint8_t battery_pct)
{
	return battery_pct <= 100;
}

/* True when the buffer holds a complete message of this version and type. */
static inline bool savi_msg_valid(const void *data, int len, uint8_t type, size_t size)
{
	const savi_header_t *header = (const savi_header_t *)data;
	if (data == NULL || size < sizeof(savi_header_t) || len < (int)size) {
		return false;
	}
	return header->version == SAVI_PROTO_VERSION && header->type == type;
}

static inline uint8_t savi_msg_type(const void *data, int len)
{
	const savi_header_t *header = (const savi_header_t *)data;
	if (data == NULL || len < (int)sizeof(savi_header_t) || header->version != SAVI_PROTO_VERSION) {
		return 0;
	}
	return header->type;
}

#endif
