#include "max30102.h"
#include "i2c_io.h"
#include "max30102_algorithm.h"
#include "vital_alert.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define REG_FIFO_WR_PTR   0x04
#define REG_FIFO_RD_PTR   0x06
#define REG_FIFO_DATA     0x07
#define REG_FIFO_CONFIG   0x08
#define REG_MODE_CONFIG   0x09
#define REG_SPO2_CONFIG   0x0A
#define REG_LED1_PA       0x0C
#define REG_LED2_PA       0x0D
#define REG_PART_ID       0xFF

#define PART_ID_MAX30102  0x15

/* FIFO holds at most 32 unread samples (6 bytes each: 3 red + 3 ir). */
#define FIFO_MAX_SAMPLES  32

/* Below this raw IR reading, the finger is assumed to be off the sensor. Configurable under Sensor hardware calibration. */
#define FINGER_PRESENT_THRESHOLD ((uint32_t)CONFIG_SAVI_MAX30102_FINGER_THRESHOLD)

/* Configurable under sensor hardware calibration. */
#define BPM_VALID_MIN  CONFIG_SAVI_MAX30102_BPM_VALID_MIN
#define BPM_VALID_MAX  CONFIG_SAVI_MAX30102_BPM_VALID_MAX
#define SPO2_VALID_MIN CONFIG_SAVI_MAX30102_SPO2_VALID_MIN

static uint32_t s_ir_buf[MAX30102_ALG_BUFFER_SIZE];
static uint32_t s_red_buf[MAX30102_ALG_BUFFER_SIZE];
static int s_filled;  /* samples held so far, ramps up to the buffer size once at startup */
static int s_pending; /* new samples since the last recompute, once the buffer is full */

static uint8_t s_hr_bpm;
static uint8_t s_spo2_pct;
static bool s_fire_bpm_low;
static bool s_fire_bpm_high;
static bool s_fire_spo2_low;

static vital_alert_t s_alert_bpm_low;
static vital_alert_t s_alert_bpm_high;
static vital_alert_t s_alert_spo2_low;

static uint32_t decode_18(const uint8_t *b)
{
	return ((uint32_t)(b[0] & 0x03) << 16) | ((uint32_t)b[1] << 8) | b[2];
}

static void recompute(void)
{
	int32_t spo2 = -999;
	int32_t heart_rate = -999;
	int8_t spo2_valid = 0;
	int8_t hr_valid = 0;

	maxim_heart_rate_and_oxygen_saturation(s_ir_buf, MAX30102_ALG_BUFFER_SIZE, s_red_buf, &spo2, &spo2_valid, &heart_rate, &hr_valid);

	bool finger_present = s_ir_buf[MAX30102_ALG_BUFFER_SIZE - 1] > FINGER_PRESENT_THRESHOLD;
	bool hr_ok = finger_present && hr_valid == 1 && heart_rate >= BPM_VALID_MIN && heart_rate <= BPM_VALID_MAX;
	bool spo2_ok = finger_present && spo2_valid == 1 && spo2 >= SPO2_VALID_MIN && spo2 <= 100;

	s_hr_bpm = hr_ok ? (uint8_t)heart_rate : 0;
	s_spo2_pct = spo2_ok ? (uint8_t)spo2 : 0;

	if (!finger_present) {
		vital_alert_reset(&s_alert_bpm_low);
		vital_alert_reset(&s_alert_bpm_high);
		vital_alert_reset(&s_alert_spo2_low);
		s_fire_bpm_low = false;
		s_fire_bpm_high = false;
		s_fire_spo2_low = false;
		return;
	}

	/* When a reading is out of its sane range, the reference firmware simply skips evaluating
	 * that alert for this cycle (it does not reset the streak either). */
	if (hr_ok) {
		s_fire_bpm_low = vital_alert_update(&s_alert_bpm_low, heart_rate < CONFIG_SAVI_BPM_LOW_ALERT, CONFIG_SAVI_VITALS_ALERT_CONFIRMATIONS,
											 CONFIG_SAVI_VITALS_ALERT_COOLDOWN_MS);
		s_fire_bpm_high = vital_alert_update(&s_alert_bpm_high, heart_rate > CONFIG_SAVI_BPM_HIGH_ALERT, CONFIG_SAVI_VITALS_ALERT_CONFIRMATIONS,
											  CONFIG_SAVI_VITALS_ALERT_COOLDOWN_MS);
	} else {
		s_fire_bpm_low = false;
		s_fire_bpm_high = false;
	}

	if (spo2_ok) {
		s_fire_spo2_low = vital_alert_update(&s_alert_spo2_low, spo2 < CONFIG_SAVI_SPO2_LOW_ALERT, CONFIG_SAVI_VITALS_ALERT_CONFIRMATIONS,
											  CONFIG_SAVI_VITALS_ALERT_COOLDOWN_MS);
	} else {
		s_fire_spo2_low = false;
	}
}

/* Keeps a sliding window of the last MAX30102_ALG_BUFFER_SIZE samples and recomputes once every
 * 25 new samples (~1 s at the effective 25 Hz output rate), matching the reference firmware's
 * "shift by 25, read 25 new, recalculate" cadence. */
static void push_sample(uint32_t red, uint32_t ir)
{
	if (s_filled < MAX30102_ALG_BUFFER_SIZE) {
		s_red_buf[s_filled] = red;
		s_ir_buf[s_filled] = ir;
		s_filled++;
		if (s_filled == MAX30102_ALG_BUFFER_SIZE) {
			recompute();
		}
		return;
	}

	memmove(&s_red_buf[0], &s_red_buf[1], (MAX30102_ALG_BUFFER_SIZE - 1) * sizeof(uint32_t));
	memmove(&s_ir_buf[0], &s_ir_buf[1], (MAX30102_ALG_BUFFER_SIZE - 1) * sizeof(uint32_t));
	s_red_buf[MAX30102_ALG_BUFFER_SIZE - 1] = red;
	s_ir_buf[MAX30102_ALG_BUFFER_SIZE - 1] = ir;

	if (++s_pending >= 25) {
		s_pending = 0;
		recompute();
	}
}

/* using the exact same threshold ladder as SparkFun's MAX30105::setup()/setADCRange(). */
static uint8_t adc_range_bits(void)
{
	int adc_range_na = CONFIG_SAVI_MAX30102_ADC_RANGE_NA;
	if (adc_range_na < 4096) {
		return 0x00; /* 2048 nA */
	}
	if (adc_range_na < 8192) {
		return 0x20; /* 4096 nA */
	}
	if (adc_range_na < 16384) {
		return 0x40; /* 8192 nA */
	}
	return 0x60; /* 16384 nA */
}

/* using the exact same threshold ladder as SparkFun's MAX30105::setup()/setPulseWidth(). */
static uint8_t pulse_width_bits(void)
{
	int pulse_width_us = CONFIG_SAVI_MAX30102_PULSE_WIDTH_US;
	if (pulse_width_us < 118) {
		return 0x00; /* 69 us, 15-bit */
	}
	if (pulse_width_us < 215) {
		return 0x01; /* 118 us, 16-bit */
	}
	if (pulse_width_us < 411) {
		return 0x02; /* 215 us, 17-bit */
	}
	return 0x03; /* 411 us, 18-bit */
}

esp_err_t max30102_init(i2c_master_dev_handle_t dev)
{
	uint8_t id = 0;
	if (i2c_read_regs(dev, REG_PART_ID, &id, 1) != ESP_OK || id != PART_ID_MAX30102) {
		return ESP_ERR_NOT_FOUND;
	}

	i2c_write_reg(dev, REG_MODE_CONFIG, 0x40); /* reset */
	vTaskDelay(pdMS_TO_TICKS(10));

	i2c_write_reg(dev, REG_FIFO_WR_PTR, 0x00);
	i2c_write_reg(dev, REG_FIFO_RD_PTR, 0x00);
	i2c_write_reg(dev, REG_FIFO_CONFIG, 0x50); /* avg 4, rollover enabled, almost full = 0 (unused, no interrupt) */

	/* Sample rate is fixed at 100 Hz: combined with the fixed 4x on-chip averaging above, that
	 * gives the 25 Hz effective FIFO rate the HR/SpO2 algorithm assumes (see
	 * max30102_algorithm.h). Changing either without also changing MAX30102_ALG_SAMPLE_RATE_HZ
	 * would silently skew every HR reading, so only the ADC range and pulse width (which the
	 * algorithm does not depend on) are exposed via Kconfig. */
	uint8_t particle_config = adc_range_bits() | 0x04 /* 100 Hz */ | pulse_width_bits();
	i2c_write_reg(dev, REG_SPO2_CONFIG, particle_config);

	i2c_write_reg(dev, REG_LED1_PA, (uint8_t)CONFIG_SAVI_MAX30102_LED_CURRENT);
	i2c_write_reg(dev, REG_LED2_PA, (uint8_t)CONFIG_SAVI_MAX30102_LED_CURRENT);
	return i2c_write_reg(dev, REG_MODE_CONFIG, 0x03); /* SpO2 (red + IR) */
}

void max30102_poll(i2c_master_dev_handle_t dev)
{
	uint8_t wr = 0;
	uint8_t rd = 0;
	if (i2c_read_regs(dev, REG_FIFO_WR_PTR, &wr, 1) != ESP_OK ||
		i2c_read_regs(dev, REG_FIFO_RD_PTR, &rd, 1) != ESP_OK) {
		return;
	}

	int n = (wr - rd) & 0x1F;
	if (n == 0) {
		return;
	}
	if (n > FIFO_MAX_SAMPLES) {
		n = FIFO_MAX_SAMPLES;
	}

	uint8_t buf[FIFO_MAX_SAMPLES * 6];
	if (i2c_read_regs(dev, REG_FIFO_DATA, buf, (size_t)n * 6) != ESP_OK) {
		return;
	}

	for (int i = 0; i < n; i++) {
		uint32_t red = decode_18(&buf[i * 6]);
		uint32_t ir = decode_18(&buf[i * 6 + 3]);
		push_sample(red, ir);
	}
}

void max30102_get(uint8_t *hr_bpm, uint8_t *spo2_pct, bool *alert_bpm_low, bool *alert_bpm_high, bool *alert_spo2_low)
{
	*hr_bpm = s_hr_bpm;
	*spo2_pct = s_spo2_pct;
	*alert_bpm_low = s_fire_bpm_low;
	*alert_bpm_high = s_fire_bpm_high;
	*alert_spo2_low = s_fire_spo2_low;
	/* One-shot: consume the fire flags so sensors.c's ~50 ms poll loop reports each alert once. */
	s_fire_bpm_low = false;
	s_fire_bpm_high = false;
	s_fire_spo2_low = false;
}
