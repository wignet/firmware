#pragma once
/**
 * Display Hardware Abstraction Layer
 *
 * This interface allows the Wignet client to work with different
 * ESP32 boards and e-paper display configurations.
 *
 * Each hardware target implements these functions in its own .cpp file.
 */

// Enable GxEPD2_GFX base class for polymorphism - MUST be before includes
#define ENABLE_GxEPD2_GFX 1

#include <GxEPD2_BW.h>

namespace DisplayHAL {

/**
 * Initialize SPI bus and e-paper display hardware.
 * Must be called once during setup().
 */
void init();

/**
 * Put the panel controller to sleep and turn off any board-specific display
 * power rail. Call before deep sleep.
 */
void hibernate();

/**
 * Get pointer to the display object for drawing operations.
 * The underlying type is hardware-specific but always extends GxEPD2_GFX.
 */
GxEPD2_GFX *getDisplay();

/**
 * Get display width in pixels.
 */
int getWidth();

/**
 * Get display height in pixels.
 */
int getHeight();

#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
/**
 * Render a complete, top-down 4-bpp Spectra image. The implementation may
 * transform the disposable payload buffer in place for the panel's physical
 * mounting orientation.
 *
 * Each byte holds two pixels (first/even pixel in the high nibble). Nibbles use
 * GxEPD2's generic 7-color values: 0 black, 1 white, 2 green, 3 blue, 4 red,
 * 5 yellow. The PhotoPainter driver translates those values to the panel's
 * controller-native encoding before refreshing.
 */
bool renderPackedColor(uint8_t *pixels, size_t length, int width,
                       int height);
#endif

#if defined(WIGNET_DISPLAY_RGB)
/**
 * A colour LCD keeps one upright frame, getWidth() x getHeight() RGB565
 * pixels, big-endian (as the panel takes them). Everything drawn lands in
 * it: a decoded PNG written straight in, and the 1-bit pages main.cpp draws
 * through getDisplay(), always at rotation 0. showFrame() sends it to the
 * panel turned by setTurn(), so turning never needs a redraw.
 */
uint16_t *frame();
void showFrame();

/**
 * The colour the frame starts in (RGB565, native order), so the panel never
 * flashes white before the first screen. The backlight comes on with the
 * first showFrame(), never on an empty panel.
 */
void setBackground(uint16_t color);

/**
 * Clockwise quarter turn (0, 90, 180, 270) applied to the upright frame on
 * its way to the panel; repaints when it changes. Square panels only.
 */
void setTurn(int degrees);
int turn();

#if defined(WIGNET_LVGL_SPIKE) || defined(WIGNET_SCENES)
/**
 * LVGL spike (lvgl_spike.cpp): while lent, showFrame() sends nothing and
 * pushRegion() owns the panel. pushRegion() takes an upright rectangle of
 * native-order RGB565 pixels (LVGL's own) and sends it the way showFrame()
 * would: turned, calibrated, 18 bits a pixel.
 *
 * `pageTurn`: a page laid out for how the board is held (a scene's manifest
 * `turn`, orientation_report1), turned clockwise that much into the frame. A
 * sideways one is getHeight() x getWidth().
 */
void lendPanel(bool lent);
void pushRegion(const uint16_t *pixels, int x, int y, int w, int h, int pageTurn = 0);
#endif
#endif

} // namespace DisplayHAL
