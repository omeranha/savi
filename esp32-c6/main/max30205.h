#ifndef MAX30205_H
#define MAX30205_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdbool.h>

/* Configurable under Sensor hardware calibration. */
#define MAX30205_SANE_MIN_C (CONFIG_SAVI_MAX30205_SANE_MIN_C_X10 / 10.0f)
#define MAX30205_SANE_MAX_C (CONFIG_SAVI_MAX30205_SANE_MAX_C_X10 / 10.0f)

esp_err_t max30205_init(i2c_master_dev_handle_t dev);
esp_err_t max30205_start_oneshot(i2c_master_dev_handle_t dev);
esp_err_t max30205_shutdown(i2c_master_dev_handle_t dev);
bool max30205_read_c(i2c_master_dev_handle_t dev, float *temp_c);

/* Feeds one new temperature reading into the low/high alert gates. valid should be false when
 * the reading (or the sensor itself) is unavailable or outside MAX30205_SANE_*_C; the gates are
 * then reset, matching the reference firmware's skin-contact-gated processarAlertaTemperatura(). */
void max30205_evaluate(float temp_c, bool valid);

/* One-shot: true exactly once, on the call right after max30205_evaluate() decides to fire. */
void max30205_get_alerts(bool *alert_temp_low, bool *alert_temp_high);

#endif
