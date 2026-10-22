/**
 * Display HAL Implementation: Waveshare ePaper ESP32 Driver Board + 2.7"
 * Display
 *
 * Target: Waveshare Universal e-Paper Driver Board (ESP32)
 * Display: 2.7" (264x176) - GxEPD2_270
 *
 * Pin Configuration (Waveshare board standard):
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
#define DISPLAY_WIDTH 264
#define DISPLAY_HEIGHT 176

// Display driver instance (2.7")
static GxEPD2_BW<GxEPD2_270, GxEPD2_270::HEIGHT>
    display(GxEPD2_270(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

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
