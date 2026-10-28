#include "savi_battery.h"
#include "esp_log.h"

static const char *TAG = "savi_battery";

#if CONFIG_SAVI_BATTERY_MONITOR

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"

/* 12 dB keeps a 1S cell behind a 1:2 divider inside the linear window. */
#define BATTERY_ATTEN          ADC_ATTEN_DB_12
#define BATTERY_SAMPLES        8
/* Only used when eFuse calibration is missing: 12-bit full scale at 12 dB. */
#define BATTERY_RAW_FULL_SCALE 4095
#define BATTERY_UNCAL_FULL_MV  3100
#define BATTERY_DEFAULT_VREF   1100

static adc_oneshot_unit_handle_t s_unit;
static adc_cali_handle_t s_cali;
static adc_channel_t s_channel;
static bool s_ready;

static esp_err_t calibration_init(adc_unit_t unit)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
	adc_cali_curve_fitting_config_t cfg = {
		.unit_id = unit,
		.chan = s_channel,
		.atten = BATTERY_ATTEN,
		.bitwidth = ADC_BITWIDTH_DEFAULT,
	};
	return adc_cali_create_scheme_curve_fitting(&cfg, &s_cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
	adc_cali_line_fitting_config_t cfg = {
		.unit_id = unit,
		.atten = BATTERY_ATTEN,
		.bitwidth = ADC_BITWIDTH_DEFAULT,
#if CONFIG_IDF_TARGET_ESP32
		.default_vref = BATTERY_DEFAULT_VREF,
#endif
	};
	return adc_cali_create_scheme_line_fitting(&cfg, &s_cali);
#else
	(void)unit;
	return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t savi_battery_init(void)
{
	adc_unit_t unit;
	esp_err_t err = adc_oneshot_io_to_channel(CONFIG_SAVI_BATTERY_ADC_GPIO, &unit, &s_channel);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "GPIO %d has no ADC channel: %s", CONFIG_SAVI_BATTERY_ADC_GPIO, esp_err_to_name(err));
		return err;
	}

	adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = unit, .ulp_mode = ADC_ULP_MODE_DISABLE};
	err = adc_oneshot_new_unit(&unit_cfg, &s_unit);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Cannot open ADC unit %d: %s", (int)unit, esp_err_to_name(err));
		return err;
	}

	adc_oneshot_chan_cfg_t chan_cfg = {.atten = BATTERY_ATTEN, .bitwidth = ADC_BITWIDTH_DEFAULT};
	err = adc_oneshot_config_channel(s_unit, s_channel, &chan_cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Cannot configure GPIO %d for ADC: %s", CONFIG_SAVI_BATTERY_ADC_GPIO, esp_err_to_name(err));
		adc_oneshot_del_unit(s_unit);
		s_unit = NULL;
		return err;
	}

	if (calibration_init(unit) != ESP_OK) {
		s_cali = NULL;
		ESP_LOGW(TAG, "No eFuse ADC calibration, voltages are estimated from a %d mV full scale", BATTERY_UNCAL_FULL_MV);
	}

	s_ready = true;
	ESP_LOGI(TAG, "Gauge on GPIO %d, divider %d k/%d k, %d mV = 0%%, %d mV = 100%%", CONFIG_SAVI_BATTERY_ADC_GPIO, CONFIG_SAVI_BATTERY_R_HIGH_KOHM, CONFIG_SAVI_BATTERY_R_LOW_KOHM, CONFIG_SAVI_BATTERY_EMPTY_MV, CONFIG_SAVI_BATTERY_FULL_MV);
	return ESP_OK;
}

static bool pin_millivolts(int *out_mv)
{
	int total = 0;
	int taken = 0;

	for (int i = 0; i < BATTERY_SAMPLES; i++) {
		int mv = 0;
		esp_err_t err;
		if (s_cali != NULL) {
			err = adc_oneshot_get_calibrated_result(s_unit, s_cali, s_channel, &mv);
		} else {
			int raw = 0;
			err = adc_oneshot_read(s_unit, s_channel, &raw);
			mv = raw * BATTERY_UNCAL_FULL_MV / BATTERY_RAW_FULL_SCALE;
		}
		if (err == ESP_OK) {
			total += mv;
			taken++;
		}
	}

	if (taken == 0) {
		return false;
	}
	*out_mv = total / taken;
	return true;
}

/* Linear between the configured end points. Refine here with a real discharge
 * curve once the cell is characterised: a 1S li-ion sits near 3.7 V for most of
 * its capacity, so a straight line reads optimistic in the middle. */
static uint8_t percent_from_mv(int cell_mv)
{
	const int empty_mv = CONFIG_SAVI_BATTERY_EMPTY_MV;
	const int span_mv = CONFIG_SAVI_BATTERY_FULL_MV - empty_mv;

	if (cell_mv <= empty_mv) {
		return 0;
	}
	if (cell_mv - empty_mv >= span_mv) {
		return 100;
	}
	return (uint8_t)((cell_mv - empty_mv) * 100 / span_mv);
}

bool savi_battery_read(uint16_t *cell_mv, uint8_t *percent)
{
	if (!s_ready || cell_mv == NULL || percent == NULL) {
		return false;
	}

	int pin_mv = 0;
	if (!pin_millivolts(&pin_mv)) {
		ESP_LOGW(TAG, "Every ADC conversion on GPIO %d failed", CONFIG_SAVI_BATTERY_ADC_GPIO);
		return false;
	}

	/* VBAT = V_pin * (R_high + R_low) / R_low */
	int battery_mv = pin_mv * (CONFIG_SAVI_BATTERY_R_HIGH_KOHM + CONFIG_SAVI_BATTERY_R_LOW_KOHM) / CONFIG_SAVI_BATTERY_R_LOW_KOHM;
	*cell_mv = (uint16_t)(battery_mv > UINT16_MAX ? UINT16_MAX : battery_mv);
	*percent = percent_from_mv(battery_mv);
	ESP_LOGD(TAG, "pin=%d mV cell=%u mV %u%%", pin_mv, *cell_mv, *percent);
	return true;
}

#else /* !CONFIG_SAVI_BATTERY_MONITOR */

esp_err_t savi_battery_init(void)
{
	ESP_LOGI(TAG, "Gauge disabled, enable SAVI_BATTERY_MONITOR once the divider is wired");
	return ESP_ERR_NOT_SUPPORTED;
}

bool savi_battery_read(uint16_t *cell_mv, uint8_t *percent)
{
	(void)cell_mv;
	(void)percent;
	return false;
}

#endif /* CONFIG_SAVI_BATTERY_MONITOR */

uint8_t savi_battery_percent(void)
{
	uint16_t cell_mv = 0;
	uint8_t percent = 0;
	return savi_battery_read(&cell_mv, &percent) ? percent : SAVI_BATTERY_PERCENT_UNKNOWN;
}
