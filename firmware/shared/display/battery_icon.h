#ifndef SHARED_BATTERY_ICON_H
#define SHARED_BATTERY_ICON_H

#include <OLEDDisplay.h>
#include "battery/battery_level.h"

/*
 * Battery icon for the 128x64 SSD1306 header (ArialMT_Plain_10 title at y=0,
 * separator line at y=12). Occupies x=104..127, y=1..9:
 *
 *   bolt (5x8, charging only)   body 15x9 + nub 2x5, 4 fill segments
 *        x=104..108                     x=111..127
 *
 * Fill: 1 segment per started 25 %. Below the low threshold (and not
 * charging) the outline stays visible and a single segment blinks.
 * No battery (PMU-reported): empty outline with a dash inside.
 */

#define BATTERY_ICON_X 111
#define BATTERY_ICON_Y 1
#define BATTERY_ICON_BODY_W 15
#define BATTERY_ICON_BODY_H 9
#define BATTERY_ICON_SEGMENTS 4
#define BATTERY_ICON_BOLT_X 104
#define BATTERY_ICON_BOLT_Y 2
#define BATTERY_ICON_BOLT_W 5
#define BATTERY_ICON_BOLT_H 8

// XBM, LSB = leftmost pixel
static const uint8_t BATTERY_BOLT_XBM[] = {0x18, 0x0C, 0x06, 0x1F, 0x0C, 0x06, 0x03, 0x01};

/**
 * @brief Draw the battery icon in the top-right corner of the header.
 * @param display     Target display (frame buffer only, caller calls display())
 * @param status      Current battery state
 * @param low_percent Blink threshold in percent
 * @param now_ms      millis() timestamp, drives the blink phase
 * @param blink_ms    Half-period of the low-battery blink
 */
inline void drawBatteryIcon(OLEDDisplay &display, const BatteryStatus &status,
                            uint8_t low_percent, uint32_t now_ms, uint32_t blink_ms)
{
    if (!status.valid)
        return;

    const int16_t x = BATTERY_ICON_X;
    const int16_t y = BATTERY_ICON_Y;

    display.setColor(WHITE);
    display.drawRect(x, y, BATTERY_ICON_BODY_W, BATTERY_ICON_BODY_H);
    display.fillRect(x + BATTERY_ICON_BODY_W, y + 2, 2, BATTERY_ICON_BODY_H - 4);

    if (!status.present)
    {
        display.drawHorizontalLine(x + 4, y + BATTERY_ICON_BODY_H / 2, BATTERY_ICON_BODY_W - 8);
        return;
    }

    if (status.charging)
        display.drawXbm(BATTERY_ICON_BOLT_X, BATTERY_ICON_BOLT_Y, BATTERY_ICON_BOLT_W, BATTERY_ICON_BOLT_H,
                        BATTERY_BOLT_XBM);

    uint8_t segments = (status.percent + 24) / 25; // 1..25 % -> 1, 76..100 % -> 4
    if (segments > BATTERY_ICON_SEGMENTS)
        segments = BATTERY_ICON_SEGMENTS;

    if (status.percent < low_percent && !status.charging)
    {
        bool blink_on = blink_ms == 0 || ((now_ms / blink_ms) % 2) == 0;
        segments = blink_on ? 1 : 0;
    }

    // Segments: 2 px wide, 1 px gap, 1 px clearance from the outline
    const int16_t seg_y = y + 2;
    const int16_t seg_h = BATTERY_ICON_BODY_H - 4;
    for (uint8_t i = 0; i < segments; i++)
        display.fillRect(x + 2 + i * 3, seg_y, 2, seg_h);
}

#endif // SHARED_BATTERY_ICON_H
