#include "max30102.h"
#include "i2c_io.h"
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
#define SAMPLE_RATE_HZ    50
#define WINDOW_LEN        100

static uint32_t s_ir[WINDOW_LEN];
static uint32_t s_red[WINDOW_LEN];
static int s_count;
static int s_idx;
static uint8_t s_hr;
static uint8_t s_spo2;

static uint32_t decode_18(const uint8_t *b)
{
	return ((uint32_t)(b[0] & 0x03) << 16) | ((uint32_t)b[1] << 8) | b[2];
}

static void update_vitals(void)
{
	if (s_count < WINDOW_LEN) {
		return;
	}

	uint64_t ir_sum = 0;
	uint64_t red_sum = 0;
	uint32_t ir_min = UINT32_MAX;
	uint32_t ir_max = 0;
	uint32_t red_min = UINT32_MAX;
	uint32_t red_max = 0;

	for (int i = 0; i < WINDOW_LEN; i++) {
		ir_sum += s_ir[i];
		red_sum += s_red[i];
		if (s_ir[i] < ir_min) {
			ir_min = s_ir[i];
		}
		if (s_ir[i] > ir_max) {
			ir_max = s_ir[i];
		}
		if (s_red[i] < red_min) {
			red_min = s_red[i];
		}
		if (s_red[i] > red_max) {
			red_max = s_red[i];
		}
	}

	float ir_dc = (float)ir_sum / WINDOW_LEN;
	float red_dc = (float)red_sum / WINDOW_LEN;
	if (ir_dc < 20000.0f) {
		s_hr = 0;
		s_spo2 = 0;
		return;
	}

	float thresh = ir_dc + 0.3f * ((float)ir_max - ir_dc);
	int peaks = 0;
	int first = -1;
	int last = -1;
	for (int i = 1; i < WINDOW_LEN - 1; i++) {
		if (s_ir[i] > thresh && s_ir[i] >= s_ir[i - 1] && s_ir[i] >= s_ir[i + 1]) {
			if (first < 0) {
				first = i;
			}
			last = i;
			peaks++;
		}
	}

	if (peaks >= 2 && last > first) {
		float bpm = 60.0f * (float)(peaks - 1) * SAMPLE_RATE_HZ / (float)(last - first);
		if (bpm >= 40.0f && bpm <= 180.0f) {
			s_hr = (uint8_t)(bpm + 0.5f);
		}
	}

	float ir_ac = (float)(ir_max - ir_min);
	float red_ac = (float)(red_max - red_min);
	if (ir_ac > 1.0f && red_dc > 1.0f) {
		float r = (red_ac / red_dc) / (ir_ac / ir_dc);
		float spo2 = 110.0f - 25.0f * r;
		if (spo2 < 70.0f) {
			spo2 = 70.0f;
		}
		if (spo2 > 100.0f) {
			spo2 = 100.0f;
		}
		s_spo2 = (uint8_t)(spo2 + 0.5f);
	}
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
	i2c_write_reg(dev, REG_FIFO_CONFIG, 0x4F); /* avg 4, rollover, almost full */
	i2c_write_reg(dev, REG_SPO2_CONFIG, 0x23); /* 4096 nA, 50 Hz, 411 us */
	i2c_write_reg(dev, REG_LED1_PA, 0x24);
	i2c_write_reg(dev, REG_LED2_PA, 0x24);
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
	if (n > 8) {
		n = 8;
	}

	uint8_t buf[8 * 6];
	if (i2c_read_regs(dev, REG_FIFO_DATA, buf, (size_t)n * 6) != ESP_OK) {
		return;
	}

	for (int i = 0; i < n; i++) {
		s_red[s_idx] = decode_18(&buf[i * 6]);
		s_ir[s_idx] = decode_18(&buf[i * 6 + 3]);
		s_idx = (s_idx + 1) % WINDOW_LEN;
		if (s_count < WINDOW_LEN) {
			s_count++;
		}
	}

	update_vitals();
}

void max30102_get(uint8_t *hr_bpm, uint8_t *spo2_pct)
{
	*hr_bpm = s_hr;
	*spo2_pct = s_spo2;
}
