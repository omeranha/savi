#include "savi_proto.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SAVI_ASSERT_SIZE(type, bytes) typedef char savi_size_check_##type[(sizeof(type) == (bytes)) ? 1 : -1]

SAVI_ASSERT_SIZE(savi_header_t, 2);
SAVI_ASSERT_SIZE(savi_sensor_data_msg_t, 37);
SAVI_ASSERT_SIZE(savi_sensor_event_msg_t, 11);
SAVI_ASSERT_SIZE(savi_hello_msg_t, 2);
SAVI_ASSERT_SIZE(savi_welcome_msg_t, 9);
SAVI_ASSERT_SIZE(savi_need_id_msg_t, 2);
SAVI_ASSERT_SIZE(savi_assign_id_msg_t, 12);

typedef char savi_fits_one_frame[(sizeof(savi_sensor_data_msg_t) <= 250) ? 1 : -1];

int main(void)
{
	savi_sensor_data_msg_t data;
	memset(&data, 0, sizeof(data));
	data.version = SAVI_PROTO_VERSION;
	data.type = SAVI_MSG_SENSOR_DATA;
	data.esp_id = 7;
	data.battery_pct = SAVI_BATTERY_PCT_UNKNOWN;

	assert(savi_msg_valid(&data, sizeof(data), SAVI_MSG_SENSOR_DATA, sizeof(data)));
	assert(!savi_msg_valid(&data, sizeof(data) - 1, SAVI_MSG_SENSOR_DATA, sizeof(data)));
	assert(!savi_msg_valid(&data, sizeof(data), SAVI_MSG_SENSOR_EVENT, sizeof(data)));
	assert(!savi_msg_valid(NULL, sizeof(data), SAVI_MSG_SENSOR_DATA, sizeof(data)));
	assert(savi_msg_type(&data, sizeof(data)) == SAVI_MSG_SENSOR_DATA);

	data.version = 1;
	assert(!savi_msg_valid(&data, sizeof(data), SAVI_MSG_SENSOR_DATA, sizeof(data)));
	assert(savi_msg_type(&data, sizeof(data)) == 0);
	assert(savi_msg_type(&data, 1) == 0);

	/* Both gateway replies are broadcast, so each bracelet must pick out its own. */
	const uint8_t mine[SAVI_MAC_LEN] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
	const uint8_t other[SAVI_MAC_LEN] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

	savi_welcome_msg_t welcome = {.version = SAVI_PROTO_VERSION, .type = SAVI_MSG_WELCOME, .channel = 6};
	memcpy(welcome.bracelet_mac, mine, SAVI_MAC_LEN);
	assert(savi_msg_valid(&welcome, sizeof(welcome), SAVI_MSG_WELCOME, sizeof(welcome)));
	assert(!savi_msg_valid(&welcome, sizeof(welcome), SAVI_MSG_ASSIGN_ID, sizeof(welcome)));
	assert(memcmp(welcome.bracelet_mac, mine, SAVI_MAC_LEN) == 0);
	assert(memcmp(welcome.bracelet_mac, other, SAVI_MAC_LEN) != 0);

	savi_assign_id_msg_t assign = {.version = SAVI_PROTO_VERSION, .type = SAVI_MSG_ASSIGN_ID, .esp_id = 3};
	memcpy(assign.bracelet_mac, mine, SAVI_MAC_LEN);
	assert(savi_msg_valid(&assign, sizeof(assign), SAVI_MSG_ASSIGN_ID, sizeof(assign)));
	assert(assign.esp_id == 3);

	assert(strcmp(savi_event_name(SAVI_EVENT_TEMP_HIGH), "TEMP_HIGH") == 0);
	assert(strcmp(savi_event_name(SAVI_EVENT_MOTION), "MOTION") == 0);
	assert(strcmp(savi_event_name(SAVI_EVENT_BATTERY_LOW), "BATTERY_LOW") == 0);
	assert(strcmp(savi_event_name(99), "UNKNOWN") == 0);
	assert(savi_event_is_known(SAVI_EVENT_BATTERY_LOW));
	assert(!savi_event_is_known(99));

	assert(savi_battery_pct_valid(0));
	assert(savi_battery_pct_valid(100));
	assert(!savi_battery_pct_valid(101));
	assert(!savi_battery_pct_valid(SAVI_BATTERY_PCT_UNKNOWN));

	printf("sensor_data=%u event=%u hello=%u welcome=%u need_id=%u assign_id=%u\n", (unsigned)sizeof(savi_sensor_data_msg_t), (unsigned)sizeof(savi_sensor_event_msg_t), (unsigned)sizeof(savi_hello_msg_t), (unsigned)sizeof(savi_welcome_msg_t), (unsigned)sizeof(savi_need_id_msg_t), (unsigned)sizeof(savi_assign_id_msg_t));
	return 0;
}
