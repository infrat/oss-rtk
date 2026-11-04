#include "battery_monitor.h"
#include <Wire.h>
#include <XPowersLib.h>
#include "config.h"

// Averaging over several conversions removes most of the ESP32 ADC noise
static const int ADC_SAMPLES = 16;

BatteryMonitor::BatteryMonitor() : pmu(nullptr), pmuIsAxp2101(false), lastSampleMs(0)
{
}

/**
 * @brief Read the PMU chip-type register directly over Wire.
 *
 * Probing through XPowersLib objects is not an option: destroying a driver
 * whose init() failed calls Wire.end() and would take the OLED down with it.
 * @return Register value, or -1 when nothing answers at PMU_I2C_ADDRESS
 */
static int readPmuChipId()
{
    Wire.beginTransmission(PMU_I2C_ADDRESS);
    Wire.write(XPOWERS_AXP2101_IC_TYPE); // same register (0x03) on AXP192
    if (Wire.endTransmission(false) != 0)
        return -1;
    if (Wire.requestFrom((uint8_t)PMU_I2C_ADDRESS, (uint8_t)1) != 1)
        return -1;
    return Wire.read();
}

void BatteryMonitor::begin()
{
    int chip_id = readPmuChipId();

    if (chip_id == XPOWERS_AXP2101_CHIP_ID)
    {
        XPowersAXP2101 *axp2101 = new XPowersAXP2101(Wire, PMU_I2C_SDA, PMU_I2C_SCL, PMU_I2C_ADDRESS);
        if (axp2101->init())
        {
            pmu = axp2101;
            pmuIsAxp2101 = true;
        }
    }
    else if (chip_id == XPOWERS_AXP192_CHIP_ID)
    {
        XPowersAXP192 *axp192 = new XPowersAXP192(Wire, PMU_I2C_SDA, PMU_I2C_SCL, PMU_I2C_ADDRESS);
        if (axp192->init())
            pmu = axp192;
    }
    // A driver whose init() failed is intentionally leaked (a few bytes, once):
    // its destructor would call Wire.end().

    if (pmu)
    {
        pmu->enableBattDetection();
        pmu->enableBattVoltageMeasure();
        Serial.printf("✓ Battery monitor: %s PMU\n", getSourceName());
    }
    else
    {
        analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
        Serial.println(F("✗ No PMU found, battery monitor falls back to ADC"));
    }

    sample();
}

void BatteryMonitor::update()
{
    if (millis() - lastSampleMs >= BATTERY_SAMPLE_INTERVAL_MS)
        sample();
}

const char *BatteryMonitor::getSourceName() const
{
    if (!pmu)
        return "ADC";
    return pmuIsAxp2101 ? "AXP2101" : "AXP192";
}

void BatteryMonitor::sample()
{
    lastSampleMs = millis();

    uint16_t mv;
    if (pmu)
    {
        status.present = pmu->isBatteryConnect();
        status.charging = status.present && pmu->isCharging();
        mv = status.present ? pmu->getBattVoltage() : 0;
    }
    else
    {
        status.present = true;
        status.charging = false;
        mv = readAdcMv();
    }

    if (!status.present)
    {
        status.voltage_mv = 0;
        status.percent = 0;
        status.valid = true;
        return;
    }

    status.voltage_mv = batteryFilterMv(status.voltage_mv, mv, !status.valid || status.voltage_mv == 0);
    status.percent = batteryPercentFromMv(status.voltage_mv);
    status.valid = true;
}

uint16_t BatteryMonitor::readAdcMv()
{
    uint32_t sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++)
        sum += analogReadMilliVolts(BATTERY_ADC_PIN); // eFuse-calibrated, unlike raw analogRead()
    return (uint16_t)((sum / ADC_SAMPLES) * BATTERY_SCALE_FACTOR);
}
