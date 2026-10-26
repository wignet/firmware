#pragma once
#include <Arduino.h>

namespace Ota {
#if defined(WIGNET_OTA_BENCH)
void begin();
bool pending();
bool holding();
void command(const String &args);
bool writeImage(Stream &source, size_t size, const char *sha256);
void imageReceived();
void scheduleReceived(const String &payload);
void setNoticeHandler(void (*handler)());
#else
inline void begin() {}
inline bool pending() { return false; }
inline bool holding() { return false; }
inline void imageReceived() {}
inline void scheduleReceived(const String &) {}
#endif
} // namespace Ota
