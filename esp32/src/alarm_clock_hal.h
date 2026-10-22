#pragma once

#include <Arduino.h>

namespace AlarmClockHAL {

// PhotoPainter-only entry points; other targets have no-op implementations.
void begin();
void syncFromNtp();
void updateFromSchedule(const String &json);
void ringIfDue();
void prepareSleep(int &seconds);

// USB bench (usb_console.cpp): a one-off alarm kept apart from the
// platform's snapshot, so a /schedule poll neither replaces it nor loses the
// real alarm. It rings through the same path. It lives in RTC memory: it
// survives deep sleep, not a power cycle.
bool setTestAlarm(int64_t epoch, const char *tune, uint8_t volume,
                  uint32_t fadeInSec, uint32_t maxDurationSec);
void clearTestAlarm();
// Seconds added to the deep-sleep timer when an alarm caps the sleep, so
// that the RTC interrupt, not the timer backup, has to wake the frame.
void setTimerBackupSlack(int seconds);
// Clock, RTC and both alarms, as lines starting with `prefix`.
void printStatus(const char *prefix);

} // namespace AlarmClockHAL
