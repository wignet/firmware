#pragma once

/**
 * LVGL spike (-DWIGNET_LVGL_SPIKE, env waveshare_esp32s3_lcd_2_lvgl): a
 * sprite wandering over a background, animated on the board by LVGL 9.6,
 * described by a small JSON scene (docs/lvgl_spike.md). Not for production.
 */
namespace LvglSpike {

// Takes the panel from main.cpp's screens and starts LVGL in a task of its
// own, beside the /schedule loop. Call once, after DisplayHAL::init().
void start();

} // namespace LvglSpike
