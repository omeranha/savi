#ifndef SAVI_BATTERY_H
#define SAVI_BATTERY_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/*
 * Battery gauge for one li-ion cell sensed through a resistor divider, shared by
 * the bracelet and the gateway:
 *
 *   VBAT --[ R_high ]--+--[ R_low ]-- GND
 *                      |
 *                   ADC GPIO
 *
 * Pin, resistor values and the cell end points live in menuconfig under
 * Component config -> SAVI battery gauge. The divider has to keep the pin under
 * about 3.1 V at a full cell.
 */

#define SAVI_BATTERY_PERCENT_UNKNOWN 0xFF

esp_err_t savi_battery_init(void);

/* Averaged cell voltage and charge estimate. False when the gauge is disabled or
 * every conversion failed, leaving both outputs untouched. */
bool savi_battery_read(uint16_t *cell_mv, uint8_t *percent);

/* Percentage only, SAVI_BATTERY_PERCENT_UNKNOWN when unavailable. */
uint8_t savi_battery_percent(void);

#endif
