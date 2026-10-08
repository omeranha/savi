#ifndef SENSORS_H
#define SENSORS_H

#include "driver/gpio.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Edit these to match your board. All three sensors share this I2C bus. */
#define PIN_I2C_SDA        GPIO_NUM_6
#define PIN_I2C_SCL        GPIO_NUM_7
#define I2C_FREQ_HZ        400000

#define ADDR_MAX30102      0x57
#define ADDR_MAX30205      0x48  /* A0=A1=A2=GND */
#define ADDR_BMI270        0x68  /* SDO to GND; use 0x69 if SDO is high */

#define PIN_LED            GPIO_NUM_4
#define PIN_BUZZER         GPIO_NUM_5

#define TEMP_HIGH_THRESHOLD_C  38.0f
#define MOTION_DELTA_G         0.4f

typedef struct {
	float temperature_c;
	uint8_t hr_bpm;
	uint8_t spo2_pct;
	float ax;
	float ay;
	float az;
	float gx;
	float gy;
	float gz;
	bool motion;
} sensor_readings_t;

esp_err_t sensors_init(void);
sensor_readings_t sensors_read(void);

#endif
