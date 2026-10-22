#pragma once

#include <Arduino.h>
#include "board.h"

// Bench console over USB serial (the PhotoPainter's native port): play a tune,
// run an alarm, draw an image, with no platform or Wi-Fi involved.
// esp32/play_mml.py, test_alarm.py and show_image.py drive it; a
// person can too, in a serial monitor.
//
//   mml          then the tune's lines, then a line "end"
//   play [volume 0-100, default 60] [seconds 1-3600, default 30]
//   stop         (any input also stops a tune that is playing)
//   alarm in <seconds> [volume 60] [fade-in 10] [duration 30] [sound]
//                a one-off alarm through the real alarm path, with the last
//                tune (or the WAV/tone with "sound"); the platform's alarm
//                stays as it is
//   alarm        clock, RTC and both alarms;  alarm clear
//   sleep [rtc]  deep sleep now, even on USB, until the next alarm; "rtc"
//                sets the timer backup two minutes late so the RTC
//                interrupt must wake the frame
//   image <bytes> then the BMP or WGN6 file, sent in 4 KB chunks, each
//                acknowledged ("[bench] +"); drawn by the decoder that draws
//                downloads (esp32/show_image.py)
//   resume       let the platform refresh the panel again after "image"
//   ringdraw [draws 2] [volume 60]
//                plays the last tune (the tone without one) in the audio
//                task and redraws the panel `draws` times meanwhile, then
//                reports the worst gap between audio buffers and the
//                dropouts: the "screen refresh while music plays" test
//   i2c [reset]  bus levels and scan; reset = 9-clock recovery
//   help
//
// Every reply starts with "[bench] ", so it stands out from the firmware log.
// The first line of "help" names the image formats the frame draws
// ("commands (images: bmp wgn6):"), so show_image.py picks the profile.
//
// The other ESP32 frames have "image", "resume" and "help" only, over their
// USB-serial bridge (a CH343 on the Waveshare driver board): 1-bit BMP, the
// same protocol. ESP8266 targets have no-op implementations.
#if defined(WIGNET_HAS_AUDIO)
#define WIGNET_BENCH_CONSOLE 1
#define WIGNET_BENCH_AUDIO 1 // mml, play, alarm, sleep, i2c
#elif defined(ARDUINO_ARCH_ESP32)
#define WIGNET_BENCH_CONSOLE 1
#endif

namespace UsbConsole {

// Before Serial.begin(): enlarges the receive buffer so a whole tune or
// image chunk fits while the loop is busy (an e-paper refresh takes tens of
// seconds).
void prepareSerial();

// From loop(): reads what arrived and runs complete commands. Blocks while
// a tune plays.
void poll();

// main.cpp's way into deep sleep, for "sleep": the console cannot reach it.
void setSleepHandler(void (*handler)(bool rtcOnly));

// main.cpp's image decoder, for "image": draws a BMP or WGN6 read from
// `source` (exactly `length` bytes) and says whether it was shown. A shown
// image pauses the platform's refresh for BENCH_HOLD_MINUTES, so it stays up
// to be looked at; "resume" (the resume handler) lifts the pause.
constexpr unsigned long BENCH_HOLD_MINUTES = 30;
void setImageHandler(bool (*handler)(Stream &source, size_t length));
void setResumeHandler(void (*handler)());

// main.cpp's panel, for "ringdraw": draw test screen `index` of `count`.
void setDrawHandler(void (*handler)(int index, int count));

} // namespace UsbConsole
