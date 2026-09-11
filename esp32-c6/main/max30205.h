#ifndef MAX30205_H
#define MAX30205_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdbool.h>

esp_err_t max30205_init(i2c_master_dev_handle_t dev);
esp_err_t max30205_start_oneshot(i2c_master_dev_handle_t dev);
esp_err_t max30205_shutdown(i2c_master_dev_handle_t dev);
bool max30205_read_c(i2c_master_dev_handle_t dev, float *temp_c);

#endif
