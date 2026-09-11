#ifndef I2C_IO_H
#define I2C_IO_H

#include "driver/i2c_master.h"
#include "esp_err.h"

#define I2C_IO_TIMEOUT_MS 50

esp_err_t i2c_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val);
esp_err_t i2c_write_regs(i2c_master_dev_handle_t dev, uint8_t reg, const uint8_t *data, size_t len);
esp_err_t i2c_read_regs(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *data, size_t len);

#endif
