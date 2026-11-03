#include "bmi270_imu.h"
#include "i2c_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <math.h>

#define REG_CHIP_ID   0x00
#define REG_DATA_ACC  0x0C
#define REG_ACC_CONF  0x40
#define REG_ACC_RANGE 0x41
#define REG_GYR_CONF  0x42
#define REG_GYR_RANGE 0x43
#define REG_CMD       0x7E
#define REG_PWR_CONF  0x7C
#define REG_PWR_CTRL  0x7D

#define CHIP_ID_BMI270 0x24
#define CMD_SOFT_RESET 0xB6

/* ±4 g and ±2000 dps */
#define ACC_LSB_PER_G     8192.0f
#define GYR_LSB_PER_DPS   16.384f

esp_err_t bmi270_imu_init(i2c_master_dev_handle_t dev)
{
	uint8_t id = 0;
	if (i2c_read_regs(dev, REG_CHIP_ID, &id, 1) != ESP_OK || id != CHIP_ID_BMI270) {
		return ESP_ERR_NOT_FOUND;
	}

	i2c_write_reg(dev, REG_CMD, CMD_SOFT_RESET);
	vTaskDelay(pdMS_TO_TICKS(50));

	/* Disable advanced power save so config registers can be written. */
	i2c_write_reg(dev, REG_PWR_CONF, 0x00);
	vTaskDelay(pdMS_TO_TICKS(1));

	/* Accel: 50 Hz, normal bandwidth, ±4 g */
	i2c_write_reg(dev, REG_ACC_CONF, 0xA7);
	i2c_write_reg(dev, REG_ACC_RANGE, 0x01);
	/* Gyro: 50 Hz, normal, ±2000 dps */
	i2c_write_reg(dev, REG_GYR_CONF, 0xA7);
	i2c_write_reg(dev, REG_GYR_RANGE, 0x00);

	/* Enable accel + gyro + aux-temp, then advanced power save. */
	i2c_write_reg(dev, REG_PWR_CTRL, 0x0E);
	vTaskDelay(pdMS_TO_TICKS(1));
	return i2c_write_reg(dev, REG_PWR_CONF, 0x01);
}

bool bmi270_imu_read(i2c_master_dev_handle_t dev, float *ax, float *ay, float *az,
					 float *gx, float *gy, float *gz)
{
	uint8_t raw[12];
	if (i2c_read_regs(dev, REG_DATA_ACC, raw, sizeof(raw)) != ESP_OK) {
		return false;
	}

	int16_t acc_x = (int16_t)(raw[0] | (raw[1] << 8));
	int16_t acc_y = (int16_t)(raw[2] | (raw[3] << 8));
	int16_t acc_z = (int16_t)(raw[4] | (raw[5] << 8));
	int16_t gyr_x = (int16_t)(raw[6] | (raw[7] << 8));
	int16_t gyr_y = (int16_t)(raw[8] | (raw[9] << 8));
	int16_t gyr_z = (int16_t)(raw[10] | (raw[11] << 8));

	*ax = (float)acc_x / ACC_LSB_PER_G;
	*ay = (float)acc_y / ACC_LSB_PER_G;
	*az = (float)acc_z / ACC_LSB_PER_G;
	*gx = (float)gyr_x / GYR_LSB_PER_DPS;
	*gy = (float)gyr_y / GYR_LSB_PER_DPS;
	*gz = (float)gyr_z / GYR_LSB_PER_DPS;
	return true;
}

/* Derived from the Kconfig thresholds (hundredths of a g) so the rest of this file can work in
 * plain floats. A bool Kconfig option that is unset leaves its CONFIG_ macro undefined entirely,
 * so it has to be read with #if, never as a bare C expression. */
#define FALL_IMPACT_G           (CONFIG_SAVI_FALL_IMPACT_G_X100 / 100.0f)
#define FALL_FREEFALL_G         (CONFIG_SAVI_FALL_FREEFALL_G_X100 / 100.0f)
#if CONFIG_SAVI_FALL_REQUIRE_FREEFALL
#define FALL_REQUIRE_FREEFALL 1
#else
#define FALL_REQUIRE_FREEFALL 0
#endif

static bool s_saw_freefall;
static int64_t s_freefall_us;
static bool s_fired_before;
static int64_t s_last_fire_us;

bool bmi270_imu_check_fall(float ax, float ay, float az, float *peak_g)
{
	float mag = sqrtf(ax * ax + ay * ay + az * az);
	int64_t now_us = esp_timer_get_time();

	if (mag < FALL_FREEFALL_G) {
		s_saw_freefall = true;
		s_freefall_us = now_us;
	}

	if (mag <= FALL_IMPACT_G) {
		return false;
	}

	bool freefall_recent = s_saw_freefall && (now_us - s_freefall_us) <= (int64_t)CONFIG_SAVI_FALL_FREEFALL_WINDOW_MS * 1000;
	bool condition = !FALL_REQUIRE_FREEFALL || freefall_recent;
	bool past_cooldown = !s_fired_before || (now_us - s_last_fire_us) > (int64_t)CONFIG_SAVI_FALL_COOLDOWN_MS * 1000;

	if (condition && past_cooldown) {
		s_fired_before = true;
		s_last_fire_us = now_us;
		s_saw_freefall = false;
		*peak_g = mag;
		return true;
	}
	return false;
}
