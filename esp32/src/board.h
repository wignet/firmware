#pragma once
/**
 * What each board has, and on which pins: the code tests features
 * (WIGNET_HAS_AUDIO…), never board names (HARDWARE.md).
 *
 * WIGNET_HAS_AUDIO: an ES8311 speaker codec, so alarms, pushed sounds, MML
 * tunes and the list of sounds (audio_hal.cpp, alarm_clock_hal.cpp), with the
 * buttons that stop a ring.
 */

#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
#define WIGNET_HAS_AUDIO 1
// I2C: PMIC (AXP2101), RTC (PCF85063) and codec.
#define WIGNET_I2C_SDA 47
#define WIGNET_I2C_SCL 48
#define WIGNET_I2S_MCLK 14
#define WIGNET_I2S_BCLK 15
#define WIGNET_I2S_LRCK 16
#define WIGNET_I2S_DOUT 17
#define WIGNET_AUDIO_PA 7
// The codec's rail is the PMIC's ALDO3, switched on before each ring.
#define WIGNET_AUDIO_RAIL_AXP2101 1
#define WIGNET_BUTTON_KEY 4
// PWR goes through the PMIC: high when pressed, and it pulses while the
// amplifier plays (audio_hal.cpp, PWR_HOLD_MS).
#define WIGNET_PWR_ACTIVE_HIGH 1
// The RTC's alarm output: wakes the board from deep sleep (ext0).
#define WIGNET_RTC_INT 6

#elif defined(TARGET_WAVESHARE_ESP32S3_LCD_2)
// No audio and no buttons beyond BOOT. I2C: the QMI8658, on SDA 48 / SCL 47:
// the board says how it is held (orientation_report1, docs/lcd_2.md).
#define WIGNET_I2C_SDA 48
#define WIGNET_I2C_SCL 47
// The sensor's axes against the unturned page (orientation_hal.cpp): a first
// guess, to measure on the bench.
#define WIGNET_IMU_SCREEN_X_AXIS 0
#define WIGNET_IMU_SCREEN_X_SIGN -1
#define WIGNET_IMU_SCREEN_Y_AXIS 1
#define WIGNET_IMU_SCREEN_Y_SIGN 1
#define WIGNET_IMU_SCREEN_Z_SIGN -1 // lying screen up reads z = -1 g

#elif defined(TARGET_WAVESHARE_ESP32S3_AMOLED_18)
#define WIGNET_HAS_AUDIO 1
// I2C: touch (CST816, 0x15), ES8311 (0x18), the TCA9554 expander (0x20),
// AXP2101 (0x34), PCF85063 (0x51), QMI8658 (0x6B). Pins and I2S from
// Waveshare's examples/arduino-v2 (pin_config.h, 15_ES8311).
#define WIGNET_I2C_SDA 15
#define WIGNET_I2C_SCL 14
// The 2" board's axes: the page followed all four turns on the bench
// (2026-10-30), lying screen up reads z = -1 g.
#define WIGNET_IMU_SCREEN_X_AXIS 0
#define WIGNET_IMU_SCREEN_X_SIGN -1
#define WIGNET_IMU_SCREEN_Y_AXIS 1
#define WIGNET_IMU_SCREEN_Y_SIGN 1
#define WIGNET_IMU_SCREEN_Z_SIGN -1
#define WIGNET_I2S_MCLK 16
#define WIGNET_I2S_BCLK 9
#define WIGNET_I2S_LRCK 45
#define WIGNET_I2S_DOUT 8
#define WIGNET_AUDIO_PA 46
// BOOT only: no user key, and PWR reaches the ESP32 through the expander
// (EXIO4), not a GPIO. GPIO5 is a panel data line here.
#define WIGNET_BUTTON_KEY -1
#define WIGNET_BUTTON_PWR -1

#elif defined(TARGET_WAVESHARE_ESP32S3_LCD_154)
#define WIGNET_HAS_AUDIO 1
// I2C: codecs (ES8311 speaker, ES7210 microphones) and the QMI8658.
#define WIGNET_I2C_SDA 42
#define WIGNET_I2C_SCL 41
// The QMI8658 faces away from the screen: x is the screen's left (measured
// 2026-10-16, orientation_hal.cpp).
#define WIGNET_IMU_SCREEN_X_AXIS 0
#define WIGNET_IMU_SCREEN_X_SIGN -1
#define WIGNET_IMU_SCREEN_Y_AXIS 1
#define WIGNET_IMU_SCREEN_Y_SIGN 1
#define WIGNET_IMU_SCREEN_Z_SIGN -1 // lying screen up reads z = -1 g
#define WIGNET_I2S_MCLK 8
#define WIGNET_I2S_BCLK 9
#define WIGNET_I2S_LRCK 10
#define WIGNET_I2S_DOUT 12
#define WIGNET_AUDIO_PA 7
// PLUS, Waveshare's name for the user button.
#define WIGNET_BUTTON_KEY 4
// No RTC chip and no PMIC: the codec is always powered, PWR is a plain
// active-low button, and alarms wake the board by the ESP32's timer.
#endif

#if defined(WIGNET_HAS_AUDIO)
#define WIGNET_BUTTON_BOOT 0
#if !defined(WIGNET_BUTTON_PWR)
#define WIGNET_BUTTON_PWR 5
#endif
#endif
