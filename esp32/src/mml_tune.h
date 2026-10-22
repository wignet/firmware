#pragma once
#include <stddef.h>
#include <stdint.h>

// MML v1 alarm tunes (docs/mml.md): parser and four-channel Game Boy-style
// synth. Plain C++ with no Arduino dependency, so tests/test_mml.py can build
// it on the host and check it against the platform's parser
// (backoffice/mml.py). The platform only sends tunes it has validated and
// normalized; the frame validates again and falls back to the WAV or the
// generated tone if a tune is refused.
namespace Mml {

constexpr uint32_t TICKS_PER_QUARTER = 96;
constexpr uint8_t CHANNEL_COUNT = 4; // A, B pulse; C wave; D noise

struct Note {
  uint32_t tick;
  uint32_t length;
  uint32_t gate; // ticks the note sounds; 0 for a rest
  int16_t note;  // MIDI number, -1 for a rest
  uint8_t volume;
  uint8_t timbre;
  uint8_t envelope;
};

// Walks one channel of a normalized tune, expanding loops and ties.
// Mirrors _Cursor in backoffice/mml.py; keep the two in step.
class ChannelCursor {
public:
  void begin(const char *text, size_t length, uint8_t channel);
  // The next note or rest. False at the channel's end, or on an error, when
  // error() is non-null.
  bool next(Note &note);
  const char *error() const { return error_; }
  uint32_t tick() const { return tick_; }
  uint16_t tempo() const { return tempo_; } // 0 when the channel sets none

private:
  bool fail(const char *message);
  bool nextLine();
  void skipSpaces();
  bool number(uint32_t &value);
  bool length(uint32_t &ticks);
  bool pitch(int16_t &note);

  struct Loop {
    size_t start;
    int8_t remaining; // -1 until the loop's "]" is first reached
  };

  const char *text_ = nullptr;
  size_t length_ = 0;
  char letter_ = 'A';
  uint8_t channel_ = 0;
  size_t nextLineStart_ = 0;
  size_t pos_ = 0;
  size_t lineEnd_ = 0;
  bool inLine_ = false;
  Loop loops_[4] = {};
  uint8_t depth_ = 0;
  uint8_t octave_ = 4;
  uint32_t defaultLength_ = TICKS_PER_QUARTER;
  uint8_t volume_ = 12;
  uint8_t timbre_ = 0;
  uint8_t envelope_ = 0;
  uint8_t gate_ = 7;
  uint16_t tempo_ = 0;
  uint32_t tick_ = 0;
  uint32_t steps_ = 0;
  const char *error_ = nullptr;
};

struct Info {
  uint16_t tempo;
  uint32_t lengthTicks;
  uint8_t channels; // bit 0 = A ... bit 3 = D
};

// Accepts exactly the normalized tunes backoffice/mml.py accepts. On
// failure, *error names the reason.
bool validate(const char *text, size_t length, Info &info, const char **error);

// Renders a validated tune as mono 16-bit samples, looping it forever. The
// caller scales the output (the alarm fade-in) and decides when to stop.
class Synth {
public:
  bool begin(const char *text, size_t length, uint32_t sampleRate,
             const char **error);
  void render(int16_t *out, size_t samples);
  uint64_t loopSamples() const { return loopSamples_; }

private:
  struct Voice {
    ChannelCursor cursor;
    Note note;
    bool done;
    uint64_t gateEnd;
    uint64_t end;
    uint32_t phase;
    uint32_t increment;
    uint16_t lfsr;
    uint8_t level;
    uint32_t envelopeCounter;
  };

  void restart();
  void load(Voice &voice, uint8_t channel);
  uint64_t sampleAt(uint32_t tick) const;
  int32_t voiceSample(Voice &voice, uint8_t channel);

  const char *text_ = nullptr;
  size_t length_ = 0;
  uint32_t sampleRate_ = 48000;
  Info info_ = {};
  Voice voices_[CHANNEL_COUNT] = {};
  uint64_t sample_ = 0;
  uint64_t loopSamples_ = 0;
};

} // namespace Mml
