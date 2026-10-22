#include "usb_console.h"

#if defined(WIGNET_BENCH_CONSOLE)

#include <Arduino.h>
#include "power_hal.h"
#if defined(WIGNET_BENCH_AUDIO)
#include "alarm_clock_hal.h"
#include "audio_hal.h"
#include "mml_tune.h"
#include <Wire.h>
#endif

namespace {

constexpr size_t MAX_TUNE_BYTES = 2048; // as backoffice/mml.py MAX_BYTES
constexpr size_t MAX_LINE = MAX_TUNE_BYTES + 64;
constexpr size_t RX_BUFFER = 4096;
constexpr char HEADER[] = "#MML1\n";
constexpr uint8_t DEFAULT_VOLUME = 60;
constexpr uint32_t DEFAULT_SECONDS = 30;

String line;         // the line being received
bool lineTooLong = false;
bool collecting = false;
String pending;      // tune lines received since "mml"
String tune;         // the last tune accepted, with its header
void (*sleepHandler)(bool rtcOnly) = nullptr;
bool (*imageHandler)(Stream &source, size_t length) = nullptr;
void (*resumeHandler)() = nullptr;
void (*drawHandler)(int index, int count) = nullptr;
constexpr size_t IMAGE_CHUNK = 4096;
// A 800x480 WGN6 is 192,020 bytes; a 1-bit BMP of it 48,062.
constexpr long MAX_IMAGE_BYTES = 256 * 1024;

// Exactly `length` bytes of USB input, as a Stream for the image decoder.
// Each IMAGE_CHUNK is acknowledged ("[bench] +") before the sender sends the
// next, so the 4 KB receive buffer cannot overflow while the decoder is busy.
// Over a UART bridge at 115200 baud a 1-bit 800x480 BMP takes about 4 s.
class ChunkedUsbStream : public Stream {
public:
  explicit ChunkedUsbStream(size_t length) : remaining_(length) { setTimeout(10000); }
  int available() override {
    return static_cast<int>(min(static_cast<size_t>(Serial.available()), remaining_));
  }
  int read() override {
    if (!remaining_ || !Serial.available()) return -1;
    const int c = Serial.read();
    if (c >= 0) consumed(1);
    return c;
  }
  int peek() override { return remaining_ ? Serial.peek() : -1; }
  size_t write(uint8_t) override { return 0; }
  size_t readBytes(char *buffer, size_t length) override {
    size_t got = 0;
    uint32_t lastData = millis();
    while (got < length && remaining_) {
      const size_t want = min(min(length - got, remaining_), chunkLeft_);
      const size_t n = Serial.read(reinterpret_cast<uint8_t *>(buffer + got), want);
      if (n) {
        got += n;
        consumed(n);
        lastData = millis();
      } else if (millis() - lastData > _timeout) {
        break;
      } else {
        delay(1);
      }
    }
    return got;
  }
  // What the decoder did not read (it stops at the first bad byte): consumed
  // and acknowledged, so the sender finishes and none of it reads as a command.
  void drain() {
    char sink[256];
    while (remaining_ && readBytes(sink, min(sizeof(sink), remaining_))) {
    }
  }
  size_t remaining() const { return remaining_; }
  uint32_t lastByteMs() const { return lastByteMs_; }

private:
  void consumed(size_t n) {
    remaining_ -= n;
    chunkLeft_ -= n;
    if (!remaining_) {
      lastByteMs_ = millis();
    } else if (!chunkLeft_) {
      Serial.println("[bench] +");
      Serial.flush();
      chunkLeft_ = IMAGE_CHUNK;
    }
  }
  size_t remaining_;
  size_t chunkLeft_ = IMAGE_CHUNK;
  uint32_t lastByteMs_ = 0;
};

#if defined(WIGNET_BENCH_AUDIO)
constexpr int64_t MIN_VALID_EPOCH = 1735689600; // 2025-01-01, as alarm_clock_hal

// Any input stops a tune that is playing; the input itself is discarded.
bool serialStop() {
  if (!Serial.available()) return false;
  while (Serial.available()) Serial.read();
  return true;
}

void acceptTune() {
  collecting = false;
  String candidate = pending.startsWith(HEADER) ? pending : String(HEADER) + pending;
  pending = "";
  while (candidate.endsWith("\n")) candidate.remove(candidate.length() - 1);
  Mml::Info info;
  const char *error = nullptr;
  if (!Mml::validate(candidate.c_str(), candidate.length(), info, &error)) {
    Serial.printf("[bench] error %s\n", error);
    return;
  }
  tune = candidate;
  char channels[5] = {};
  size_t count = 0;
  for (uint8_t channel = 0; channel < Mml::CHANNEL_COUNT; ++channel) {
    if (info.channels & (1U << channel)) channels[count++] = static_cast<char>('A' + channel);
  }
  const float seconds = info.lengthTicks * 60.0f / (info.tempo * Mml::TICKS_PER_QUARTER);
  Serial.printf("[bench] ok %.1f s per loop, t%u, channels %s, %u bytes\n", seconds,
                info.tempo, channels, static_cast<unsigned>(tune.length()));
}

void collect(const String &text) {
  if (text == "end") {
    acceptTune();
    return;
  }
  if (text == "mml") {
    // A sender that gave up halfway starts again: never a tune line, which
    // begins with a channel letter.
    pending = "";
    Serial.println("[bench] ready: send the tune, then a line end");
    return;
  }
  // Blank lines and comments (a person typing): acknowledged, not kept.
  if (!text.isEmpty() && !text.startsWith(";")) {
    if (pending.length() + text.length() + 1 > MAX_TUNE_BYTES + sizeof(HEADER)) {
      collecting = false;
      pending = "";
      Serial.printf("[bench] error tune larger than %u bytes\n",
                    static_cast<unsigned>(MAX_TUNE_BYTES));
      return;
    }
    pending += text;
    pending += '\n';
  }
  Serial.println("[bench] +");
}

void play(const String &args) {
  if (tune.isEmpty()) {
    Serial.println("[bench] error no tune yet: send one with mml ... end");
    return;
  }
  long volume = DEFAULT_VOLUME;
  long seconds = DEFAULT_SECONDS;
  sscanf(args.c_str(), "%ld %ld", &volume, &seconds);
  volume = constrain(volume, 0L, 100L);
  seconds = constrain(seconds, 1L, 3600L);
  Serial.printf("[bench] playing %ld s at volume %ld (stop: any input or a button)\n",
                seconds, volume);
  Serial.flush(); // before playback blocks the loop
  const bool played = AudioHAL::playTest(tune.c_str(), static_cast<uint8_t>(volume),
                                         static_cast<uint32_t>(seconds), serialStop);
  Serial.println(played ? "[bench] done" : "[bench] error audio did not start");
}

// A scene's sound (AudioHAL::startCue): the tune once, at most 10 s. The
// buttons stay free: every press while it plays is logged, not taken.
void cue(const String &args) {
  if (tune.isEmpty()) {
    Serial.println("[bench] error no tune yet: send one with mml ... end");
    return;
  }
  long volume = DEFAULT_VOLUME;
  sscanf(args.c_str(), "%ld", &volume);
  volume = constrain(volume, 0L, 100L);
  if (!AudioHAL::startCue(tune.c_str(), static_cast<uint8_t>(volume), 10000)) {
    Serial.println("[bench] error audio did not start (already playing?)");
    return;
  }
  Serial.printf("[bench] cue at volume %ld; press BOOT or KEY while it plays\n", volume);
  const unsigned long startMs = millis();
  bool boot = false, key = false;
  unsigned presses = 0;
  while (AudioHAL::isPlaying()) {
    const bool bootNow = digitalRead(WIGNET_BUTTON_BOOT) == LOW;
    const bool keyNow = digitalRead(WIGNET_BUTTON_KEY) == LOW;
    if (bootNow && !boot) Serial.printf("[bench] BOOT pressed at %lu ms, still playing\n", millis() - startMs);
    if (keyNow && !key) Serial.printf("[bench] KEY pressed at %lu ms, still playing\n", millis() - startMs);
    presses += (bootNow && !boot) + (keyNow && !key);
    boot = bootNow;
    key = keyNow;
    delay(10);
  }
  const AudioHAL::Stats stats = AudioHAL::lastStats();
  Serial.printf("[bench] cue %s after %lu ms, %u presses, %lu buffers, %lu dropouts\n",
                stats.played ? "ended" : "failed", millis() - startMs, presses,
                static_cast<unsigned long>(stats.writes), static_cast<unsigned long>(stats.dropouts));
}

// Sound and panel together, as an alarm with an event image will do: the
// audio task plays while this (the loop) redraws. A panel refresh takes tens
// of seconds, far longer than the ~85 ms the I2S DMA holds, so any stall of
// the audio task shows as a dropout.
void ringDraw(const String &args) {
  if (!drawHandler) {
    Serial.println("[bench] error no panel on this build");
    return;
  }
  long draws = 2;
  long volume = DEFAULT_VOLUME;
  sscanf(args.c_str(), "%ld %ld", &draws, &volume);
  draws = constrain(draws, 1L, 10L);
  volume = constrain(volume, 0L, 100L);
  // Long enough for every refresh; stopped as soon as the last one is done.
  const uint32_t seconds = static_cast<uint32_t>(draws) * 60U + 10U;
  if (!AudioHAL::startTest("", tune.c_str(), static_cast<uint8_t>(volume), seconds)) {
    Serial.println("[bench] error audio did not start (already playing?)");
    return;
  }
  Serial.printf("[bench] playing %s at volume %ld, drawing %ld times\n",
                tune.isEmpty() ? "the tone" : "the tune", volume, draws);
  const unsigned long startMs = millis();
  for (long i = 1; i <= draws && AudioHAL::isPlaying(); ++i) {
    const unsigned long drawStartMs = millis();
    drawHandler(static_cast<int>(i), static_cast<int>(draws));
    Serial.printf("[bench] draw %ld/%ld took %lu ms\n", i, draws, millis() - drawStartMs);
  }
  const bool stoppedEarly = !AudioHAL::isPlaying();
  AudioHAL::stop();
  while (AudioHAL::isPlaying()) delay(10);
  const AudioHAL::Stats stats = AudioHAL::lastStats();
  if (!stats.played) {
    Serial.println("[bench] error audio did not start");
    return;
  }
  Serial.printf("[bench] done %lu ms, %lu buffers, worst gap %lu us, %lu dropouts%s\n",
                millis() - startMs, static_cast<unsigned long>(stats.writes),
                static_cast<unsigned long>(stats.maxGapUs),
                static_cast<unsigned long>(stats.dropouts),
                stoppedEarly ? " (sound stopped before the draws ended)" : "");
}

// The board's shared I2C bus (board.h). PhotoPainter: RTC (0x51), codec
// (0x18), PMIC (0x34); LCD 1.54: codecs (0x18, 0x40) and the QMI8658 (0x6B).
constexpr int I2C_SDA = WIGNET_I2C_SDA;
constexpr int I2C_SCL = WIGNET_I2C_SCL;

void i2cReport() {
  Wire.end();
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(50);
  Serial.printf("[bench] i2c lines idle: SDA %s, SCL %s (both should be high)\n",
                digitalRead(I2C_SDA) ? "high" : "LOW", digitalRead(I2C_SCL) ? "high" : "LOW");
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  Wire.setTimeOut(20); // a broken bus must not hang the scan
  String found;
  for (uint8_t address = 0x08; address < 0x78; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission(true) == 0) found += String(" 0x") + String(address, HEX);
  }
  Serial.printf("[bench] i2c answering:%s (expected 0x18 codec when powered, 0x34 PMIC, 0x51 RTC)\n",
                found.isEmpty() ? " none" : found.c_str());
  Wire.setTimeOut(50); // the Arduino default, for the drivers sharing the bus
}

// Standard bus recovery: a device reset mid-transfer can hold SDA low until
// it sees the rest of its byte. Nine clock pulses and a STOP release it.
void i2cRecover() {
  Wire.end();
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SCL, HIGH);
  for (int i = 0; i < 9; ++i) {
    digitalWrite(I2C_SCL, LOW);
    delayMicroseconds(10);
    digitalWrite(I2C_SCL, HIGH);
    delayMicroseconds(10);
  }
  pinMode(I2C_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SDA, LOW);
  delayMicroseconds(10);
  digitalWrite(I2C_SDA, HIGH); // STOP: SDA rises while SCL is high
  delayMicroseconds(10);
  Serial.println("[bench] i2c: sent 9 clocks and a STOP");
  i2cReport();
}

void alarm(const String &args) {
  if (args.isEmpty()) {
    AlarmClockHAL::printStatus("[bench]");
    return;
  }
  if (args == "clear") {
    AlarmClockHAL::clearTestAlarm();
    Serial.println("[bench] test alarm cleared");
    return;
  }
  long seconds = -1, volume = 60, fadeIn = 10, duration = 30;
  char word[8] = {};
  const int fields = sscanf(args.c_str(), "in %ld %ld %ld %ld %7s", &seconds, &volume,
                            &fadeIn, &duration, word);
  if (fields < 1 || seconds < 1 || seconds > 86400) {
    Serial.println("[bench] error usage: alarm in <seconds> [volume] [fade-in] [duration] [sound]");
    return;
  }
  const int64_t now = time(nullptr);
  if (now < MIN_VALID_EPOCH) {
    Serial.println("[bench] error the clock is not set yet (no RTC, NTP or platform time)");
    return;
  }
  const bool useSound = args.endsWith(" sound") || tune.isEmpty();
  const int64_t epoch = now + seconds;
  if (!AlarmClockHAL::setTestAlarm(epoch, useSound ? "" : tune.c_str(),
                                   static_cast<uint8_t>(constrain(volume, 0L, 100L)),
                                   static_cast<uint32_t>(constrain(fadeIn, 0L, 3600L)),
                                   static_cast<uint32_t>(constrain(duration, 30L, 3600L)))) {
    Serial.println("[bench] error test alarm refused");
    return;
  }
  Serial.printf("[bench] alarm set epoch %lld in %ld s, %s\n", static_cast<long long>(epoch),
                seconds, useSound ? "sound" : "tune");
}
#endif // WIGNET_BENCH_AUDIO

void image(const String &args) {
  if (!imageHandler) {
    Serial.println("[bench] error image is not available in this firmware");
    return;
  }
  const long length = args.toInt();
  if (length < 16 || length > MAX_IMAGE_BYTES) {
    Serial.printf("[bench] error usage: image <bytes>, 16 to %ld\n", MAX_IMAGE_BYTES);
    return;
  }
  Serial.printf("[bench] send %u bytes per chunk\n", static_cast<unsigned>(IMAGE_CHUNK));
  Serial.flush();
  const uint32_t start = millis();
  ChunkedUsbStream stream(static_cast<size_t>(length));
  const bool shown = imageHandler(stream, static_cast<size_t>(length));
  const size_t unread = stream.remaining();
  const uint32_t drawn = millis();
  stream.drain();
  if (!shown) {
    Serial.println("[bench] error image refused; the panel keeps its image (see the log above)");
  } else if (unread) {
    Serial.printf("[bench] shown, but %u trailing bytes were not part of the image\n",
                  static_cast<unsigned>(unread));
  } else {
    // The decoders read everything before touching the panel, so the time
    // after the last byte is validation plus the panel refresh.
    Serial.printf("[bench] shown: transfer %lu ms, then decode and panel %lu ms; "
                  "platform refresh paused %lu min (resume lifts it)\n",
                  static_cast<unsigned long>(stream.lastByteMs() - start),
                  static_cast<unsigned long>(drawn - stream.lastByteMs()), UsbConsole::BENCH_HOLD_MINUTES);
  }
}

#if defined(WIGNET_BENCH_AUDIO)
void sleepNow(const String &args) {
  if (!sleepHandler) {
    Serial.println("[bench] error sleep is not available in this firmware");
    return;
  }
  const bool rtcOnly = args == "rtc";
  Serial.printf("[bench] sleeping%s: USB drops now and returns when the frame wakes\n",
                rtcOnly ? " (timer backup 2 min late: the RTC must wake it)" : "");
  Serial.flush();
  sleepHandler(rtcOnly); // returns only if sleep was not possible
  Serial.println("[bench] error did not sleep");
}
#endif

// show_image.py reads the formats from the first line to pick its profile.
void help() {
#if defined(WIGNET_DISPLAY_RGB)
  Serial.println("[bench] commands (images: bmp png):");
  Serial.println("[bench]   image <bytes>       then a 1-bit BMP or a panel-size PNG in acknowledged chunks");
#elif defined(WIGNET_BENCH_AUDIO)
  Serial.println("[bench] commands (images: bmp wgn6):");
  Serial.println("[bench]   image <bytes>       then a BMP or WGN6 file in acknowledged chunks");
#else
  Serial.println("[bench] commands (images: bmp):");
  Serial.println("[bench]   image <bytes>       then a 1-bit BMP file in acknowledged chunks");
#endif
#if defined(WIGNET_BENCH_AUDIO)
  Serial.println("[bench]   mml                 then the tune's lines, then: end");
  Serial.println("[bench]   play [vol] [secs]   volume 0-100 (60), seconds 1-3600 (30)");
  Serial.println("[bench]   stop                any input also stops playback");
  Serial.println("[bench]   cue [vol]           the tune once, as a scene's sound; buttons not taken");
  Serial.println("[bench]   alarm in <s> [vol] [fade] [dur] [sound]  test alarm, real path");
  Serial.println("[bench]   alarm [clear]       status of clock and alarms, or clear the test one");
  Serial.println("[bench]   sleep [rtc]         deep sleep now until the next alarm");
  Serial.println("[bench]   buttons [secs]      log every BOOT/KEY/PWR change for secs (300)");
  Serial.println("[bench]   ringdraw [n] [vol]  play while redrawing the panel n times (2)");
#endif
  Serial.println("[bench]   resume              let the platform refresh the panel again");
  Serial.println("[bench]   power               power source and battery, as /schedule reports them");
#if defined(WIGNET_BENCH_AUDIO)
  Serial.println("[bench]   i2c [reset]         bus state and scan; reset = 9-clock recovery");
#endif
}

void handle(String text) {
  text.trim();
#if defined(WIGNET_BENCH_AUDIO)
  if (collecting) {
    collect(text);
    return;
  }
#endif
  if (text.isEmpty()) return;
  const int space = text.indexOf(' ');
  const String command = space < 0 ? text : text.substring(0, space);
  const String args = space < 0 ? String() : text.substring(space + 1);
  if (command == "image") {
    image(args);
  } else if (command == "resume") {
    if (resumeHandler) resumeHandler();
    Serial.println("[bench] platform refresh resumed");
#if defined(WIGNET_BENCH_AUDIO)
  } else if (command == "mml") {
    collecting = true;
    pending = "";
    Serial.println("[bench] ready: send the tune, then a line end");
  } else if (command == "play") {
    play(args);
  } else if (command == "ringdraw") {
    ringDraw(args);
  } else if (command == "cue") {
    cue(args);
  } else if (command == "stop") {
    Serial.println("[bench] idle");
  } else if (command == "alarm") {
    alarm(args);
  } else if (command == "buttons") {
    AudioHAL::watchButtons(args.isEmpty() ? 300 : static_cast<uint32_t>(args.toInt()));
  } else if (command == "sleep") {
    sleepNow(args);
  } else if (command == "i2c") {
    if (args == "reset") {
      i2cRecover();
    } else {
      i2cReport();
    }
#endif
  } else if (command == "power") {
    PowerHAL::describe(Serial);
  } else if (command == "help") {
    help();
  } else {
    Serial.printf("[bench] error unknown command %s (try help)\n", command.c_str());
  }
}

} // namespace

namespace UsbConsole {

void setSleepHandler(void (*handler)(bool rtcOnly)) { sleepHandler = handler; }

void setImageHandler(bool (*handler)(Stream &source, size_t length)) { imageHandler = handler; }

void setResumeHandler(void (*handler)()) { resumeHandler = handler; }

void setDrawHandler(void (*handler)(int index, int count)) { drawHandler = handler; }

// The PhotoPainter's native USB port, or the UART behind a USB-serial bridge
// (256 bytes by default: less than one image chunk).
void prepareSerial() { Serial.setRxBufferSize(RX_BUFFER); }

void poll() {
  while (Serial.available()) {
    const int c = Serial.read();
    if (c == '\n') {
      if (lineTooLong) {
        Serial.println("[bench] error line too long");
        collecting = false;
        pending = "";
      } else {
        handle(line);
      }
      // The native USB port can hold a reply's last partial packet until
      // more data goes out; a script waiting for "ok" hung until its next
      // command (seen on hardware 2026-09-26). Push every reply out now.
      Serial.flush();
      line = "";
      lineTooLong = false;
    } else if (c != '\r' && c >= 0) {
      if (line.length() < MAX_LINE) {
        line += static_cast<char>(c);
      } else {
        lineTooLong = true;
      }
    }
  }
}

} // namespace UsbConsole

#else

namespace UsbConsole {
void prepareSerial() {}
void poll() {}
void setSleepHandler(void (*)(bool)) {}
void setImageHandler(bool (*)(Stream &, size_t)) {}
void setResumeHandler(void (*)()) {}
void setDrawHandler(void (*)(int, int)) {}
} // namespace UsbConsole

#endif
