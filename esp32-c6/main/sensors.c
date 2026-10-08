#include "sensors.h"
#include "bmi270_imu.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "max30102.h"
#include "max30205.h"
#include <math.h>

static const char *TAG = "sensors";

/* One-shot conversion ~every 250 ms (5 x 50 ms polls). Fever is slow. */
#define TEMP_READ_PERIOD_POLLS 5

static SemaphoreHandle_t s_lock;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_max30102;
static i2c_master_dev_handle_t s_max30205;
static i2c_master_dev_handle_t s_bmi270;
static bool s_have_max30102;
static bool s_have_max30205;
static bool s_have_bmi270;

static i2c_master_dev_handle_t add_dev(uint8_t addr)
{
	i2c_device_config_t cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = addr,
		.scl_speed_hz = I2C_FREQ_HZ,
	};
	i2c_master_dev_handle_t dev = NULL;
	ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &cfg, &dev));
	return dev;
}

esp_err_t sensors_init(void)
{
	s_lock = xSemaphoreCreateMutex();
	if (s_lock == NULL) {
		return ESP_ERR_NO_MEM;
	}

	i2c_master_bus_config_t bus_cfg = {
		.i2c_port = I2C_NUM_0,
		.sda_io_num = PIN_I2C_SDA,
		.scl_io_num = PIN_I2C_SCL,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

	s_max30102 = add_dev(ADDR_MAX30102);
	s_max30205 = add_dev(ADDR_MAX30205);
	s_bmi270 = add_dev(ADDR_BMI270);

	if (max30102_init(s_max30102) == ESP_OK) {
		s_have_max30102 = true;
		ESP_LOGI(TAG, "MAX30102 ready");
	} else {
		ESP_LOGW(TAG, "MAX30102 not found at 0x%02X", ADDR_MAX30102);
	}

	if (max30205_init(s_max30205) == ESP_OK) {
		s_have_max30205 = true;
		ESP_LOGI(TAG, "MAX30205 ready");
	} else {
		ESP_LOGW(TAG, "MAX30205 not found at 0x%02X", ADDR_MAX30205);
	}

	if (bmi270_imu_init(s_bmi270) == ESP_OK) {
		s_have_bmi270 = true;
		ESP_LOGI(TAG, "BMI270 ready");
	} else {
		ESP_LOGW(TAG, "BMI270 not found at 0x%02X", ADDR_BMI270);
	}

	return ESP_OK;
}

sensor_readings_t sensors_read(void)
{
	sensor_readings_t r = {0};
	static float last_temp_c;
	static int poll_n;
	static bool temp_pending;

	xSemaphoreTake(s_lock, portMAX_DELAY);

	if (s_have_max30205) {
		if (temp_pending) {
			if (max30205_read_c(s_max30205, &last_temp_c)) {
				max30205_shutdown(s_max30205);
			}
			temp_pending = false;
		} else if ((poll_n % TEMP_READ_PERIOD_POLLS) == 0) {
			if (max30205_start_oneshot(s_max30205) == ESP_OK) {
				temp_pending = true;
			}
		}
		r.temperature_c = last_temp_c;
		poll_n++;
	}
	if (s_have_max30102) {
		max30102_poll(s_max30102);
		max30102_get(&r.hr_bpm, &r.spo2_pct);
	}
	if (s_have_bmi270 &&
		bmi270_imu_read(s_bmi270, &r.ax, &r.ay, &r.az, &r.gx, &r.gy, &r.gz)) {
		float mag = sqrtf(r.ax * r.ax + r.ay * r.ay + r.az * r.az);
		r.motion = fabsf(mag - 1.0f) > MOTION_DELTA_G;
	}

	xSemaphoreGive(s_lock);
	return r;
}
