#ifndef BATTERY_H
#define BATTERY_H

#include <Arduino.h>
#include "battery/battery_level.h"

/**
 * @brief Configure the VBAT ADC pin and take the first sample.
 * Call from setup() before the tasks start.
 */
void setupBattery();

/**
 * @brief Take a new sample if BATTERY_SAMPLE_INTERVAL_MS has elapsed.
 * Owned by networkTask (Core 0), the same task that renders the OLED,
 * so the status needs no inter-core locking.
 */
void updateBattery();

/** @brief Latest filtered battery state (charging is never detected here). */
const BatteryStatus &getBatteryStatus();

#endif // BATTERY_H
