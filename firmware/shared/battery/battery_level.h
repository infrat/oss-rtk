#ifndef SHARED_BATTERY_LEVEL_H
#define SHARED_BATTERY_LEVEL_H

#include <stdint.h>

/**
 * @brief Battery state shared between the measuring module and the OLED UI.
 *
 * Header-only, used by both the transmitter (AXP192/AXP2101 PMU) and the
 * receiver (VBAT divider on GPIO35).
 */
struct BatteryStatus
{
    bool valid = false;      // at least one sample taken
    bool present = true;     // false only when the PMU reports no cell
    bool charging = false;   // only the T-Beam PMU can detect this
    uint16_t voltage_mv = 0; // filtered cell voltage
    uint8_t percent = 0;     // 0..100, from batteryPercentFromMv()
};

/**
 * @brief Convert a single-cell Li-Ion/LiPo voltage to an approximate charge level.
 *
 * Piecewise-linear open-circuit discharge curve (3.30 V = 0 %, 4.20 V = 100 %).
 * The curve is flat around 3.7-3.8 V, so a linear 3.3-4.2 V mapping would
 * report ~50 % for a cell that is really at ~30 %.
 *
 * @param mv Cell voltage in millivolts
 * @return Charge level 0..100
 */
inline uint8_t batteryPercentFromMv(uint16_t mv)
{
    static const uint16_t CURVE_MV[] = {3300, 3550, 3650, 3700, 3750, 3800, 3850, 3920, 4000, 4100, 4200};
    static const uint8_t CURVE_PCT[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    const int points = sizeof(CURVE_MV) / sizeof(CURVE_MV[0]);

    if (mv <= CURVE_MV[0])
        return 0;
    if (mv >= CURVE_MV[points - 1])
        return 100;

    for (int i = 1; i < points; i++)
    {
        if (mv <= CURVE_MV[i])
        {
            uint32_t span_mv = CURVE_MV[i] - CURVE_MV[i - 1];
            uint32_t span_pct = CURVE_PCT[i] - CURVE_PCT[i - 1];
            return CURVE_PCT[i - 1] + (uint8_t)(((uint32_t)(mv - CURVE_MV[i - 1]) * span_pct) / span_mv);
        }
    }
    return 100;
}

/**
 * @brief Exponential moving average for battery voltage (alpha = 1/8).
 *
 * Smooths ADC noise and load-induced dips (LoRa TX bursts, WiFi) so the
 * icon does not flicker between segments.
 */
inline uint16_t batteryFilterMv(uint16_t previous_mv, uint16_t sample_mv, bool first_sample)
{
    if (first_sample)
        return sample_mv;
    return (uint16_t)(((uint32_t)previous_mv * 7 + sample_mv) / 8);
}

#endif // SHARED_BATTERY_LEVEL_H
