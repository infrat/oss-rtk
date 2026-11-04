#ifndef BATTERY_MONITOR_H
#define BATTERY_MONITOR_H

#include <Arduino.h>
#include "battery/battery_level.h"

class XPowersLibInterface;

/**
 * @brief Battery voltage / charge-state monitor for the TTGO T-Beam.
 *
 * Auto-detects the PMU on the OLED I2C bus: AXP2101 (T-Beam v1.2) or
 * AXP192 (v1.0/v1.1). Without a PMU it falls back to the VBAT/2 divider on
 * BATTERY_ADC_PIN (v0.7), where presence and charging are not detectable.
 *
 * Only reads the PMU, never changes its power rails.
 * Not thread-safe: call begin()/update() from the same task that drives the
 * OLED (Arduino loop, Core 1), because both share the Wire bus.
 */
class BatteryMonitor
{
public:
    BatteryMonitor();

    /**
     * @brief Detect the PMU and take the first sample.
     * Call after DisplayManager::begin() so the I2C bus is already set up.
     */
    void begin();

    /**
     * @brief Take a new sample if BATTERY_SAMPLE_INTERVAL_MS has elapsed.
     * Cheap to call on every loop() iteration.
     */
    void update();

    const BatteryStatus &getStatus() const { return status; }

    /** @return "AXP2101", "AXP192" or "ADC" */
    const char *getSourceName() const;

private:
    void sample();
    uint16_t readAdcMv();

    XPowersLibInterface *pmu;
    bool pmuIsAxp2101;
    BatteryStatus status;
    unsigned long lastSampleMs;
};

#endif // BATTERY_MONITOR_H
