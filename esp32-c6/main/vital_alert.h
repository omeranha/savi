#ifndef VITAL_ALERT_H
#define VITAL_ALERT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Confirmation-and-cooldown gate for a vital-sign alert, a condition has to hold for several
 * consecutive updates before it fires, and once it has fired it will not fire again until the cooldown
 * elapses, even if the condition keeps holding.
 */
typedef struct {
	int streak;
	int64_t last_fire_us;
	bool fired_before;
} vital_alert_t;

/* Clears the confirmation streak, e.g. when the underlying reading becomes
 * unavailable (no finger, sensor off skin). Does not touch the cooldown. */
void vital_alert_reset(vital_alert_t *alert);

/* Call once per new reading with whether the monitored value is currently out
 * of range. Returns true exactly on the update where the alert should fire. */
bool vital_alert_update(vital_alert_t *alert, bool out_of_range, int confirmations, uint32_t cooldown_ms);

#endif
