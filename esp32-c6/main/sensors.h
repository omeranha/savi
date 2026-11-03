#ifndef SENSORS_H
#define SENSORS_H

#include "driver/gpio.h"
#include "esp_err.h"
#include "savi_battery.h"
#include <stdbool.h>
#include <stdint.h>

/* Edit these to match your board. All three sensors share this I2C bus. */
#define PIN_I2C_SDA        GPIO_NUM_6
#define PIN_I2C_SCL        GPIO_NUM_7
#define I2C_FREQ_HZ        400000

/* I2C addresses, LED/ADC calibration and finger/temperature sanity ranges are configurable under
 * Component config -> SAVI Bracelet Configuration -> Sensor hardware calibration (CONFIG_SAVI_*),
 * used directly by sensors.c/max30102.c/max30205.c/bmi270_imu.c. */

#define PIN_LED            GPIO_NUM_4
#define PIN_BUZZER         GPIO_NUM_5

/* Fever threshold handling moved to the Kconfig-backed vital-sign alert thresholds
 * (SAVI_TEMP_HIGH_ALERT_C_X10 etc.); see main.c's sensor_task. */
#define MOTION_DELTA_G         0.4f

typedef struct {
	float temperature_c;
	uint8_t hr_bpm;
	uint8_t spo2_pct;
	uint8_t battery_pct;   /* SAVI_BATTERY_PERCENT_UNKNOWN when there is no gauge */
	uint16_t battery_mv;   /* 0 when there is no gauge, for calibration logs */
	float ax;
	float ay;
	float az;
	float gx;
	float gy;
	float gz;
	bool motion;
	bool fall;             /* one-shot: true exactly once when a fall is detected */
	float fall_peak_g;     /* acceleration magnitude that triggered the fall, valid when fall is true */
	bool alert_bpm_low;    /* one-shot vital-sign alerts, gated by confirmations + cooldown */
	bool alert_bpm_high;
	bool alert_spo2_low;
	bool alert_temp_low;
	bool alert_temp_high;
} sensor_readings_t;

esp_err_t sensors_init(void);
sensor_readings_t sensors_read(void);

#endif
