#pragma once

// The upcoming sounds a frame holds (/schedule `sounds`, firmware reporting
// sound_list1; DEVICE_API.md). Plain C++ with no Arduino types, so
// tests/test_sound_list_firmware.py builds and checks it on the host; parsing the
// JSON and storing the list stay in alarm_clock_hal.cpp.

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace SoundList {

struct Entry {
  std::string occurrenceId;
  std::string eventId;
  std::string sound;  // the WAV's name, rung when there is no tune
  std::string tuneId; // key into /schedule `tunes`; empty rings `sound`
  int64_t epoch = 0;  // UTC
  bool pushed = false;
  uint8_t volume = 70;
  uint32_t fadeInSec = 600;
  uint32_t maxDurationSec = 600;
};

// A sound found later than this after its time is skipped, not rung late
// (the same rule as the single `alarm`).
constexpr int64_t GRACE_SECONDS = 120;
// The server sends at most 16; pushed sounds carried over can add a few.
constexpr size_t MAX_ENTRIES = 20;
// Occurrences already rung, kept so that a reset or a repeated list never
// rings one twice. More than a list holds, so none drops out while listed.
constexpr size_t DONE_KEPT = 32;

bool isDone(const std::vector<std::string> &done, const std::string &occurrenceId);
// Records `occurrenceId` as rung, keeping the newest DONE_KEPT.
void markDone(std::vector<std::string> &done, const std::string &occurrenceId);

// The list to hold after a /schedule answer. The server is the authority on
// planned sounds, so `incoming` replaces them. A pushed sound is sent once
// (the server hands it over at one poll), so one held but not rung yet is
// kept while it can still ring, unless `incoming` lists it again.
std::vector<Entry> merge(const std::vector<Entry> &held, const std::vector<Entry> &incoming,
                         const std::vector<std::string> &done, int64_t now);

// Index of the sound to ring now: due, within its grace, not rung yet, the
// earliest first. -1 when none.
int dueIndex(const std::vector<Entry> &entries, const std::vector<std::string> &done, int64_t now);

// Time of the next sound still to ring after `now`, for the RTC alarm; 0
// when none.
int64_t nextEpoch(const std::vector<Entry> &entries, const std::vector<std::string> &done, int64_t now);

} // namespace SoundList
