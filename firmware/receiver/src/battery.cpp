#include "battery.h"
#include "config.h"
#include "debug.h"

static BatteryStatus battery_status;
static unsigned long last_sample_ms = 0;

static void sampleBattery()
{
    last_sample_ms = millis();

    uint32_t sum = 0;
    for (int i = 0; i < BATTERY_ADC_SAMPLES; i++)
        sum += analogReadMilliVolts(BATTERY_ADC_PIN); // eFuse-calibrated, unlike raw analogRead()
    uint16_t mv = (uint16_t)((sum / BATTERY_ADC_SAMPLES) * BATTERY_SCALE_FACTOR);

    battery_status.voltage_mv = batteryFilterMv(battery_status.voltage_mv, mv, !battery_status.valid);
    battery_status.percent = batteryPercentFromMv(battery_status.voltage_mv);
    battery_status.valid = true;
}

void setupBattery()
{
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db); // VBAT/2 is up to ~2.1 V
    sampleBattery();
    DEBUG_PRINTF("Battery: %u mV (%u%%)\n", battery_status.voltage_mv, battery_status.percent);
}

void updateBattery()
{
    if (millis() - last_sample_ms >= BATTERY_SAMPLE_INTERVAL_MS)
        sampleBattery();
}

const BatteryStatus &getBatteryStatus()
{
    return battery_status;
}
