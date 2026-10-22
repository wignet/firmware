/**
 * Display HAL Implementation: Waveshare ePaper ESP32 Driver Board + 3.52" BW
 *
 * Targets:
 * - Waveshare Universal e-Paper Driver Board (ESP32)
 * - Wemos/Lolin D1 mini ESP8266 + Waveshare e-Paper Driver HAT
 * Display: 3.52" BW (240x360) - UC8253c controller
 *
 * This implementation uses a custom GxEPD2 driver class to support the 3.52"
 * panel based on the official Waveshare Arduino driver.
 */

#define ENABLE_GxEPD2_GFX 1
#include "display_hal.h"
#include <GxEPD2_BW.h>
#include <GxEPD2_EPD.h>
#include <SPI.h>

// ============ Pin Definitions ============
#if defined(TARGET_D1_MINI_35)
// Same SPI/HAT wiring as the documented D1 mini 7.5" setup.
#define EPD_BUSY D2 // GPIO4
#define EPD_RST D4  // GPIO2
#define EPD_DC D3   // GPIO0
#define EPD_CS D8   // GPIO15
#else
#define EPD_BUSY 25
#define EPD_RST 26
#define EPD_DC 27
#define EPD_CS 15
#define EPD_SCK 13
#define EPD_MOSI 14
#endif

// ============ Custom GxEPD2 Driver for 3.52" ============

class GxEPD2_352_Wignet : public GxEPD2_EPD {
public:
  static const uint16_t WIDTH = 240;
  static const uint16_t WIDTH_VISIBLE = WIDTH;
  static const uint16_t HEIGHT = 360;
  static const GxEPD2::Panel panel = GxEPD2::GDEW0371W7; // Dummy ID
  static const bool hasColor = false;
  static const bool hasPartialUpdate = true;
  static const bool usePartialUpdateWindow = true;
  static const bool hasFastPartialUpdate = true;
  static const bool useFastFullUpdate = true;
  static const uint16_t power_on_time = 100;
  static const uint16_t power_off_time = 100;
  static const uint16_t full_refresh_time = 2000;
  static const uint16_t partial_refresh_time = 500;

  GxEPD2_352_Wignet(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
      : GxEPD2_EPD(cs, dc, rst, busy, HIGH, 10000000, WIDTH, HEIGHT, panel,
                   hasColor, hasPartialUpdate, hasFastPartialUpdate) {}

  // Virtual methods from GxEPD2_EPD
  void clearScreen(uint8_t value) override {
    _initial_write = false;
    _writeScreenBuffer(0x13, value);
    _Update_Full();
  }
  void writeScreenBuffer(uint8_t value) override {
    _writeScreenBuffer(0x13, value);
  }
  void writeScreenBufferAgain(uint8_t value = 0xFF) override {
    _writeScreenBuffer(0x10, value);
  }
  void writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w,
                  int16_t h, bool invert = false, bool mirror_y = false,
                  bool pgm = false) override {
    _writeImage(0x13, bitmap, x, y, w, h, invert, mirror_y, pgm);
  }
  void writeImageForFullRefresh(const uint8_t bitmap[], int16_t x, int16_t y,
                                int16_t w, int16_t h, bool invert = false,
                                bool mirror_y = false,
                                bool pgm = false) override {
    _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm);
    _writeImage(0x13, bitmap, x, y, w, h, invert, mirror_y, pgm);
  }
  void writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part,
                      int16_t w_bitmap, int16_t h_bitmap, int16_t x, int16_t y,
                      int16_t w, int16_t h, bool invert = false,
                      bool mirror_y = false, bool pgm = false) override {
    _writeImage(0x13, bitmap, x, y, w, h, invert, mirror_y, pgm);
  }
  void writeImageAgain(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w,
                       int16_t h, bool invert = false, bool mirror_y = false,
                       bool pgm = false) override {
    _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm);
  }
  void writeImagePartAgain(const uint8_t bitmap[], int16_t x_part,
                           int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                           int16_t x, int16_t y, int16_t w, int16_t h,
                           bool invert = false, bool mirror_y = false,
                           bool pgm = false) override {
    _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm);
  }
  void refresh(bool partial_update_mode = false) override {
    if (partial_update_mode)
      _Update_Part();
    else
      _Update_Full();
  }
  void refresh(int16_t x, int16_t y, int16_t w, int16_t h) override {
    _Update_Part();
  }
  void powerOff() override {}
  void hibernate() override {
    _writeCommand(0x07);
    _writeData(0xA5);
  }

  // Methods required by GxEPD2_BW template
  void writeImage(const uint8_t *black, const uint8_t *color, int16_t x,
                  int16_t y, int16_t w, int16_t h, bool invert = false,
                  bool mirror_y = false, bool pgm = false) {
    if (black)
      writeImage(black, x, y, w, h, invert, mirror_y, pgm);
  }
  void writeImagePart(const uint8_t *black, const uint8_t *color,
                      int16_t x_part, int16_t y_part, int16_t w_bitmap,
                      int16_t h_bitmap, int16_t x, int16_t y, int16_t w,
                      int16_t h, bool invert = false, bool mirror_y = false,
                      bool pgm = false) {
    if (black)
      writeImagePart(black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h,
                     invert, mirror_y, pgm);
  }
  void writeNative(const uint8_t *data1, const uint8_t *data2, int16_t x,
                   int16_t y, int16_t w, int16_t h, bool invert = false,
                   bool mirror_y = false, bool pgm = false) {
    if (data1)
      _writeImage(0x13, data1, x, y, w, h, invert, mirror_y, pgm);
  }
  void drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w,
                 int16_t h, bool invert = false, bool mirror_y = false,
                 bool pgm = false) {
    writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
    refresh(x, y, w, h);
  }
  void drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part,
                     int16_t w_bitmap, int16_t h_bitmap, int16_t x, int16_t y,
                     int16_t w, int16_t h, bool invert = false,
                     bool mirror_y = false, bool pgm = false) {
    writeImagePart(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h,
                   invert, mirror_y, pgm);
    refresh(x, y, w, h);
  }
  void drawImage(const uint8_t *black, const uint8_t *color, int16_t x,
                 int16_t y, int16_t w, int16_t h, bool invert = false,
                 bool mirror_y = false, bool pgm = false) {
    if (black)
      drawImage(black, x, y, w, h, invert, mirror_y, pgm);
  }
  void drawImagePart(const uint8_t *black, const uint8_t *color, int16_t x_part,
                     int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                     int16_t x, int16_t y, int16_t w, int16_t h,
                     bool invert = false, bool mirror_y = false,
                     bool pgm = false) {
    if (black)
      drawImagePart(black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h,
                    invert, mirror_y, pgm);
  }
  void drawNative(const uint8_t *data1, const uint8_t *data2, int16_t x,
                  int16_t y, int16_t w, int16_t h, bool invert = false,
                  bool mirror_y = false, bool pgm = false) {
    if (data1)
      drawImage(data1, x, y, w, h, invert, mirror_y, pgm);
  }

protected:
  uint8_t _readData() {
    _startTransfer();
    uint8_t data = _pSPIx->transfer(0x00);
    _endTransfer();
    return data;
  }

  void _waitWhileBusy_Software(const char *comment, uint16_t busy_time) {
    if (comment && *comment) {
      Serial.print(comment);
      Serial.print(" : ");
      Serial.println("Software Busy Check");
    }
    unsigned long start = millis();
    while (1) {
      _writeCommand(0x71);
      // Waveshare controller status: bit 0 set means the panel is ready.
      if (_readData() & 0x01)
        break;

      if (millis() - start > busy_time) {
        Serial.println("Busy Timeout!");
        break;
      }
      delay(10);
      yield(); // Avoid WDT reset
    }
  }

  void _InitDisplay() {
    _reset();
    _waitWhileBusy_Software("_InitDisplay", 5000);

    _writeCommand(0x00);
    _writeData(0xFF);
    _writeData(0x01);
    _writeCommand(0x01);
    _writeData(0x03);
    _writeData(0x10);
    _writeData(0x3F);
    _writeData(0x3F);
    _writeData(0x03);
    _writeCommand(0x06);
    _writeData(0x37);
    _writeData(0x3D);
    _writeData(0x3D);
    _writeCommand(0x60);
    _writeData(0x22);
    _writeCommand(0x82);
    _writeData(0x07);
    _writeCommand(0x30);
    _writeData(0x09);
    _writeCommand(0xE3);
    _writeData(0x88);
    _writeCommand(0x61);
    _writeData(0xF0);
    _writeData(0x01);
    _writeData(0x68);
    _writeCommand(0x50);
    _writeData(0xB7);
    _init_display_done = true;
  }

  void _Update_Full() {
    if (!_init_display_done)
      _InitDisplay();
    _writeScreenBuffer(0x10, 0xFF); // Init Old Data to White
    _lut_GC();
    _writeCommand(0x17);
    _writeData(0xA5);
    _waitWhileBusy_Software("_Update_Full", 2000);
  }

  void _Update_Part() {
    if (!_init_display_done)
      _InitDisplay();
    _lut_DU();
    _writeCommand(0x17);
    _writeData(0xA5);
    _waitWhileBusy_Software("_Update_Part", 1000);
  }

  void _writeScreenBuffer(uint8_t command, uint8_t value) {
    if (!_init_display_done)
      _InitDisplay();
    _writeCommand(command);
    _startTransfer();
    // Data Inversion Removed: Sending value directly
    for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++)
      _transfer(value);
    _endTransfer();
  }

  void _writeImage(uint8_t command, const uint8_t bitmap[], int16_t x,
                   int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y,
                   bool pgm) {
    if (!_init_display_done)
      _InitDisplay();
    _writeCommand(command);
    _startTransfer();
    for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++) {
      uint8_t data = pgm ? pgm_read_byte(&bitmap[i]) : bitmap[i];
      if (invert)
        data = ~data;
      // Data Inversion Removed: Sending data directly
      _transfer(data);
    }
    _endTransfer();
  }

private:
  static const uint8_t lut_R20_GC[];
  static const uint8_t lut_R21_GC[];
  static const uint8_t lut_R22_GC[];
  static const uint8_t lut_R23_GC[];
  static const uint8_t lut_R24_GC[];

  static const uint8_t lut_R20_DU[];
  static const uint8_t lut_R21_DU[];
  static const uint8_t lut_R22_DU[];
  static const uint8_t lut_R23_DU[];
  static const uint8_t lut_R24_DU[];

  void _lut_GC() {
    _writeCommand(0x20);
    for (int i = 0; i < 56; i++)
      _writeData(lut_R20_GC[i]);
    _writeCommand(0x21);
    for (int i = 0; i < 42; i++)
      _writeData(lut_R21_GC[i]);
    _writeCommand(0x24);
    for (int i = 0; i < 42; i++)
      _writeData(lut_R24_GC[i]);
    _writeCommand(0x22);
    for (int i = 0; i < 56; i++)
      _writeData(lut_R22_GC[i]);
    _writeCommand(0x23);
    for (int i = 0; i < 56; i++)
      _writeData(lut_R23_GC[i]);
  }

  void _lut_DU() {
    _writeCommand(0x20);
    for (int i = 0; i < 56; i++)
      _writeData(lut_R20_DU[i]);
    _writeCommand(0x21);
    for (int i = 0; i < 42; i++)
      _writeData(lut_R21_DU[i]);
    _writeCommand(0x24);
    for (int i = 0; i < 42; i++)
      _writeData(lut_R24_DU[i]);
    _writeCommand(0x22);
    for (int i = 0; i < 56; i++)
      _writeData(lut_R22_DU[i]);
    _writeCommand(0x23);
    for (int i = 0; i < 56; i++)
      _writeData(lut_R23_DU[i]);
  }
};

// ============ LUT Data Definitions ============

const uint8_t GxEPD2_352_Wignet::lut_R20_GC[] = {
    0x01, 0x0f, 0x0f, 0x0f, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t GxEPD2_352_Wignet::lut_R21_GC[] = {
    0x01, 0x4f, 0x8f, 0x0f, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t GxEPD2_352_Wignet::lut_R22_GC[] = {
    0x01, 0x0f, 0x8f, 0x0f, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t GxEPD2_352_Wignet::lut_R23_GC[] = {
    0x01, 0x4f, 0x8f, 0x4f, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t GxEPD2_352_Wignet::lut_R24_GC[] = {
    0x01, 0x0f, 0x8f, 0x4f, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t GxEPD2_352_Wignet::lut_R20_DU[] = {
    0x01, 0x0f, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t GxEPD2_352_Wignet::lut_R21_DU[] = {
    0x01, 0x0f, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t GxEPD2_352_Wignet::lut_R22_DU[] = {
    0x01, 0x8f, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t GxEPD2_352_Wignet::lut_R23_DU[] = {
    0x01, 0x4f, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
const uint8_t GxEPD2_352_Wignet::lut_R24_DU[] = {
    0x01, 0x0f, 0x01, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// ============ HAL Implementation ============

static GxEPD2_BW<GxEPD2_352_Wignet, GxEPD2_352_Wignet::HEIGHT>
    display(GxEPD2_352_Wignet(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

namespace DisplayHAL {

void init() {
  Serial.println("[HAL] Initializing SPI...");
#if defined(TARGET_D1_MINI_35)
  // ESP8266 hardware SPI pins: CLK=D5/GPIO14, DIN=D7/GPIO13.
  SPI.begin();
#else
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
#endif
  Serial.println("[HAL] Initializing Display...");
  display.init(
      0); // Disable library internal diagnostics to avoid serial conflict
  display.setRotation(1); // Landscape orientation (360x240)
  Serial.println("[HAL] Display initialized.");
}

void hibernate() { display.hibernate(); }

GxEPD2_GFX *getDisplay() { return &display; }

int getWidth() { return GxEPD2_352_Wignet::HEIGHT; }

int getHeight() { return GxEPD2_352_Wignet::WIDTH; }

} // namespace DisplayHAL
