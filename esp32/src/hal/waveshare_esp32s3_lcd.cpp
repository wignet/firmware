/**
 * Display HAL Implementation: Waveshare ESP32-S3 LCD boards (ST7789)
 *
 * Boards: ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM, an IPS ST7789 over
 * 4-wire SPI, colours inverted (INVON):
 * - ESP32-S3-LCD-1.54: 240x240, turned by its accelerometer (docs/lcd_154.md).
 *   Pins from waveshareteam/ESP32-S3-Touch-LCD-1.54, ESP-IDF 01_factory.
 * - ESP32-S3-LCD-2: 240x320, natively portrait; its frame is the landscape
 *   buffer, 320x240, turned once onto the panel (docs/lcd_2.md). Pins from
 *   Waveshare's ESP32-S3-LCD-2-Demo.zip (ESP-IDF 06_lvgl_example).
 *
 * An LCD is not an e-paper panel, but main.cpp draws its own screens
 * (startup, provisioning, text) through GxEPD2. A GxEPD2 driver here takes
 * those 1-bit pages and writes them into an upright RGB565 frame; a PNG is
 * decoded straight into the same frame (main.cpp, displayPngFromStream).
 * showFrame() sends it to the panel turned by the accelerometer's quarter
 * turn (orientation_hal.cpp), so turning never needs a redraw, and through
 * this panel's colour calibration (below), in its 18-bit mode.
 */

#define ENABLE_GxEPD2_GFX 1
#include "display_hal.h"
#include <GxEPD2_BW.h>
#include <GxEPD2_EPD.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============ Boards ============
// PANEL_*: the controller's own scan. FRAME_*: the upright frame main.cpp
// draws and the server renders (getWidth/getHeight, the landscape buffer).
// MOUNT_TURN: the clockwise quarter turn from the frame to the panel, fixed
// by how the panel sits; a square board adds the accelerometer's (setTurn).
#if defined(TARGET_WAVESHARE_ESP32S3_LCD_2)
#define LCD_SCLK 39
#define LCD_MOSI 38
#define LCD_CS 45
#define LCD_DC 42
#define LCD_RST -1 // none: SWRESET only
#define LCD_BL 1
#define SD_CS 41   // the microSD card shares the SPI bus: kept deselected
static constexpr int PANEL_W = 240, PANEL_H = 320;
static constexpr int FRAME_W = 320, FRAME_H = 240;
static constexpr int MOUNT_TURN = 90;
#else // TARGET_WAVESHARE_ESP32S3_LCD_154
#define LCD_SCLK 38
#define LCD_MOSI 39
#define LCD_CS 21
#define LCD_DC 45
#define LCD_RST 40
#define LCD_BL 46
static constexpr int PANEL_W = 240, PANEL_H = 240;
static constexpr int FRAME_W = 240, FRAME_H = 240;
static constexpr int MOUNT_TURN = 0;
#endif
static constexpr int FRAME_PIXELS = FRAME_W * FRAME_H;
static constexpr uint32_t SPI_HZ = 40000000;

// ============ Colour calibration ============
// Measured on the 1.54" panel; the 2" one starts from the same values until
// a test card says otherwise. This panel at full backlight shows a cool white and lifts dark tones: the
// brand cream #F7F2E8 read as white, #332021 as a light brown. Picked by eye
// against the brand colours on a Mac screen, from test cards sent over the
// bench console (2026-10-16): a gamma of 1.75 on top of the panel's own, then
// green x0.97 and blue x0.90 for a warmer white. It applies to everything the
// panel shows, the platform's PNGs included: the server stays the same for
// every screen. Frames stay RGB565; the panel takes 18 bits a pixel, so the
// curve keeps 6 bits a channel on the way out.
static constexpr float CALIBRATION_GAMMA = 1.75f;
static constexpr float CALIBRATION_GAIN[3] = {1.00f, 0.97f, 0.90f};
// RGB565 channel value -> the byte the panel takes (6 bits, left-aligned).
static uint8_t calibratedRed[32], calibratedGreen[64], calibratedBlue[32];

static void buildCalibration() {
  auto fill = [](uint8_t *table, int levels, float gain) {
    for (int i = 0; i < levels; i++) {
      const float out = powf(static_cast<float>(i) / (levels - 1), CALIBRATION_GAMMA) * gain;
      table[i] = static_cast<uint8_t>(lroundf(out * 63.0f)) << 2;
    }
  };
  fill(calibratedRed, 32, CALIBRATION_GAIN[0]);
  fill(calibratedGreen, 64, CALIBRATION_GAIN[1]);
  fill(calibratedBlue, 32, CALIBRATION_GAIN[2]);
}

// The upright frame, big-endian RGB565 (PSRAM), and one panel row (RGB666).
static uint16_t *uprightFrame = nullptr;
static uint8_t rowBuffer[PANEL_W * 3];
static int currentTurn = 0;
// The orientation task (orientation_hal.cpp) repaints from its own task
// while the loop may be drawing: one frame goes to the panel at a time.
static SemaphoreHandle_t panelLock = nullptr;
// Off until the first frame has reached the panel: what lit up first is a
// real screen, never the controller's leftover RAM.
static bool backlightOn = false;

// ============ ST7789 ============

static void lcdCommand(uint8_t command, const uint8_t *data = nullptr,
                       size_t length = 0) {
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_CS, LOW);
  digitalWrite(LCD_DC, LOW);
  SPI.write(command);
  if (length) {
    digitalWrite(LCD_DC, HIGH);
    SPI.writeBytes(data, length);
  }
  digitalWrite(LCD_CS, HIGH);
  SPI.endTransaction();
}

static void lcdInit() {
  pinMode(LCD_CS, OUTPUT);
  pinMode(LCD_DC, OUTPUT);
  digitalWrite(LCD_CS, HIGH);
#if defined(SD_CS)
  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
#endif
  if (LCD_RST >= 0) {
    pinMode(LCD_RST, OUTPUT);
    digitalWrite(LCD_RST, HIGH);
    delay(10);
    digitalWrite(LCD_RST, LOW);
    delay(10);
    digitalWrite(LCD_RST, HIGH);
    delay(120);
  }

  lcdCommand(0x01); // SWRESET
  delay(150);
  lcdCommand(0x11); // SLPOUT
  delay(120);
  const uint8_t colmod = 0x66; // 18 bits per pixel: the calibration's 6 bits
  lcdCommand(0x3A, &colmod, 1);
  const uint8_t madctl = 0x00; // native scan, RGB order: turns are in software
  lcdCommand(0x36, &madctl, 1);
  lcdCommand(0x21); // INVON: this panel shows true colours inverted
  lcdCommand(0x13); // NORON
  lcdCommand(0x29); // DISPON
}

static void lcdWindow(int x, int y, int w, int h) {
  const uint8_t columns[] = {uint8_t(x >> 8), uint8_t(x),
                             uint8_t((x + w - 1) >> 8), uint8_t(x + w - 1)};
  const uint8_t rows[] = {uint8_t(y >> 8), uint8_t(y),
                          uint8_t((y + h - 1) >> 8), uint8_t(y + h - 1)};
  lcdCommand(0x2A, columns, sizeof(columns)); // CASET
  lcdCommand(0x2B, rows, sizeof(rows));       // RASET
}

// Panel pixel (px, py) shows upright pixel (ux, uy): the inverse of a
// clockwise turn `turn` of an upright sw x sh picture (the frame, or a page).
static inline void panelToUpright(int px, int py, int turn, int sw, int sh, int &ux, int &uy) {
  switch (turn) {
  case 90:  ux = py;          uy = sh - 1 - px; break;
  case 180: ux = sw - 1 - px; uy = sh - 1 - py; break;
  case 270: ux = sw - 1 - py; uy = px;          break;
  default:  ux = px;          uy = py;          break;
  }
}

static inline void panelToUpright(int px, int py, int turn, int &ux, int &uy) {
  panelToUpright(px, py, turn, FRAME_W, FRAME_H, ux, uy);
}

static inline uint16_t uprightAt(int px, int py, int turn) {
  int ux, uy;
  panelToUpright(px, py, turn, ux, uy);
  return uprightFrame[uy * FRAME_W + ux];
}

#if defined(WIGNET_LVGL_SPIKE) || defined(WIGNET_SCENES)
// While LVGL draws (lvgl_spike.cpp), the panel is its own: the loop keeps
// decoding pages into the upright frame, but nothing it draws is sent.
static volatile bool panelLent = false;
#endif

static void pushFrame() {
  if (!uprightFrame || !panelLock) return;
#if defined(WIGNET_LVGL_SPIKE) || defined(WIGNET_SCENES)
  if (panelLent) return;
#endif
  xSemaphoreTakeRecursive(panelLock, portMAX_DELAY);
  lcdWindow(0, 0, PANEL_W, PANEL_H);
  const int turn = (MOUNT_TURN + currentTurn) % 360;
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_CS, LOW);
  digitalWrite(LCD_DC, LOW);
  SPI.write(0x2C); // RAMWR
  digitalWrite(LCD_DC, HIGH);
  for (int y = 0; y < PANEL_H; y++) {
    uint8_t *out = rowBuffer;
    for (int x = 0; x < PANEL_W; x++) {
      const uint16_t stored = uprightAt(x, y, turn);
      const uint16_t pixel = static_cast<uint16_t>((stored << 8) | (stored >> 8));
      *out++ = calibratedRed[pixel >> 11];
      *out++ = calibratedGreen[(pixel >> 5) & 0x3F];
      *out++ = calibratedBlue[pixel & 0x1F];
    }
    SPI.writeBytes(rowBuffer, sizeof(rowBuffer));
  }
  digitalWrite(LCD_CS, HIGH);
  SPI.endTransaction();
  if (!backlightOn) {
    digitalWrite(LCD_BL, HIGH); // full brightness; dimming comes with the battery
    backlightOn = true;
  }
  xSemaphoreGiveRecursive(panelLock);
}

// ============ GxEPD2 driver: 1-bit pages into the upright frame ============

class GxEPD2_LCD_Wignet : public GxEPD2_EPD {
public:
  static const uint16_t WIDTH = FRAME_W;
  static const uint16_t WIDTH_VISIBLE = WIDTH;
  static const uint16_t HEIGHT = FRAME_H;
  static const GxEPD2::Panel panel = GxEPD2::GDEW0154T8; // Dummy ID
  static const bool hasColor = false;
  static const bool hasPartialUpdate = true;
  static const bool usePartialUpdateWindow = true;
  static const bool hasFastPartialUpdate = true;
  static const bool useFastFullUpdate = true;
  static const uint16_t power_on_time = 0;
  static const uint16_t power_off_time = 0;
  static const uint16_t full_refresh_time = 0;
  static const uint16_t partial_refresh_time = 0;

  // No pins: the LCD is driven above, GxEPD2 only hands over its pages.
  GxEPD2_LCD_Wignet()
      : GxEPD2_EPD(-1, -1, -1, -1, HIGH, 0, WIDTH, HEIGHT, panel, hasColor,
                   hasPartialUpdate, hasFastPartialUpdate) {}

  void clearScreen(uint8_t value) override {
    writeScreenBuffer(value);
    pushFrame();
  }
  void writeScreenBuffer(uint8_t value) override {
    if (!uprightFrame) return;
    const uint16_t pixel = value ? 0xFFFF : 0x0000;
    for (int i = 0; i < FRAME_PIXELS; i++) uprightFrame[i] = pixel;
  }
  void writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w,
                  int16_t h, bool invert = false, bool mirror_y = false,
                  bool pgm = false) override {
    _write(bitmap, x, y, w, h, invert, mirror_y);
  }
  void writeImageForFullRefresh(const uint8_t bitmap[], int16_t x, int16_t y,
                                int16_t w, int16_t h, bool invert = false,
                                bool mirror_y = false,
                                bool pgm = false) override {
    _write(bitmap, x, y, w, h, invert, mirror_y);
  }
  void writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part,
                      int16_t w_bitmap, int16_t h_bitmap, int16_t x, int16_t y,
                      int16_t w, int16_t h, bool invert = false,
                      bool mirror_y = false, bool pgm = false) override {
    _write(bitmap, x, y, w, h, invert, mirror_y);
  }
  void writeImageAgain(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w,
                       int16_t h, bool invert = false, bool mirror_y = false,
                       bool pgm = false) override {}
  void writeImagePartAgain(const uint8_t bitmap[], int16_t x_part,
                           int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                           int16_t x, int16_t y, int16_t w, int16_t h,
                           bool invert = false, bool mirror_y = false,
                           bool pgm = false) override {}
  void refresh(bool partial_update_mode = false) override { pushFrame(); }
  void refresh(int16_t x, int16_t y, int16_t w, int16_t h) override {
    pushFrame();
  }
  void powerOff() override {}
  void hibernate() override {}

  // Methods required by the GxEPD2_BW template
  void writeImage(const uint8_t *black, const uint8_t *color, int16_t x,
                  int16_t y, int16_t w, int16_t h, bool invert = false,
                  bool mirror_y = false, bool pgm = false) {
    if (black) writeImage(black, x, y, w, h, invert, mirror_y, pgm);
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
    if (data1) writeImage(data1, x, y, w, h, invert, mirror_y, pgm);
  }
  void drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w,
                 int16_t h, bool invert = false, bool mirror_y = false,
                 bool pgm = false) {
    writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
    pushFrame();
  }
  void drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part,
                     int16_t w_bitmap, int16_t h_bitmap, int16_t x, int16_t y,
                     int16_t w, int16_t h, bool invert = false,
                     bool mirror_y = false, bool pgm = false) {
    writeImagePart(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h,
                   invert, mirror_y, pgm);
    pushFrame();
  }
  void drawImage(const uint8_t *black, const uint8_t *color, int16_t x,
                 int16_t y, int16_t w, int16_t h, bool invert = false,
                 bool mirror_y = false, bool pgm = false) {
    if (black) drawImage(black, x, y, w, h, invert, mirror_y, pgm);
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
    if (data1) drawImage(data1, x, y, w, h, invert, mirror_y, pgm);
  }

private:
  // GxEPD2's 1-bit rows, MSB first, a set bit is white.
  void _write(const uint8_t *bitmap, int16_t x, int16_t y, int16_t w,
              int16_t h, bool invert, bool mirror_y) {
    if (!uprightFrame || !bitmap) return;
    const int stride = (w + 7) / 8;
    for (int row = 0; row < h; row++) {
      const int fy = y + (mirror_y ? h - 1 - row : row);
      if (fy < 0 || fy >= FRAME_H) continue;
      for (int col = 0; col < w; col++) {
        const int fx = x + col;
        if (fx < 0 || fx >= FRAME_W) continue;
        bool white = bitmap[row * stride + col / 8] & (0x80 >> (col % 8));
        if (invert) white = !white;
        uprightFrame[fy * FRAME_W + fx] = white ? 0xFFFF : 0x0000;
      }
    }
  }
};

// ============ HAL Implementation ============

static GxEPD2_BW<GxEPD2_LCD_Wignet, GxEPD2_LCD_Wignet::HEIGHT>
    display(GxEPD2_LCD_Wignet{});

namespace DisplayHAL {

void init() {
  Serial.println("[HAL] Initializing LCD...");
  uprightFrame = static_cast<uint16_t *>(
      heap_caps_malloc(FRAME_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
  if (!uprightFrame) {
    Serial.println("[HAL] No PSRAM for the frame; the screen stays dark");
    return;
  }
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, LOW);
  for (int i = 0; i < FRAME_PIXELS; i++) uprightFrame[i] = 0xFFFF;
  panelLock = xSemaphoreCreateRecursiveMutex();
  buildCalibration();
  // Started here with this board's pins; GxEPD2's own SPI.begin() below is
  // then a no-op and cannot take the default pins (11-13: audio, SD on the
  // 1.54"). MISO is not used: the 2"'s microSD keeps GPIO40 to itself.
  SPI.begin(LCD_SCLK, -1, LCD_MOSI, LCD_CS);
  lcdInit();
  display.init(0);
  // Always upright: the turn is applied on the way to the panel.
  display.setRotation(0);
  Serial.println("[HAL] LCD initialized.");
}

// Plugged in for now: nothing to put to sleep.
void hibernate() {}

GxEPD2_GFX *getDisplay() { return &display; }

int getWidth() { return FRAME_W; }

int getHeight() { return FRAME_H; }

uint16_t *frame() { return uprightFrame; }

void showFrame() { pushFrame(); }

void setBackground(uint16_t color) {
  if (!uprightFrame) return;
  const uint16_t pixel = static_cast<uint16_t>((color << 8) | (color >> 8));
  for (int i = 0; i < FRAME_PIXELS; i++) uprightFrame[i] = pixel;
}

void setTurn(int degrees) {
  // A frame turned a quarter would not fit a panel that is not square.
  if (FRAME_W != FRAME_H) return;
  degrees = ((degrees % 360) + 360) % 360;
  if (degrees % 90 || degrees == currentTurn || !panelLock) return;
  xSemaphoreTakeRecursive(panelLock, portMAX_DELAY);
  currentTurn = degrees;
  // Before the first screen there is nothing to turn yet: it will be sent
  // turned.
  if (backlightOn) pushFrame();
  xSemaphoreGiveRecursive(panelLock);
}

int turn() { return currentTurn; }

#if defined(WIGNET_LVGL_SPIKE) || defined(WIGNET_SCENES)
void lendPanel(bool lent) { panelLent = lent; }

// The same way to the panel as showFrame(): turned by MOUNT_TURN (and a
// square board's own turn), through the calibration, in 18 bits. Only the
// panel window that the upright rectangle lands on is written. A page laid
// out for how the board is held (pageTurn) is turned into the frame first.
void pushRegion(const uint16_t *pixels, int x, int y, int w, int h, int pageTurn) {
  if (!panelLock || w <= 0 || h <= 0) return;
  pageTurn = ((pageTurn % 360) + 360) % 360;
  const bool sidewaysPage = pageTurn == 90 || pageTurn == 270;
  const int sw = sidewaysPage ? FRAME_H : FRAME_W, sh = sidewaysPage ? FRAME_W : FRAME_H;
  const int turn = (MOUNT_TURN + currentTurn + pageTurn) % 360;
  // Where the rectangle's corners land on the panel (the forward turn).
  int px0, py0, px1, py1;
  switch (turn) {
  case 90:  px0 = sh - y - h; py0 = x;          break;
  case 180: px0 = sw - x - w; py0 = sh - y - h; break;
  case 270: px0 = y;          py0 = sw - x - w; break;
  default:  px0 = x;          py0 = y;          break;
  }
  const bool sideways = turn == 90 || turn == 270;
  px1 = px0 + (sideways ? h : w);
  py1 = py0 + (sideways ? w : h);
  xSemaphoreTakeRecursive(panelLock, portMAX_DELAY);
  lcdWindow(px0, py0, px1 - px0, py1 - py0);
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(LCD_CS, LOW);
  digitalWrite(LCD_DC, LOW);
  SPI.write(0x2C); // RAMWR
  digitalWrite(LCD_DC, HIGH);
  const size_t rowBytes = static_cast<size_t>(px1 - px0) * 3;
  for (int py = py0; py < py1; py++) {
    uint8_t *out = rowBuffer;
    for (int px = px0; px < px1; px++) {
      int ux, uy;
      panelToUpright(px, py, turn, sw, sh, ux, uy);
      const uint16_t pixel = pixels[(uy - y) * w + (ux - x)]; // native order
      *out++ = calibratedRed[pixel >> 11];
      *out++ = calibratedGreen[(pixel >> 5) & 0x3F];
      *out++ = calibratedBlue[pixel & 0x1F];
    }
    SPI.writeBytes(rowBuffer, rowBytes);
  }
  digitalWrite(LCD_CS, HIGH);
  SPI.endTransaction();
  if (!backlightOn) {
    digitalWrite(LCD_BL, HIGH);
    backlightOn = true;
  }
  xSemaphoreGiveRecursive(panelLock);
}
#endif

} // namespace DisplayHAL
