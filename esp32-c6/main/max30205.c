#include "max30205.h"
#include "i2c_io.h"

#define MAX30205_REG_TEMP  0x00
#define MAX30205_REG_CFG   0x01

#define CFG_SHUTDOWN  0x01
#define CFG_ONESHOT   0x81

esp_err_t max30205_init(i2c_master_dev_handle_t dev)
{
    return max30205_shutdown(dev);
}

esp_err_t max30205_start_oneshot(i2c_master_dev_handle_t dev)
{
    return i2c_write_reg(dev, MAX30205_REG_CFG, CFG_ONESHOT);
}

esp_err_t max30205_shutdown(i2c_master_dev_handle_t dev)
{
    return i2c_write_reg(dev, MAX30205_REG_CFG, CFG_SHUTDOWN);
}

bool max30205_read_c(i2c_master_dev_handle_t dev, float *temp_c)
{
    uint8_t raw[2];
    if (i2c_read_regs(dev, MAX30205_REG_TEMP, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }

    int16_t counts = (int16_t)((raw[0] << 8) | raw[1]);
    *temp_c = (float)counts / 256.0f;
    return true;
}
