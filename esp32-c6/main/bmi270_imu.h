#ifndef BMI270_IMU_H
#define BMI270_IMU_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdbool.h>

esp_err_t bmi270_imu_init(i2c_master_dev_handle_t dev);
bool bmi270_imu_read(i2c_master_dev_handle_t dev, float *ax, float *ay, float *az, float *gx, float *gy, float *gz);

/* Free-fall-then-impact fall detector, a dip below the free-fall threshold followed, within the configured window,
 * by a spike above the impact threshold is treated as a fall, gated by a cooldown so the same
 * fall is not reported repeatedly. Call once per new accelerometer reading (ax/ay/az in g).
 * Returns true exactly on the reading where a fall should be reported, and sets *peak_g to the
 * acceleration magnitude that triggered it. */
bool bmi270_imu_check_fall(float ax, float ay, float az, float *peak_g);

#endif
