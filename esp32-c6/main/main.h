#ifndef MAIN_H
#define MAIN_H

#include <stdint.h>

/* The ESP-NOW message layout lives in components/savi_proto/include/savi_proto.h
 * so the bracelet and the gateway can never disagree about the wire format. */

#if CONFIG_ESPNOW_WIFI_MODE_STATION
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF   WIFI_IF_STA
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF   WIFI_IF_AP
#endif

#endif
