#pragma once
#include <stdint.h>

namespace AudioHAL {

// Ring until BOOT/KEY/PWR is pressed or the safety timeout expires. A
// non-empty MML v1 `tune` (docs/mml.md) loops if it validates; otherwise the
// bundled WAV at `path` loops; if that is absent too, a generated tone still
// provides an audible alarm. Returns true if audio hardware started. The WAV
// is never removed from flash.
// Loudness rises from silence to `volume` over `fadeInSec`, then holds for
// `maxDurationSec`: the duration counts from full volume.
bool playAlarm(const char *path, const char *tune, uint8_t volume,
               uint32_t maxDurationSec, uint32_t fadeInSec);

// USB bench test (usb_console.cpp): play `tune` at `volume` from the first
// sample, no fade-in, for `seconds` (1-3600). Stops on a button or as soon
// as `shouldStop` returns true. Blocks until it ends.
bool playTest(const char *tune, uint8_t volume, uint32_t seconds, bool (*shouldStop)());

// The same ring, in its own FreeRTOS task: returns as soon as playback has
// started (or failed to), so the caller can join Wi-Fi, fetch and draw while
// the sound plays. The task is pinned to the app core above the loop's
// priority; i2s_write blocks it between buffers, which leaves the loop free.
// The strings are copied. False if a sound is already playing.
bool startAlarm(const char *path, const char *tune, uint8_t volume,
                uint32_t maxDurationSec, uint32_t fadeInSec);
// Bench variant: no fade-in; `path` "" and `tune` "" give the generated tone.
bool startTest(const char *path, const char *tune, uint8_t volume, uint32_t seconds);

bool isPlaying();

// A scene's sound (docs/scenes.md "Sounds"): the MML `tune` played once from
// the first sample at `volume`, at most `maxMs`, in the same task as a ring.
// Buttons do not stop a cue (they stay the scene's); stop() does. False if a
// sound is already playing.
bool startCue(const char *tune, uint8_t volume, uint32_t maxMs);
// True while an alarm or a pushed sound rings, not a cue: the press is the ring's.
bool isRinging();

// A scene's tone (docs/scenes.md "Instrument"): a continuous wave from the
// first sample at `volume`, gliding to each new frequency over about
// `glideMs`, until stop(). Buttons do not stop it, as a cue. False if a sound
// is already playing.
enum class Wave : uint8_t { Sine, Triangle, Square, Saw };
bool startTone(Wave wave, float hz, uint8_t volume, uint32_t glideMs);
// The frequency the tone glides to; any task may call it.
void setToneHz(float hz);

// Ask a playing sound to stop, as a button would; returns at once.
void stop();

// What the last ring measured. A gap between two buffer writes longer than
// the I2S DMA holds (about 85 ms) means the DMA ran dry: a dropout.
struct Stats {
  uint32_t writes = 0;
  uint32_t maxGapUs = 0;
  uint32_t dropouts = 0;
  bool played = false;
};
Stats lastStats();

// Bench diagnostic: log every change of BOOT, KEY and PWR for `seconds`,
// sampled every 5 ms in a background task, with how long each level lasted.
void watchButtons(uint32_t seconds);

} // namespace AudioHAL
