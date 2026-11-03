#ifndef MAX30102_H
#define MAX30102_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t max30102_init(i2c_master_dev_handle_t dev);
void max30102_poll(i2c_master_dev_handle_t dev);

/* hr_bpm/spo2_pct are 0 when there is no finger or no valid reading yet.
 * alert_* are one-shot: true exactly once, on the call right after the
 * confirmation-and-cooldown gate (see vital_alert.h) decides to fire. */
void max30102_get(uint8_t *hr_bpm, uint8_t *spo2_pct, bool *alert_bpm_low, bool *alert_bpm_high, bool *alert_spo2_low);

#endif
