/**
 * LVGL 9.6 configuration for the LVGL spike (env waveshare_esp32s3_lcd_2_lvgl,
 * -DWIGNET_LVGL_SPIKE). Only what differs from LVGL's own defaults
 * (lv_conf_internal.h fills in the rest): no widgets beyond the image and
 * the animated image, no layouts, no extra themes, the C library's malloc.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

// malloc: on this board anything over 4 KB goes to PSRAM
// (CONFIG_SPIRAM_USE_MALLOC), so LVGL's heap is measured with heap_caps.
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

// One task calls lv_timer_handler (lvgl_spike.cpp): no locking in LVGL.
#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 33 // ~30 fps at most

// Only the formats the scene uses: RGB565 background, RGB565A8 sprites, and
// i4 sprites, which LVGL converts a line at a time to ARGB8888 while drawing.
#define LV_DRAW_SW_SUPPORT_RGB888 0
#define LV_DRAW_SW_SUPPORT_XRGB8888 0
#define LV_DRAW_SW_SUPPORT_ARGB8888 1
#define LV_DRAW_SW_SUPPORT_L8 0
#define LV_DRAW_SW_SUPPORT_AL88 0
// A8 as a render target: with ARGB8888 off, 9.6 fails to link
// (lv_draw_sw_blend_to_a8.c needs argb8888_image_blend). Not needed here.
#define LV_DRAW_SW_SUPPORT_A8 0
#define LV_DRAW_SW_SUPPORT_I1 0

#define LV_USE_LOG 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_USE_FLEX 0
#define LV_USE_GRID 0
#define LV_USE_OBSERVER 0

// Text stays server-rendered: one font, because the core needs a default.
#define LV_FONT_MONTSERRAT_14 1

#define LV_USE_IMAGE 1
#define LV_USE_ANIMIMG 1
#define LV_USE_LABEL 1 // the default theme references it
#define LV_USE_ARC 0
#define LV_USE_ARCLABEL 0
#define LV_USE_BAR 0
#define LV_USE_BUTTON 0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LINE 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_SWITCH 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif // LV_CONF_H
