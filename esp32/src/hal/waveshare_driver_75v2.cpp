/**
 * Display HAL Implementation: Waveshare ePaper ESP32 Driver Board
 *
 * Target: Waveshare Universal e-Paper Driver Board (ESP32)
 * Display: 7.5" V2 (800x480) - GxEPD2_750_T7
 *
 * Pin Configuration:
 *   BUSY -> GPIO 25
 *   RST  -> GPIO 26
 *   DC   -> GPIO 27
 *   CS   -> GPIO 15
 *   CLK  -> GPIO 13
 *   DIN  -> GPIO 14
 */

#define ENABLE_GxEPD2_GFX 1
#include "display_hal.h"
#include <SPI.h>

// ============ Pin Definitions ============
#define EPD_BUSY 25
#define EPD_RST 26
#define EPD_DC 27
#define EPD_CS 15
#define EPD_SCK 13
#define EPD_MOSI 14

// ============ Display Configuration ============
#define DISPLAY_WIDTH 800
#define DISPLAY_HEIGHT 480

// Display driver instance (7.5" V2)
static GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT>
    display(GxEPD2_750_T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ============ HAL Implementation ============

namespace DisplayHAL {

void init() {
  // Initialize custom SPI pins for Waveshare board
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init();
}

void hibernate() { display.hibernate(); }

GxEPD2_GFX *getDisplay() { return &display; }

int getWidth() { return DISPLAY_WIDTH; }

int getHeight() { return DISPLAY_HEIGHT; }

} // namespace DisplayHAL
