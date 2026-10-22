/**
 * Display HAL Implementation: DFRobot FireBeetle ESP32
 *
 * Target: DFRobot FireBeetle ESP32
 * Display: 7.5" V2 (800x480) - GxEPD2_750_T7
 *
 * Wiring (Based on User Soldering + DIO Flash Mode Test):
 *   BUSY -> IO27
 *   RST  -> IO21
 *   DC   -> IO22
 *   CS   -> IO9 (Trying DIO mode to avoid flash conflict)
 *   CLK  -> IO18
 *   DIN  -> IO23
 */

#define ENABLE_GxEPD2_GFX 1
#include "display_hal.h"
#include <SPI.h>

// ============ Pin Definitions ============
#define EPD_BUSY 27
#define EPD_RST 21
#define EPD_DC 22
#define EPD_CS 9 // Testing D5/IO9 with DIO mode
// CLK (18) and MOSI (23) are default VSPI

// ============ Display Configuration ============
#define DISPLAY_WIDTH 800
#define DISPLAY_HEIGHT 480

// Display driver instance (7.5" V2)
static GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT>
    display(GxEPD2_750_T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

// ============ HAL Implementation ============

namespace DisplayHAL {

void init() {
  SPI.begin();

  // Enable internal pull-up on BUSY pin (GPIO 27)
  pinMode(EPD_BUSY, INPUT_PULLUP);

  // Waveshare HAT - shortened reset pulse
  display.init(115200, true, 2, false);
}

void hibernate() { display.hibernate(); }

GxEPD2_GFX *getDisplay() { return &display; }

int getWidth() { return DISPLAY_WIDTH; }

int getHeight() { return DISPLAY_HEIGHT; }

} // namespace DisplayHAL
