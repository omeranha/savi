#ifndef BMI270_IMU_H
#define BMI270_IMU_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdbool.h>

esp_err_t bmi270_imu_init(i2c_master_dev_handle_t dev);
bool bmi270_imu_read(i2c_master_dev_handle_t dev, float *ax, float *ay, float *az, float *gx, float *gy, float *gz);

#endif
