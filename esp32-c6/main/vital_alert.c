#include "vital_alert.h"
#include "esp_timer.h"

void vital_alert_reset(vital_alert_t *alert)
{
	alert->streak = 0;
}

bool vital_alert_update(vital_alert_t *alert, bool out_of_range, int confirmations, uint32_t cooldown_ms)
{
	if (!out_of_range) {
		alert->streak = 0;
		return false;
	}

	alert->streak++;
	int64_t now_us = esp_timer_get_time();
	bool past_cooldown = !alert->fired_before || (now_us - alert->last_fire_us) > (int64_t)cooldown_ms * 1000;

	if (alert->streak >= confirmations && past_cooldown) {
		alert->fired_before = true;
		alert->last_fire_us = now_us;
		return true;
	}
	return false;
}
