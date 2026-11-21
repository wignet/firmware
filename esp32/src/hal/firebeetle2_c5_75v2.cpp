/**
 * Display HAL Implementation: DFRobot FireBeetle 2 ESP32-C5 (DFR1222)
 *
 * Target: FireBeetle 2 ESP32-C5 + Waveshare e-Paper Driver HAT
 * Display: 7.5" V2 (800x480) - GxEPD2_750_T7
 *
 * Wiring (proposed 2026-11-20, not yet soldered or tested). It follows the
 * board's own GDI e-paper connector, so the same pins serve a GDI panel:
 *
 *   HAT    FireBeetle 2 C5   GPIO  Note
 *   VCC    3V3_C             -     switched by GPIO0 (HIGH = on), see below
 *   GND    GND               -
 *   DIN    MOSI              24
 *   CLK    SCK               23
 *   CS     D6                27    strapping (boot mode, ROM log); harmless
 *                                  while GPIO28/BOOT is high at reset
 *   DC     D2                 8
 *   RST    D3                26    strapping (boot mode); same as above
 *   BUSY   D12                6    the GDI connector has no BUSY: our choice
 *   PWR    3V3_C             -     only on HAT Rev 2.3+: tie it to VCC
 *
 * Avoid: GPIO2, 3, 7, 25 (strapping), 13/14 (USB), 16-22 (flash), 28 (BOOT
 * button), 1 (battery voltage), 15 (LED / GDI backlight).
 *
 * Power: the HAT runs from 3V3_C, which the board switches with GPIO0 (off
 * at reset). init() switches it on; hibernate(), the last display call
 * before deep sleep, puts the panel to sleep and then cuts the rail, so the
 * HAT draws nothing while the frame sleeps.
 */

#define ENABLE_GxEPD2_GFX 1
#include "display_hal.h"
#include <SPI.h>

// ============ Pin Definitions ============
#define EPD_POWER 0 // 3V3_C enable, HIGH = on
#define EPD_SCK 23
#define EPD_MOSI 24
#define EPD_CS 27
#define EPD_DC 8
#define EPD_RST 26
#define EPD_BUSY 6

// ============ Display Configuration ============
#define DISPLAY_WIDTH 800
#define DISPLAY_HEIGHT 480

// Display driver instance (7.5" V2)
static GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT>
    display(GxEPD2_750_T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ============ HAL Implementation ============

namespace DisplayHAL {

void init() {
  pinMode(EPD_POWER, OUTPUT);
  digitalWrite(EPD_POWER, HIGH);
  delay(10); // let the HAT's supply settle before the reset pulse

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  pinMode(EPD_BUSY, INPUT_PULLUP);

  // Waveshare HAT: shortened reset pulse, as on the first FireBeetle.
  display.init(115200, true, 2, false);
}

void hibernate() {
  display.hibernate();
  // Release the signal lines before cutting 3V3_C, so the unpowered HAT is
  // not fed through its inputs.
  SPI.end();
  for (int pin : {EPD_CS, EPD_DC, EPD_RST, EPD_SCK, EPD_MOSI}) {
    pinMode(pin, INPUT);
  }
  digitalWrite(EPD_POWER, LOW);
}

GxEPD2_GFX *getDisplay() { return &display; }

int getWidth() { return DISPLAY_WIDTH; }

int getHeight() { return DISPLAY_HEIGHT; }

} // namespace DisplayHAL
