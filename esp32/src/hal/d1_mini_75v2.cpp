/**
 * Display HAL Implementation: Wemos/Lolin D1 mini ESP8266
 *
 * Target: D1 mini / ESP8266 + Waveshare e-Paper HAT
 * Display: 7.5" V2 (800x480) - GxEPD2_750_T7
 *
 * Wiring:
 *   BUSY -> D2 / GPIO4
 *   RST  -> D4 / GPIO2
 *   DC   -> D3 / GPIO0
 *   CS   -> D8 / GPIO15
 *   CLK  -> D5 / GPIO14
 *   DIN  -> D7 / GPIO13
 */

#define ENABLE_GxEPD2_GFX 1
#include "display_hal.h"
#include <Arduino.h>
#include <SPI.h>

// ============ Pin Definitions ============
#define EPD_BUSY D2
#define EPD_RST D4
#define EPD_DC D3
#define EPD_CS D8

// ============ Display Configuration ============
#define DISPLAY_WIDTH 800
#define DISPLAY_HEIGHT 480

// Use a smaller page buffer on ESP8266 to leave RAM for WiFi and HTTP.
static GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT / 4>
    display(GxEPD2_750_T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ============ HAL Implementation ============

namespace DisplayHAL {

void init() {
  SPI.begin();
  pinMode(EPD_BUSY, INPUT);
  display.init(115200, true, 2, false);
}

void hibernate() { display.hibernate(); }

GxEPD2_GFX *getDisplay() { return &display; }

int getWidth() { return DISPLAY_WIDTH; }

int getHeight() { return DISPLAY_HEIGHT; }

} // namespace DisplayHAL
