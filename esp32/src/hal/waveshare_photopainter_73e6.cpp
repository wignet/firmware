/**
 * Display HAL: Waveshare ESP32-S3 PhotoPainter Rev2.0
 *
 * Board: ESP32-S3-WROOM-1-N16R8
 * Panel: 7.3" 800x480 Spectra 6 (GDEP073E01 / HT073A01)
 *
 * The e-paper rail is supplied by ALDO4 on the board's AXP2101 PMIC. It must
 * be enabled before the panel is initialized and disabled before deep sleep.
 */

#define ENABLE_GxEPD2_GFX 1
#include "display_hal.h"
#include <Arduino.h>
#include <GxEPD2_7C.h>
#include <SPI.h>
#include <Wire.h>

namespace {

constexpr int EPD_DC = 8;
constexpr int EPD_CS = 9;
constexpr int EPD_SCK = 10;
constexpr int EPD_MOSI = 11;
constexpr int EPD_RST = 12;
constexpr int EPD_BUSY = 13;

constexpr int PMIC_SDA = 47;
constexpr int PMIC_SCL = 48;
constexpr uint8_t AXP2101_ADDRESS = 0x34;
constexpr uint8_t AXP2101_LDO_ENABLE_0 = 0x90;
constexpr uint8_t AXP2101_ALDO4_VOLTAGE = 0x95;
constexpr uint8_t AXP2101_ALDO4_ENABLE = 0x08;
constexpr uint8_t AXP2101_ALDO4_3300MV = 0x1C;

constexpr int DISPLAY_WIDTH = 800;
constexpr int DISPLAY_HEIGHT = 480;
constexpr uint16_t DISPLAY_PAGE_HEIGHT = 60;

// Spectra 6 refreshes can exceed the upstream driver's generic 20-second BUSY
// timeout. Waiting up to 60 seconds prevents sleep/power-off from interrupting
// a valid in-progress refresh.
class PhotoPainterPanel : public GxEPD2_730c_GDEP073E01 {
public:
  PhotoPainterPanel(int16_t cs, int16_t dc, int16_t rst, int16_t busy)
      : GxEPD2_730c_GDEP073E01(cs, dc, rst, busy) {
    _busy_timeout = 60000000;
  }
};

static GxEPD2_7C<PhotoPainterPanel, DISPLAY_PAGE_HEIGHT> display(
    PhotoPainterPanel(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

bool readPmicRegister(uint8_t reg, uint8_t &value) {
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    Wire.beginTransmission(AXP2101_ADDRESS);
    Wire.write(reg);
    // End the address phase with STOP. The ESP32-S3 I2C peripheral could time
    // out on a repeated-start here after a long Spectra refresh.
    if (Wire.endTransmission(true) == 0) {
      delayMicroseconds(100);
      if (Wire.requestFrom(AXP2101_ADDRESS, static_cast<uint8_t>(1)) == 1) {
        value = Wire.read();
        return true;
      }
    }
    delay(5);
  }
  return false;
}

bool writePmicRegister(uint8_t reg, uint8_t value) {
  for (uint8_t attempt = 0; attempt < 3; attempt++) {
    Wire.beginTransmission(AXP2101_ADDRESS);
    Wire.write(reg);
    Wire.write(value);
    if (Wire.endTransmission(true) == 0) {
      return true;
    }
    delay(5);
  }
  return false;
}

bool setDisplayPower(bool enabled) {
  uint8_t enableRegister = 0;
  if (!readPmicRegister(AXP2101_LDO_ENABLE_0, enableRegister)) {
    Serial.println("[HAL] Could not read AXP2101 LDO control register");
    return false;
  }

  if (enabled) {
    uint8_t voltageRegister = 0;
    if (!readPmicRegister(AXP2101_ALDO4_VOLTAGE, voltageRegister) ||
        !writePmicRegister(AXP2101_ALDO4_VOLTAGE,
                           (voltageRegister & 0xE0) |
                               AXP2101_ALDO4_3300MV)) {
      Serial.println("[HAL] Could not set AXP2101 ALDO4 to 3.3V");
      return false;
    }
    enableRegister |= AXP2101_ALDO4_ENABLE;
  } else {
    enableRegister &= ~AXP2101_ALDO4_ENABLE;
  }

  if (!writePmicRegister(AXP2101_LDO_ENABLE_0, enableRegister)) {
    Serial.println("[HAL] Could not change AXP2101 ALDO4 state");
    return false;
  }
  return true;
}

} // namespace

namespace DisplayHAL {

void init() {
  Wire.begin(PMIC_SDA, PMIC_SCL);
  Wire.setClock(100000);

  if (setDisplayPower(true)) {
    Serial.println("[HAL] PhotoPainter e-paper power enabled");
  }
  delay(100);

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(0, true, 20, false, SPI,
               SPISettings(10000000, MSBFIRST, SPI_MODE0));
  // The panel exposes BUSY as an active-low/open-drain signal; match the
  // pull-up used by Waveshare's factory firmware.
  pinMode(EPD_BUSY, INPUT_PULLUP);
  Serial.println("[HAL] PhotoPainter 7.3-inch Spectra 6 initialized");
}

void hibernate() {
  display.hibernate();
  display.end();
  delay(10);
  if (setDisplayPower(false)) {
    Serial.println("[HAL] PhotoPainter e-paper power disabled");
  }
}

GxEPD2_GFX *getDisplay() { return &display; }

int getWidth() { return DISPLAY_WIDTH; }

int getHeight() { return DISPLAY_HEIGHT; }

bool renderPackedColor(uint8_t *pixels, size_t length, int width,
                       int height) {
  const size_t pixelCount =
      static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t expectedLength = pixelCount / 2U + (pixelCount & 1U);
  if (!pixels || width != DISPLAY_WIDTH || height != DISPLAY_HEIGHT ||
      length != expectedLength) {
    Serial.println("[HAL] Refusing invalid packed color frame");
    return false;
  }

  // The PhotoPainter panel is physically mounted at 180 degrees. WGN6 uses
  // two row-major pixels per byte (high nibble first), so reversing the byte
  // order and swapping each pair rotates the complete frame without another
  // 192 KB PSRAM allocation. The caller frees this disposable payload after
  // the refresh.
  size_t left = 0;
  size_t right = length - 1;
  while (left < right) {
    const uint8_t leftByte = pixels[left];
    const uint8_t rightByte = pixels[right];
    pixels[left] = static_cast<uint8_t>((rightByte << 4) | (rightByte >> 4));
    pixels[right] = static_cast<uint8_t>((leftByte << 4) | (leftByte >> 4));
    left++;
    right--;
  }
  if (left == right) {
    pixels[left] = static_cast<uint8_t>((pixels[left] << 4) |
                                        (pixels[left] >> 4));
  }

  // writeNative() accepts GxEPD2's generic 4-bit color codes when invert is
  // false. The GDEP073E01 driver then performs its required controller-specific
  // palette conversion while streaming the already validated PSRAM buffer.
  display.epd2.writeNative(pixels, nullptr, 0, 0, width, height, false, false,
                           false);
  display.epd2.refresh(false);
  display.epd2.powerOff();
  return true;
}

} // namespace DisplayHAL
