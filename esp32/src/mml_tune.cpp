#include "mml_tune.h"

#include <math.h>
#include <string.h>

namespace Mml {
namespace {

// Limits shared with backoffice/mml.py.
constexpr size_t MAX_BYTES = 2048;
constexpr uint32_t MAX_SECONDS = 180;
constexpr uint32_t TICKS_PER_WHOLE = TICKS_PER_QUARTER * 4;
constexpr uint32_t MAX_TICKS = MAX_SECONDS * 240 * TICKS_PER_QUARTER / 60;
constexpr uint32_t MAX_STEPS = 50000;
constexpr uint16_t MIN_TEMPO = 40;
constexpr uint16_t MAX_TEMPO = 240;
constexpr uint16_t DEFAULT_TEMPO = 120;
constexpr int16_t MIN_NOTE = 24;  // c in octave 1
constexpr int16_t MAX_NOTE = 107; // b in octave 7
constexpr uint8_t MAX_LOOP_DEPTH = 4;
constexpr uint8_t MAX_TIMBRE[CHANNEL_COUNT] = {3, 3, 1, 1};
constexpr uint8_t DEFAULT_TIMBRE[CHANNEL_COUNT] = {2, 2, 0, 0};
constexpr char HEADER[] = "#MML1\n";
constexpr size_t HEADER_LENGTH = sizeof(HEADER) - 1;

// Peak-to-peak amplitude of one channel at volume 15. Four channels at full
// volume can exceed int16; the mix is clamped.
constexpr int32_t CHANNEL_SPAN = 16000;

bool isNoteLetter(char c) { return c >= 'a' && c <= 'g'; }

bool validLength(uint32_t n) {
  static const uint8_t lengths[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
  for (uint8_t allowed : lengths) {
    if (n == allowed) return true;
  }
  return false;
}

bool bodyCharacter(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         strchr(" #+-.&<>[]@|", c) != nullptr;
}

} // namespace

void ChannelCursor::begin(const char *text, size_t length, uint8_t channel) {
  *this = ChannelCursor();
  text_ = text;
  length_ = length;
  channel_ = channel;
  letter_ = static_cast<char>('A' + channel);
  timbre_ = DEFAULT_TIMBRE[channel];
}

bool ChannelCursor::fail(const char *message) {
  error_ = message;
  return false;
}

bool ChannelCursor::nextLine() {
  while (nextLineStart_ < length_) {
    const size_t start = nextLineStart_;
    size_t end = start;
    while (end < length_ && text_[end] != '\n') ++end;
    nextLineStart_ = end + 1;
    if (end - start >= 2 && text_[start] == letter_ && text_[start + 1] == ' ') {
      pos_ = start + 2;
      lineEnd_ = end;
      depth_ = 0;
      return true;
    }
  }
  return false;
}

void ChannelCursor::skipSpaces() {
  while (pos_ < lineEnd_ && (text_[pos_] == ' ' || text_[pos_] == '|')) ++pos_;
}

bool ChannelCursor::number(uint32_t &value) {
  const size_t start = pos_;
  value = 0;
  while (pos_ < lineEnd_ && text_[pos_] >= '0' && text_[pos_] <= '9') {
    // Saturate: any value this large is out of every command's range.
    if (value < 1000000) value = value * 10 + (text_[pos_] - '0');
    ++pos_;
  }
  return pos_ > start;
}

bool ChannelCursor::length(uint32_t &ticks) {
  uint32_t n = 0;
  uint32_t base = defaultLength_;
  if (number(n)) {
    if (!validLength(n)) return fail("length must be 1,2,3,4,6,8,12,16,24,32,48 or 64");
    base = TICKS_PER_WHOLE / n;
  }
  uint32_t total = base;
  uint32_t add = base;
  while (pos_ < lineEnd_ && text_[pos_] == '.') {
    if (add % 2) return fail("dotted length too short");
    add /= 2;
    total += add;
    ++pos_;
  }
  ticks = total;
  return true;
}

bool ChannelCursor::pitch(int16_t &note) {
  static const int8_t semitones[] = {9, 11, 0, 2, 4, 5, 7}; // a..g
  int16_t semitone = semitones[text_[pos_] - 'a'];
  ++pos_;
  if (pos_ < lineEnd_ && (text_[pos_] == '+' || text_[pos_] == '#')) {
    ++semitone;
    ++pos_;
  } else if (pos_ < lineEnd_ && text_[pos_] == '-') {
    --semitone;
    ++pos_;
  }
  note = static_cast<int16_t>(12 * (octave_ + 1) + semitone);
  if (note < MIN_NOTE || note > MAX_NOTE) return fail("note out of range");
  return true;
}

bool ChannelCursor::next(Note &out) {
  if (error_) return false;
  for (;;) {
    if (!inLine_) {
      if (!nextLine()) return false;
      inLine_ = true;
    }
    skipSpaces();
    if (pos_ >= lineEnd_) {
      if (depth_) return fail("[ is never closed on this line");
      inLine_ = false;
      continue;
    }
    if (++steps_ > MAX_STEPS) return fail("too many commands");
    const char c = text_[pos_];

    if (isNoteLetter(c) || c == 'r') {
      int16_t note = -1;
      if (c == 'r') {
        ++pos_;
      } else if (!pitch(note)) {
        return false;
      }
      uint32_t ticks = 0;
      if (!length(ticks)) return false;
      while (note >= 0) {
        const size_t beforeTie = pos_;
        skipSpaces();
        if (pos_ >= lineEnd_ || text_[pos_] != '&') {
          pos_ = beforeTie;
          break;
        }
        ++pos_;
        skipSpaces();
        if (pos_ >= lineEnd_ || !isNoteLetter(text_[pos_])) {
          return fail("& must be followed by a note");
        }
        int16_t tied = -1;
        uint32_t extra = 0;
        if (!pitch(tied)) return false;
        if (tied != note) return fail("& must join the same pitch");
        if (!length(extra)) return false;
        ticks += extra;
      }
      out.tick = tick_;
      out.length = ticks;
      out.gate = note >= 0 ? ticks * gate_ / 8 : 0;
      out.note = note;
      out.volume = volume_;
      out.timbre = timbre_;
      out.envelope = envelope_;
      tick_ += ticks;
      if (tick_ > MAX_TICKS) return fail("tune too long");
      return true;
    }

    if (c == 'l') {
      ++pos_;
      const size_t lengthStart = pos_;
      uint32_t ignored = 0;
      if (!number(ignored)) return fail("l needs a length");
      pos_ = lengthStart;
      uint32_t ticks = 0;
      if (!length(ticks)) return false;
      defaultLength_ = ticks;
      continue;
    }

    if (c != '\0' && strchr("otvsq@", c) != nullptr) {
      ++pos_;
      uint32_t value = 0;
      if (!number(value)) return fail("command needs a number");
      switch (c) {
      case 'o':
        if (value < 1 || value > 7) return fail("octave out of range");
        octave_ = static_cast<uint8_t>(value);
        break;
      case 't':
        if (value < MIN_TEMPO || value > MAX_TEMPO) return fail("tempo out of range");
        if (tick_) return fail("tempo after the first note");
        tempo_ = static_cast<uint16_t>(value);
        break;
      case 'v':
        if (value > 15) return fail("volume out of range");
        volume_ = static_cast<uint8_t>(value);
        break;
      case 's':
        if (value > 7) return fail("envelope out of range");
        envelope_ = static_cast<uint8_t>(value);
        break;
      case 'q':
        if (value < 1 || value > 8) return fail("gate out of range");
        gate_ = static_cast<uint8_t>(value);
        break;
      default: // '@'
        if (value > MAX_TIMBRE[channel_]) return fail("timbre out of range");
        timbre_ = static_cast<uint8_t>(value);
        break;
      }
      continue;
    }

    if (c == '<' || c == '>') {
      const int octave = octave_ + (c == '>' ? 1 : -1);
      if (octave < 1 || octave > 7) return fail("octave out of range");
      octave_ = static_cast<uint8_t>(octave);
      ++pos_;
      continue;
    }

    if (c == '[') {
      if (depth_ >= MAX_LOOP_DEPTH) return fail("loops nested too deep");
      ++pos_;
      loops_[depth_++] = {pos_, -1};
      continue;
    }

    if (c == ']') {
      if (!depth_) return fail("] without [");
      ++pos_;
      uint32_t count = 2;
      if (!number(count)) count = 2;
      if (count < 2 || count > 16) return fail("loop count out of range");
      Loop &loop = loops_[depth_ - 1];
      if (loop.remaining < 0) loop.remaining = static_cast<int8_t>(count - 1);
      if (loop.remaining > 0) {
        --loop.remaining;
        pos_ = loop.start;
      } else {
        --depth_;
      }
      continue;
    }

    return fail(c == '&' ? "& must follow a note" : "unknown command");
  }
}

bool validate(const char *text, size_t length, Info &info, const char **error) {
  const char *ignored = nullptr;
  const char **reason = error ? error : &ignored;
  info = Info();
  if (length > MAX_BYTES) {
    *reason = "tune too large";
    return false;
  }
  if (length < HEADER_LENGTH || memcmp(text, HEADER, HEADER_LENGTH) != 0) {
    *reason = "missing #MML1 header";
    return false;
  }

  size_t start = HEADER_LENGTH;
  while (start < length) {
    size_t end = start;
    while (end < length && text[end] != '\n') ++end;
    if (end - start < 3 || text[start] < 'A' || text[start] > 'D' ||
        text[start + 1] != ' ') {
      *reason = "malformed line";
      return false;
    }
    for (size_t i = start + 2; i < end; ++i) {
      if (!bodyCharacter(text[i])) {
        *reason = "character not allowed";
        return false;
      }
    }
    info.channels |= static_cast<uint8_t>(1U << (text[start] - 'A'));
    start = end + 1;
  }
  if (!info.channels) {
    *reason = "no channel lines";
    return false;
  }

  uint32_t lengthTicks = 0;
  bool first = true;
  bool anyNote = false;
  uint16_t tempo = 0;
  for (uint8_t channel = 0; channel < CHANNEL_COUNT; ++channel) {
    if (!(info.channels & (1U << channel))) continue;
    ChannelCursor cursor;
    cursor.begin(text, length, channel);
    Note note;
    while (cursor.next(note)) {
      if (note.note >= 0) anyNote = true;
    }
    if (cursor.error()) {
      *reason = cursor.error();
      return false;
    }
    if (cursor.tempo()) {
      if (tempo && tempo != cursor.tempo()) {
        *reason = "channels use different tempos";
        return false;
      }
      tempo = cursor.tempo();
    }
    if (first) {
      lengthTicks = cursor.tick();
      first = false;
    } else if (cursor.tick() != lengthTicks) {
      *reason = "channels have different lengths";
      return false;
    }
  }
  if (!anyNote) {
    *reason = "tune has no notes";
    return false;
  }
  info.tempo = tempo ? tempo : DEFAULT_TEMPO;
  info.lengthTicks = lengthTicks;
  if (static_cast<uint64_t>(lengthTicks) * 60 >
      static_cast<uint64_t>(MAX_SECONDS) * info.tempo * TICKS_PER_QUARTER) {
    *reason = "tune too long";
    return false;
  }
  return true;
}

bool Synth::begin(const char *text, size_t length, uint32_t sampleRate,
                  const char **error) {
  if (!validate(text, length, info_, error)) return false;
  text_ = text;
  length_ = length;
  sampleRate_ = sampleRate;
  loopSamples_ = sampleAt(info_.lengthTicks);
  restart();
  return true;
}

uint64_t Synth::sampleAt(uint32_t tick) const {
  return static_cast<uint64_t>(tick) * sampleRate_ * 60 /
         (static_cast<uint64_t>(info_.tempo) * TICKS_PER_QUARTER);
}

void Synth::restart() {
  sample_ = 0;
  for (uint8_t channel = 0; channel < CHANNEL_COUNT; ++channel) {
    Voice &voice = voices_[channel];
    voice = Voice();
    voice.note.note = -1;
    voice.done = !(info_.channels & (1U << channel));
    if (!voice.done) voice.cursor.begin(text_, length_, channel);
  }
}

void Synth::load(Voice &voice, uint8_t channel) {
  if (!voice.cursor.next(voice.note)) {
    voice.done = true;
    voice.note.note = -1;
    return;
  }
  const Note &note = voice.note;
  voice.gateEnd = sampleAt(note.tick + note.gate);
  voice.end = sampleAt(note.tick + note.length);
  if (note.note < 0) return;
  double frequency = 440.0 * pow(2.0, (note.note - 69) / 12.0);
  // The noise channel's pitch is its shift-register clock: brighter when higher.
  if (channel == 3) frequency = fmin(frequency * 8.0, sampleRate_ * 0.99);
  voice.increment = static_cast<uint32_t>(frequency * 4294967296.0 / sampleRate_);
  voice.phase = 0;
  voice.lfsr = 0x7FFF;
  voice.level = note.volume;
  voice.envelopeCounter = 0;
}

int32_t Synth::voiceSample(Voice &voice, uint8_t channel) {
  const Note &note = voice.note;
  // Game Boy envelope: one volume step down every envelope/64 s.
  if (note.envelope && voice.level) {
    if (++voice.envelopeCounter >= note.envelope * sampleRate_ / 64) {
      voice.envelopeCounter = 0;
      --voice.level;
    }
  }
  int32_t value = 0;
  if (channel < 2) {
    // Pulse, offset so each duty averages to zero: no click at note edges.
    static const uint8_t eighths[] = {1, 2, 4, 6}; // 12.5, 25, 50, 75 %
    const uint32_t duty = eighths[note.timbre];
    value = voice.phase < (duty << 29) ? CHANNEL_SPAN * static_cast<int32_t>(8 - duty) / 8
                                       : -CHANNEL_SPAN * static_cast<int32_t>(duty) / 8;
  } else if (channel == 2) {
    const int32_t t = static_cast<int32_t>(voice.phase >> 16);
    if (note.timbre == 0) {
      const int32_t triangle = t < 32768 ? t - 16384 : 49151 - t;
      value = triangle * CHANNEL_SPAN / 32768;
    } else {
      value = (t - 32768) * CHANNEL_SPAN / 65536;
    }
  } else {
    value = (voice.lfsr & 1) ? -CHANNEL_SPAN / 2 : CHANNEL_SPAN / 2;
  }
  const uint32_t before = voice.phase;
  voice.phase += voice.increment;
  if (channel == 3 && voice.phase < before) {
    const uint16_t bit = (voice.lfsr ^ (voice.lfsr >> 1)) & 1;
    voice.lfsr = static_cast<uint16_t>((voice.lfsr >> 1) | (bit << 14));
    if (note.timbre == 1) {
      voice.lfsr = static_cast<uint16_t>((voice.lfsr & ~(1U << 6)) | (bit << 6));
    }
  }
  return value * voice.level / 15;
}

void Synth::render(int16_t *out, size_t samples) {
  for (size_t i = 0; i < samples; ++i) {
    if (sample_ >= loopSamples_) restart();
    int32_t mix = 0;
    for (uint8_t channel = 0; channel < CHANNEL_COUNT; ++channel) {
      Voice &voice = voices_[channel];
      while (!voice.done && sample_ >= voice.end) load(voice, channel);
      if (!voice.done && voice.note.note >= 0 && sample_ < voice.gateEnd) {
        mix += voiceSample(voice, channel);
      }
    }
    if (mix > 32767) mix = 32767;
    if (mix < -32768) mix = -32768;
    out[i] = static_cast<int16_t>(mix);
    ++sample_;
  }
}

} // namespace Mml
