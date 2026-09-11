#ifndef MAX30102_H
#define MAX30102_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t max30102_init(i2c_master_dev_handle_t dev);
void max30102_poll(i2c_master_dev_handle_t dev);
void max30102_get(uint8_t *hr_bpm, uint8_t *spo2_pct);

#endif
