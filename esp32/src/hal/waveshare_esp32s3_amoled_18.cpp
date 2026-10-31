/**
 * Display HAL Implementation: Waveshare ESP32-S3-Touch-AMOLED-1.8 (CO5300)
 *
 * Board: ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM, a 1.8" 368x448 AMOLED
 * (CO5300) on a 4-line QSPI bus, natively portrait. Like the 2" LCD, its frame
 * is the landscape buffer, 448x368, turned once onto the panel.
 *
 * Pins from waveshareteam/ESP32-S3-Touch-AMOLED-1.8, examples/arduino-v2
 * (pin_config.h, 13_LVGL_Widgets). The panel's reset and the touch's sit
 * behind a TCA9554 I/O expander on I2C (0x20), put back to its power-on
 * state before the panel is started. That revision answers on I2C
 * at 0x15 (CST816), 0x18 (ES8311), 0x20 (TCA9554), 0x34 (AXP2101),
 * 0x51 (PCF85063), 0x6B (QMI8658) and 0x7E (not identified), measured
 * 2026-10-30 on a board with MAC 90:70:69:fe:a4:08.
 *
 * Besides the screen: scenes (lendPanel, pushRegion) and the CST816 touch
 * (touchAt, touch1). Audio, the QMI8658 and the AXP2101 (power_hal.cpp) are
 * board.h's. main.cpp draws its pages through GxEPD2 as on the other colour
 * boards; the adapter below hands them to the upright frame.
 */

#define ENABLE_GxEPD2_GFX 1
#include "display_hal.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_EPD.h>
#include <Wire.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// ============ Board ============
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 11
#define LCD_CS 12
#define BOARD_I2C_SDA 15
#define BOARD_I2C_SCL 14
#define EXPANDER_ADDRESS 0x20
#define TOUCH_ADDRESS 0x15 // CST816
static constexpr int PANEL_W = 368, PANEL_H = 448;
static constexpr int FRAME_W = 448, FRAME_H = 368;
// Clockwise quarter turn from the frame to the panel, by how the panel sits.
static constexpr int MOUNT_TURN = 90;
static constexpr int FRAME_PIXELS = FRAME_W * FRAME_H;

// The upright frame, big-endian RGB565 (PSRAM), and the same picture turned
// for the panel, in native order.
static uint16_t *uprightFrame = nullptr;
static uint16_t *panelBuffer = nullptr;
static int currentTurn = 0;
static SemaphoreHandle_t panelLock = nullptr;
// Off until the first frame has reached the panel: what lit up first is a
// real screen, never the controller's leftover RAM.
static bool brightnessOn = false;

static Arduino_DataBus *bus = nullptr;
static Arduino_CO5300 *panel = nullptr;

// ============ Expander ============

static void expanderWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(EXPANDER_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

// TCA9554: 0x01 output port, 0x03 configuration (1 = input). Back to its
// power-on state, every pin an input pulled high: the resets released. The
// expander keeps its registers across a reset of the ESP32 alone, and
// Waveshare's low-then-high pulse on pins 0, 1, 2 and 6 left this panel dark
// (2026-10-30).
static void releaseResets() {
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
  expanderWrite(0x01, 0xFF);
  expanderWrite(0x03, 0xFF);
  delay(100);
}

// ============ Frame to panel ============

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

#if defined(WIGNET_SCENES)
// While a scene plays (scene_player.cpp), the panel is LVGL's: the loop keeps
// decoding pages into the upright frame, but nothing it draws is sent.
static volatile bool panelLent = false;
#endif

static void pushFrame() {
  if (!uprightFrame || !panelBuffer || !panelLock || !panel) return;
#if defined(WIGNET_SCENES)
  if (panelLent) return;
#endif
  xSemaphoreTakeRecursive(panelLock, portMAX_DELAY);
  const int turn = (MOUNT_TURN + currentTurn) % 360;
  for (int py = 0; py < PANEL_H; py++) {
    uint16_t *out = panelBuffer + py * PANEL_W;
    for (int px = 0; px < PANEL_W; px++) {
      int ux, uy;
      panelToUpright(px, py, turn, ux, uy);
      const uint16_t stored = uprightFrame[uy * FRAME_W + ux];
      *out++ = static_cast<uint16_t>((stored << 8) | (stored >> 8));
    }
  }
  // writePixels() copies each chunk into the bus's internal DMA buffer.
  // draw16bitBeRGBBitmap() would hand this PSRAM buffer to the DMA as it is,
  // and the S3's DMA reads PSRAM past the cache: a black page with scattered
  // dots of stale colour (2026-10-30).
  panel->startWrite();
  panel->writeAddrWindow(0, 0, PANEL_W, PANEL_H);
  bus->writePixels(panelBuffer, PANEL_W * PANEL_H);
  panel->endWrite();
  if (!brightnessOn) {
    panel->setBrightness(255);
    brightnessOn = true;
  }
  xSemaphoreGiveRecursive(panelLock);
}

// ============ GxEPD2 driver: 1-bit pages into the upright frame ============

class GxEPD2_AMOLED_Wignet : public GxEPD2_EPD {
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

  // No pins: the panel is driven above, GxEPD2 only hands over its pages.
  GxEPD2_AMOLED_Wignet()
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

static GxEPD2_BW<GxEPD2_AMOLED_Wignet, GxEPD2_AMOLED_Wignet::HEIGHT>
    display(GxEPD2_AMOLED_Wignet{});

namespace DisplayHAL {

void init() {
  Serial.println("[HAL] Initializing AMOLED...");
  uprightFrame = static_cast<uint16_t *>(
      heap_caps_malloc(FRAME_PIXELS * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
  panelBuffer = static_cast<uint16_t *>(
      heap_caps_malloc(PANEL_W * PANEL_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
  if (!uprightFrame || !panelBuffer) {
    Serial.println("[HAL] No PSRAM for the frames; the screen stays dark");
    return;
  }
  for (int i = 0; i < FRAME_PIXELS; i++) uprightFrame[i] = 0xFFFF;
  panelLock = xSemaphoreCreateRecursiveMutex();
  // No display.init(): GxEPD2's init() calls SPI.begin() on the S3's default
  // pins, SCK 12 and MOSI 11, this panel's CS and QSPI clock. The driver
  // above needs no bus, and GxEPD2_BW's constructor already sets what init()
  // would besides.
  // Always upright: the turn is applied on the way to the panel.
  display.setRotation(0);
  releaseResets();
  bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1,
                              LCD_SDIO2, LCD_SDIO3);
  // Rotation 0, not IPS; the panel's visible area starts 16 columns in. GFX
  // 1.4.9 takes `ips` before the size, unlike Waveshare's bundled copy: left
  // out, the size shifts by one and only a 16-row band is drawn.
  panel = new Arduino_CO5300(bus, GFX_NOT_DEFINED, 0, false, PANEL_W, PANEL_H,
                             16, 0, 0, 0);
  if (!panel->begin()) {
    Serial.println("[HAL] AMOLED begin() failed");
    return;
  }
  panel->setBrightness(0); // lit by the first frame
  Serial.println("[HAL] AMOLED initialized.");
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
  (void)degrees;
}

int turn() { return currentTurn; }

#if defined(WIGNET_SCENES)
void lendPanel(bool lent) { panelLent = lent; }

// The same way to the panel as showFrame(): turned by MOUNT_TURN, through
// writePixels(). Only the panel window that the upright rectangle lands on
// is written. A page laid out for how the board is held (pageTurn) is
// turned into the frame first.
void pushRegion(const uint16_t *pixels, int x, int y, int w, int h, int pageTurn) {
  if (!panelLock || !panel || !panelBuffer || w <= 0 || h <= 0) return;
  pageTurn = ((pageTurn % 360) + 360) % 360;
  const bool sidewaysPage = pageTurn == 90 || pageTurn == 270;
  const int sw = sidewaysPage ? FRAME_H : FRAME_W, sh = sidewaysPage ? FRAME_W : FRAME_H;
  const int turn = (MOUNT_TURN + currentTurn + pageTurn) % 360;
  // Where the rectangle's corners land on the panel (the forward turn).
  int px0, py0;
  switch (turn) {
  case 90:  px0 = sh - y - h; py0 = x;          break;
  case 180: px0 = sw - x - w; py0 = sh - y - h; break;
  case 270: px0 = y;          py0 = sw - x - w; break;
  default:  px0 = x;          py0 = y;          break;
  }
  const bool sideways = turn == 90 || turn == 270;
  const int pw = sideways ? h : w, ph = sideways ? w : h;
  xSemaphoreTakeRecursive(panelLock, portMAX_DELAY);
  uint16_t *out = panelBuffer;
  for (int py = py0; py < py0 + ph; py++) {
    for (int px = px0; px < px0 + pw; px++) {
      int ux, uy;
      panelToUpright(px, py, turn, sw, sh, ux, uy);
      *out++ = pixels[(uy - y) * w + (ux - x)]; // native order, as LVGL's
    }
  }
  panel->startWrite();
  panel->writeAddrWindow(px0, py0, pw, ph);
  bus->writePixels(panelBuffer, pw * ph);
  panel->endWrite();
  if (!brightnessOn) {
    panel->setBrightness(255);
    brightnessOn = true;
  }
  xSemaphoreGiveRecursive(panelLock);
}
#endif

#if defined(WIGNET_HAS_TOUCH)
// CST816: from register 0x02, the number of fingers, then x and y on 12 bits
// each (high nibble first), in the panel's own portrait coordinates. Asleep
// between touches it does not answer: no finger.
bool touchAt(int &x, int &y, int pageTurn) {
  Wire.beginTransmission(TOUCH_ADDRESS);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t data[5];
  if (Wire.requestFrom(TOUCH_ADDRESS, 5) != 5) return false;
  for (uint8_t &byte : data) byte = Wire.read();
  if ((data[0] & 0x0F) == 0) return false;
  const int px = ((data[1] & 0x0F) << 8) | data[2];
  const int py = ((data[3] & 0x0F) << 8) | data[4];
  if (px >= PANEL_W || py >= PANEL_H) {
    Serial.printf("[Touch] Off the panel: %d,%d\n", px, py);
    return false;
  }
  // Back from the panel to the page, the way pushRegion() goes forward.
  pageTurn = ((pageTurn % 360) + 360) % 360;
  const bool sidewaysPage = pageTurn == 90 || pageTurn == 270;
  const int sw = sidewaysPage ? FRAME_H : FRAME_W, sh = sidewaysPage ? FRAME_W : FRAME_H;
  panelToUpright(px, py, (MOUNT_TURN + currentTurn + pageTurn) % 360, sw, sh, x, y);
  return true;
}
#endif

} // namespace DisplayHAL
