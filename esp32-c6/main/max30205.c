#include "max30205.h"
#include "i2c_io.h"
#include "vital_alert.h"

#define MAX30205_REG_TEMP  0x00
#define MAX30205_REG_CFG   0x01

#define CFG_SHUTDOWN  0x01
#define CFG_ONESHOT   0x81

/* Derived from the Kconfig thresholds (tenths of a degree Celsius) so the rest of this file can
 * work in plain floats */
#define TEMP_LOW_ALERT_C         (CONFIG_SAVI_TEMP_LOW_ALERT_C_X10 / 10.0f)
#define TEMP_HIGH_ALERT_C        (CONFIG_SAVI_TEMP_HIGH_ALERT_C_X10 / 10.0f)
#define TEMP_SKIN_CONTACT_MIN_C  (CONFIG_SAVI_TEMP_SKIN_CONTACT_MIN_C_X10 / 10.0f)

static bool s_fire_temp_low;
static bool s_fire_temp_high;

static vital_alert_t s_alert_temp_low;
static vital_alert_t s_alert_temp_high;

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

void max30205_evaluate(float temp_c, bool valid)
{
	/* Below the skin-contact threshold the sensor is assumed to be off the skin (ambient air
	 * reads much cooler), so alerts are suppressed rather than false-triggering "too cold". */
	bool skin_contact = valid && temp_c >= TEMP_SKIN_CONTACT_MIN_C;
	if (!skin_contact) {
		vital_alert_reset(&s_alert_temp_low);
		vital_alert_reset(&s_alert_temp_high);
		s_fire_temp_low = false;
		s_fire_temp_high = false;
		return;
	}

	s_fire_temp_low = vital_alert_update(&s_alert_temp_low, temp_c < TEMP_LOW_ALERT_C, CONFIG_SAVI_VITALS_ALERT_CONFIRMATIONS,
										  CONFIG_SAVI_VITALS_ALERT_COOLDOWN_MS);
	s_fire_temp_high = vital_alert_update(&s_alert_temp_high, temp_c > TEMP_HIGH_ALERT_C, CONFIG_SAVI_VITALS_ALERT_CONFIRMATIONS,
										   CONFIG_SAVI_VITALS_ALERT_COOLDOWN_MS);
}

void max30205_get_alerts(bool *alert_temp_low, bool *alert_temp_high)
{
	*alert_temp_low = s_fire_temp_low;
	*alert_temp_high = s_fire_temp_high;
	s_fire_temp_low = false;
	s_fire_temp_high = false;
}
