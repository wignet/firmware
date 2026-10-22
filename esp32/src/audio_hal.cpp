#include "audio_hal.h"
#include "board.h"

#if defined(WIGNET_HAS_AUDIO)

#include "mml_tune.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <LittleFS.h>
#include <Wire.h>
#include <driver/i2s.h>

namespace {

constexpr uint8_t PMIC_ADDRESS = 0x34;
constexpr uint8_t PMIC_LDO_ENABLE = 0x90;
constexpr uint8_t PMIC_ALDO3_VOLTAGE = 0x94;
constexpr uint8_t PMIC_ALDO3_ENABLE = 0x04;
constexpr uint8_t PMIC_ALDO3_3300MV = 0x1C;

constexpr uint8_t ES8311_ADDRESS = 0x18;
constexpr int AUDIO_SDA = WIGNET_I2C_SDA;
constexpr int AUDIO_SCL = WIGNET_I2C_SCL;
constexpr int AUDIO_MCLK = WIGNET_I2S_MCLK;
constexpr int AUDIO_BCLK = WIGNET_I2S_BCLK;
constexpr int AUDIO_LRCK = WIGNET_I2S_LRCK;
constexpr int AUDIO_DOUT = WIGNET_I2S_DOUT;
constexpr int AUDIO_PA_ENABLE = WIGNET_AUDIO_PA;

constexpr uint32_t SAMPLE_RATE = 48000;
// I2S DMA: 8 buffers of 512 frames, about 85 ms of audio. Enough to ride out
// scheduling jitter while the loop draws the panel, not a whole refresh:
// the audio task keeps refilling it (startAlarm).
constexpr int DMA_BUFFERS = 8;
constexpr int DMA_FRAMES = 512;
constexpr uint32_t DMA_DEPTH_US =
    static_cast<uint32_t>(1000000ULL * DMA_BUFFERS * DMA_FRAMES / SAMPLE_RATE);
constexpr uint32_t MCLK_RATE = SAMPLE_RATE * 256U;
constexpr int BOOT_BUTTON = WIGNET_BUTTON_BOOT;
constexpr int KEY_BUTTON = WIGNET_BUTTON_KEY;
constexpr int PWR_BUTTON = WIGNET_BUTTON_PWR;
#if defined(WIGNET_PWR_ACTIVE_HIGH)
constexpr int PWR_PRESSED = HIGH;
constexpr int PWR_MODE = INPUT_PULLDOWN;
#else
constexpr int PWR_PRESSED = LOW;
constexpr int PWR_MODE = INPUT_PULLUP;
#endif

// Alarm loudness while ringing: rises from silence to the alarm's volume over
// the fade-in, then holds. Amplitude follows the square of elapsed progress
// because loudness is not linear in amplitude: a straight amplitude ramp is
// already loud a quarter of the way in, and the remaining minutes change
// little. Evaluated once per I2S buffer (about 10 ms), far below audible steps.
struct Ringing {
  uint32_t startedMs;
  uint32_t fadeInMs;
  uint32_t endMs; // fade-in plus the full-volume duration, from startedMs
  uint8_t volume;

  bool expired() const { return millis() - startedMs >= endMs; }

  // Gain in 1/32768 of full scale.
  int32_t gainQ15() const {
    const int32_t full = static_cast<int32_t>(volume) * 32768 / 100;
    const uint32_t elapsed = millis() - startedMs;
    if (fadeInMs == 0 || elapsed >= fadeInMs) return full;
    const int64_t progress = static_cast<int64_t>(elapsed) * 32768 / fadeInMs;
    return static_cast<int32_t>(full * progress * progress / (32768LL * 32768LL));
  }
};

// Extra stop condition for a USB test (AudioHAL::playTest); never set while
// a real alarm rings, so stray serial traffic cannot silence one.
bool (*extraStop)() = nullptr;
// Set by AudioHAL::stop() from another task.
volatile bool stopFlag = false;
// A scene's cue: played once, and the buttons are the scene's, not the sound's.
bool cueMode = false;

// Measured on every buffer written (noteWrite), for AudioHAL::lastStats().
AudioHAL::Stats stats;
uint32_t lastWriteUs = 0;

void resetStats() {
  stats = AudioHAL::Stats();
  lastWriteUs = 0;
}

void noteWrite() {
  const uint32_t now = micros();
  if (lastWriteUs) {
    const uint32_t gap = now - lastWriteUs;
    if (gap > stats.maxGapUs) stats.maxGapUs = gap;
    if (gap > DMA_DEPTH_US) ++stats.dropouts;
  }
  lastWriteUs = now;
  ++stats.writes;
}

// On the PhotoPainter PWR (GPIO5) is also wired to the PMIC, and pulses high
// on its own while the amplifier plays: 5-10 ms every 80-300 ms, measured with the bench's
// `buttons` on the bedroom Wignet (2026-10-10), and never at rest in 8.5
// minutes. A pulse caught by two reads 25 ms apart stopped the ring: alarms
// ended seconds into their fade-in, before they could be heard. So PWR stops a
// ring only once held this long (real presses measured 685-945 ms); BOOT and
// KEY, which do not pulse, stop it at once.
constexpr uint32_t PWR_HOLD_MS = 300;
bool pwrHigh = false;
uint32_t pwrHighSince = 0;
uint32_t pwrPulses = 0; // ignored during the current ring

bool stopRequested() {
  if (stopFlag) {
    Serial.println("[Audio] Stopped by a stop request");
    return true;
  }
  if (extraStop && extraStop()) {
    Serial.println("[Audio] Stopped by the bench (serial input)");
    return true;
  }
  if (cueMode) return false;
  // Checked once per ~10 ms buffer: never blocks playback while PWR is held.
  if (digitalRead(PWR_BUTTON) == PWR_PRESSED) {
    const uint32_t now = millis();
    if (!pwrHigh) {
      pwrHigh = true;
      pwrHighSince = now;
    }
    if (now - pwrHighSince >= PWR_HOLD_MS) {
      Serial.printf("[Audio] Stopped by PWR, held %lu ms\n",
                    static_cast<unsigned long>(now - pwrHighSince));
      pwrHigh = false;
      return true;
    }
  } else if (pwrHigh) {
    ++pwrPulses;
    pwrHigh = false;
  }
  if (digitalRead(BOOT_BUTTON) != LOW && digitalRead(KEY_BUTTON) != LOW) {
    return false;
  }
  delay(25); // Reject contact bounce and transient startup levels.
  const bool boot = digitalRead(BOOT_BUTTON) == LOW;
  const bool key = digitalRead(KEY_BUTTON) == LOW;
  if (boot || key) Serial.printf("[Audio] Stopped by %s\n", boot ? "BOOT" : "KEY");
  return boot || key;
}

bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool readRegister(uint8_t address, uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(true) != 0 ||
      Wire.requestFrom(address, static_cast<uint8_t>(1)) != 1) {
    return false;
  }
  value = Wire.read();
  return true;
}

bool setAudioPower(bool enabled) {
#if !defined(WIGNET_AUDIO_RAIL_AXP2101)
  (void)enabled;
  return true; // the codec is always powered
#endif
  uint8_t control = 0;
  if (!readRegister(PMIC_ADDRESS, PMIC_LDO_ENABLE, control)) {
    return false;
  }
  if (enabled) {
    uint8_t voltage = 0;
    if (!readRegister(PMIC_ADDRESS, PMIC_ALDO3_VOLTAGE, voltage) ||
        !writeRegister(PMIC_ADDRESS, PMIC_ALDO3_VOLTAGE,
                       (voltage & 0xE0) | PMIC_ALDO3_3300MV)) {
      return false;
    }
    control |= PMIC_ALDO3_ENABLE;
  } else {
    control &= ~PMIC_ALDO3_ENABLE;
  }
  return writeRegister(PMIC_ADDRESS, PMIC_LDO_ENABLE, control);
}

bool codecWrite(uint8_t reg, uint8_t value) {
  if (!writeRegister(ES8311_ADDRESS, reg, value)) {
    Serial.printf("[Audio] ES8311 write failed: reg 0x%02X\n", reg);
    return false;
  }
  return true;
}

bool codecRead(uint8_t reg, uint8_t &value) {
  return readRegister(ES8311_ADDRESS, reg, value);
}

bool initI2s() {
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  config.sample_rate = SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL2;
  config.dma_buf_count = DMA_BUFFERS;
  config.dma_buf_len = DMA_FRAMES;
  config.use_apll = true;
  config.tx_desc_auto_clear = true;
  config.fixed_mclk = MCLK_RATE;

  const esp_err_t installed = i2s_driver_install(I2S_NUM_0, &config, 0, nullptr);
  if (installed != ESP_OK) {
    Serial.printf("[Audio] I2S driver install failed: %s\n", esp_err_to_name(installed));
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num = AUDIO_MCLK;
  pins.bck_io_num = AUDIO_BCLK;
  pins.ws_io_num = AUDIO_LRCK;
  pins.data_out_num = AUDIO_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }
  if (i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT,
                  I2S_CHANNEL_STEREO) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }
  return true;
}

bool initCodec() {
  // Sequence adapted from Waveshare's PhotoPainter Audio_Test and Espressif's
  // ES8311 driver. The codec is an I2S slave using a 256*Fs MCLK.
  const uint8_t initial[][2] = {
      {0x44, 0x08}, {0x44, 0x08}, {0x01, 0x30}, {0x02, 0x00},
      {0x03, 0x10}, {0x16, 0x24}, {0x04, 0x10}, {0x05, 0x00},
      {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F},
      {0x00, 0x80}, {0x01, 0x3F}, {0x06, 0x00}, {0x13, 0x10},
      {0x1B, 0x0A}, {0x1C, 0x6A}, {0x44, 0x58},
      // 48 kHz with 12.288 MHz MCLK, normal I2S, 16-bit samples.
      {0x09, 0x0C}, {0x0A, 0x4C}, {0x02, 0x00}, {0x05, 0x00},
      {0x03, 0x10}, {0x04, 0x10}, {0x07, 0x00}, {0x08, 0xFF},
      {0x06, 0x03},
      // Start the DAC. Register 0x32 = roughly -8 dB digital volume.
      {0x17, 0xBF}, {0x0E, 0x02}, {0x12, 0x00}, {0x14, 0x1A},
      {0x0D, 0x01}, {0x15, 0x40}, {0x37, 0x08}, {0x45, 0x00},
      {0x32, 0xAF}, {0x31, 0x00},
  };
  for (const auto &entry : initial) {
    if (!codecWrite(entry[0], entry[1])) {
      return false;
    }
  }
  return true;
}

void stopAudio() {
  digitalWrite(AUDIO_PA_ENABLE, LOW);
  delay(20);
  codecWrite(0x32, 0x00);
  codecWrite(0x0E, 0xFF);
  codecWrite(0x12, 0x02);
  codecWrite(0x0D, 0xFA);
  codecWrite(0x00, 0x00);
  i2s_zero_dma_buffer(I2S_NUM_0);
  i2s_driver_uninstall(I2S_NUM_0);
  // ALDO3 stays on. The writes above put the ES8311 in standby (microamps).
  // Switching the rail off left the unpowered codec on the shared I2C bus
  // (GPIO47/48), which then stopped answering: RTC, PMIC and all, and the
  // PMIC is on that bus, so nothing could switch the rail back on until a
  // power cycle. Seen on hardware 2026-09-26, after every play.
}

uint16_t readU16(File &file) {
  uint8_t bytes[2];
  if (file.read(bytes, sizeof(bytes)) != sizeof(bytes)) {
    return 0;
  }
  return static_cast<uint16_t>(bytes[0]) |
         (static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t readU32(File &file) {
  uint8_t bytes[4];
  if (file.read(bytes, sizeof(bytes)) != sizeof(bytes)) {
    return 0;
  }
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) |
         (static_cast<uint32_t>(bytes[3]) << 24);
}

bool findPcmData(File &file, uint32_t &dataLength) {
  char tag[4];
  if (file.read(reinterpret_cast<uint8_t *>(tag), 4) != 4 ||
      memcmp(tag, "RIFF", 4) != 0) {
    return false;
  }
  readU32(file);
  if (file.read(reinterpret_cast<uint8_t *>(tag), 4) != 4 ||
      memcmp(tag, "WAVE", 4) != 0) {
    return false;
  }

  bool compatibleFormat = false;
  while (file.available()) {
    if (file.read(reinterpret_cast<uint8_t *>(tag), 4) != 4) {
      return false;
    }
    const uint32_t chunkLength = readU32(file);
    const size_t chunkStart = file.position();
    if (memcmp(tag, "fmt ", 4) == 0 && chunkLength >= 16) {
      const uint16_t format = readU16(file);
      const uint16_t channels = readU16(file);
      const uint32_t sampleRate = readU32(file);
      readU32(file);
      readU16(file);
      const uint16_t bits = readU16(file);
      compatibleFormat = format == 1 && channels == 1 &&
                         sampleRate == SAMPLE_RATE && bits == 16;
    } else if (memcmp(tag, "data", 4) == 0) {
      dataLength = chunkLength;
      return compatibleFormat;
    }
    file.seek(chunkStart + chunkLength + (chunkLength & 1U));
  }
  return false;
}

bool streamMonoPcm(File &file, uint32_t dataLength, const Ringing &ringing) {
  constexpr size_t MONO_SAMPLES = 512;
  int16_t mono[MONO_SAMPLES];
  int16_t stereo[MONO_SAMPLES * 2];
  uint32_t remaining = dataLength;

  while (remaining > 0) {
    if (stopRequested() || ringing.expired()) {
      return false;
    }
    const size_t requested =
        min(static_cast<size_t>(remaining), sizeof(mono));
    const size_t bytesRead = file.read(reinterpret_cast<uint8_t *>(mono),
                                       requested);
    if (bytesRead == 0) {
      return false;
    }
    const size_t samples = bytesRead / sizeof(int16_t);
    const int32_t gain = ringing.gainQ15();
    for (size_t i = 0; i < samples; i++) {
      const int16_t scaled = static_cast<int16_t>((static_cast<int32_t>(mono[i]) * gain) >> 15);
      stereo[i * 2] = scaled;
      stereo[i * 2 + 1] = scaled;
    }
    size_t bytesWritten = 0;
    if (i2s_write(I2S_NUM_0, stereo, samples * 2 * sizeof(int16_t),
                  &bytesWritten, portMAX_DELAY) != ESP_OK ||
        bytesWritten != samples * 2 * sizeof(int16_t)) {
      return false;
    }
    noteWrite();
    remaining -= bytesRead;
  }
  return true;
}

bool playFallbackTone(const Ringing &ringing) {
  // 880 Hz pulse, 250 ms on / 250 ms off. No flash asset is required.
  constexpr size_t SAMPLES = 480;
  int16_t stereo[SAMPLES * 2];
  uint32_t sample = 0;
  while (!ringing.expired()) {
    if (stopRequested()) {
      return true;
    }
    const int16_t level = static_cast<int16_t>((12000 * ringing.gainQ15()) >> 15);
    for (size_t i = 0; i < SAMPLES; ++i, ++sample) {
      const bool audible = (sample / (SAMPLE_RATE / 4)) % 2 == 0;
      const int16_t value = audible && (sample % (SAMPLE_RATE / 880)) <
                                        (SAMPLE_RATE / 1760)
                                ? level
                                : 0;
      stereo[i * 2] = value;
      stereo[i * 2 + 1] = value;
    }
    size_t written = 0;
    if (i2s_write(I2S_NUM_0, stereo, sizeof(stereo), &written,
                  portMAX_DELAY) != ESP_OK || written != sizeof(stereo)) {
      return false;
    }
    noteWrite();
  }
  return true;
}

bool playTune(Mml::Synth &synth, const Ringing &ringing) {
  // 10 ms per buffer, like the fallback tone: the fade and the buttons are
  // checked a hundred times a second.
  constexpr size_t SAMPLES = 480;
  int16_t mono[SAMPLES];
  int16_t stereo[SAMPLES * 2];
  // The synth must render 10 ms of audio in well under 10 ms, at whatever
  // CPU frequency the firmware runs; the log says how close it came.
  uint32_t worstRenderUs = 0;
  while (!ringing.expired()) {
    if (stopRequested()) {
      break;
    }
    const uint32_t renderStart = micros();
    synth.render(mono, SAMPLES);
    worstRenderUs = max(worstRenderUs, static_cast<uint32_t>(micros() - renderStart));
    const int32_t gain = ringing.gainQ15();
    for (size_t i = 0; i < SAMPLES; ++i) {
      const int16_t scaled = static_cast<int16_t>((static_cast<int32_t>(mono[i]) * gain) >> 15);
      stereo[i * 2] = scaled;
      stereo[i * 2 + 1] = scaled;
    }
    size_t written = 0;
    if (i2s_write(I2S_NUM_0, stereo, sizeof(stereo), &written,
                  portMAX_DELAY) != ESP_OK || written != sizeof(stereo)) {
      return false;
    }
    noteWrite();
  }
  Serial.printf("[Audio] Tune synth: worst buffer %lu us of 10000 us at %lu MHz\n",
                static_cast<unsigned long>(worstRenderUs),
                static_cast<unsigned long>(getCpuFrequencyMhz()));
  return true;
}

// Static: four channel cursors are too much for the loop task's stack.
Mml::Synth tuneSynth;

// Shared by the alarm and the USB test: `maxDurationSec` is already bounded.
bool ring(const char *path, const char *tune, uint8_t volume,
          uint32_t maxDurationSec, uint32_t fadeInSec) {
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  pinMode(KEY_BUTTON, INPUT_PULLUP);
  pinMode(PWR_BUTTON, PWR_MODE);
  fadeInSec = min(fadeInSec, 3600U);
  // The alarm's duration counts from full volume, so the fade-in never eats
  // into it: 600 s of fade-in and 600 s of duration ring for 20 minutes.
  pwrHigh = false; // a press held before the ring does not count toward stopping it
  pwrPulses = 0;
  Ringing ringing = {millis(), fadeInSec * 1000U,
                     (fadeInSec + maxDurationSec) * 1000U,
                     min(volume, static_cast<uint8_t>(100))};

  // The tune text must outlive playback: the synth reads it as it plays.
  bool tuneReady = false;
  if (tune && *tune) {
    const char *error = nullptr;
    tuneReady = tuneSynth.begin(tune, strlen(tune), SAMPLE_RATE, &error);
    if (!tuneReady) {
      Serial.printf("[Audio] Tune refused (%s); ringing %s\n", error, path);
    } else if (cueMode) {
      // Once: one loop of the tune, if shorter than the cue's limit.
      const uint64_t loopMs = tuneSynth.loopSamples() * 1000ULL / SAMPLE_RATE;
      if (loopMs < ringing.endMs) ringing.endMs = static_cast<uint32_t>(loopMs);
    }
  }
  if (cueMode && !tuneReady) {
    Serial.println("[Audio] Cue without a playable tune: silent");
    return false;
  }

  // AlarmClockHAL::begin() mounted it, and keeps it mounted (the list of
  // sounds lives there too): begin() only mounts it on a build without.
  const bool filesystemReady = !tuneReady && LittleFS.begin(false);
  File wav;
  uint32_t dataLength = 0;
  if (filesystemReady && LittleFS.exists(path)) {
    wav = LittleFS.open(path, "r");
    if (wav && !findPcmData(wav, dataLength)) {
      Serial.println("[Audio] WAV must be 48 kHz mono 16-bit PCM; using tone");
      wav.close();
    }
  }

  Wire.begin(AUDIO_SDA, AUDIO_SCL);
  Wire.setClock(100000);
  pinMode(AUDIO_PA_ENABLE, OUTPUT);
  digitalWrite(AUDIO_PA_ENABLE, LOW);

  Serial.printf("[Audio] Alarm %s (%lu PCM bytes; fallback tone if absent), "
                "fade-in %lu s to volume %u, then %lu s\n",
                tuneReady ? "tune" : path, static_cast<unsigned long>(dataLength),
                static_cast<unsigned long>(fadeInSec), ringing.volume,
                static_cast<unsigned long>(maxDurationSec));
  bool initialized = setAudioPower(true);
  if (!initialized) {
    Serial.println("[Audio] The PMIC did not switch the audio rail on");
  } else {
    delay(50);
    initialized = initI2s();
    if (initialized && !initCodec()) {
      i2s_driver_uninstall(I2S_NUM_0);
      initialized = false;
    }
  }
  if (!initialized) {
    Serial.println("[Audio] Codec initialization failed");
    if (wav) wav.close();
    return false;
  }

  digitalWrite(AUDIO_PA_ENABLE, HIGH);
  delay(20);
  bool played = true;
  ringing.startedMs = millis(); // a cue's length counts from its first sample
  if (tuneReady) {
    played = playTune(tuneSynth, ringing);
  } else if (wav && dataLength > 0) {
    const size_t pcmStart = wav.position();
    while (!ringing.expired() && !stopRequested()) {
      if (!wav.seek(pcmStart)) {
        played = false;
        break;
      }
      // A false return can mean button dismissal, timeout or read failure.
      if (!streamMonoPcm(wav, dataLength, ringing)) {
        break;
      }
    }
    wav.close();
  } else {
    played = playFallbackTone(ringing);
  }

  int16_t silence[256] = {};
  size_t ignored = 0;
  i2s_write(I2S_NUM_0, silence, sizeof(silence), &ignored, portMAX_DELAY);
  delay(100);
  stopAudio();

  stats.played = played;
  Serial.printf("[Audio] Alarm ended: %lu buffers, worst gap %lu us (DMA holds %lu us), "
                "%lu dropouts, %lu PWR pulses ignored\n",
                static_cast<unsigned long>(stats.writes), static_cast<unsigned long>(stats.maxGapUs),
                static_cast<unsigned long>(DMA_DEPTH_US), static_cast<unsigned long>(stats.dropouts),
                static_cast<unsigned long>(pwrPulses));
  return played;
}

// One sound at a time, played by audioTask. The strings are copies: the
// caller's may not outlive the call, and the synth reads the tune as it plays.
struct Job {
  String path;
  String tune;
  uint8_t volume = 0;
  uint32_t maxDurationSec = 0;
  uint32_t fadeInSec = 0;
  bool cue = false;
  uint32_t cueMs = 0;
  bool tone = false;
  AudioHAL::Wave wave = AudioHAL::Wave::Triangle;
  uint32_t glideMs = 0;
};
Job job;
volatile bool playing = false;
volatile float toneTargetHz = 440.0f;

// One cycle of a sine, for the tone: a table lookup per sample, not sinf().
int16_t sineTable[256];

int16_t waveSample(AudioHAL::Wave wave, uint32_t phase) {
  switch (wave) {
  case AudioHAL::Wave::Sine:
    return sineTable[phase >> 24];
  case AudioHAL::Wave::Square:
    return phase < 0x80000000U ? 32767 : -32767;
  case AudioHAL::Wave::Saw:
    return static_cast<int16_t>(static_cast<int32_t>(phase >> 16) - 32768);
  default: { // triangle
    const int32_t ramp = static_cast<int32_t>(phase >> 15); // 0..131071
    return static_cast<int16_t>(ramp < 65536 ? ramp - 32768 : 98303 - ramp);
  }
  }
}

// The tone until stop(): one 10 ms buffer at a time, its frequency moving a
// step toward toneTargetHz per buffer, faded in and out over 20 ms (no click).
bool toneRing(AudioHAL::Wave wave, uint8_t volume, uint32_t glideMs) {
  for (int i = 0; i < 256; i++) sineTable[i] = static_cast<int16_t>(32767 * sinf(i * 2 * PI / 256));
  Wire.begin(AUDIO_SDA, AUDIO_SCL);
  pinMode(AUDIO_PA_ENABLE, OUTPUT);
  digitalWrite(AUDIO_PA_ENABLE, LOW);
  bool initialized = setAudioPower(true);
  if (initialized) {
    delay(50);
    initialized = initI2s();
    if (initialized && !initCodec()) {
      i2s_driver_uninstall(I2S_NUM_0);
      initialized = false;
    }
  }
  if (!initialized) {
    Serial.println("[Audio] Tone: codec initialization failed");
    return false;
  }
  digitalWrite(AUDIO_PA_ENABLE, HIGH);
  delay(20);
  Serial.printf("[Audio] Tone at volume %u, glide %lu ms\n", volume, static_cast<unsigned long>(glideMs));
  constexpr size_t SAMPLES = 480;
  int16_t stereo[SAMPLES * 2];
  // Per buffer: the share of the way to the target covered in 10 ms.
  const float step = glideMs < 10 ? 1.0f : 1.0f - expf(-10.0f / glideMs);
  const int32_t full = static_cast<int32_t>(min(volume, static_cast<uint8_t>(100))) * 14000 / 100;
  float hz = toneTargetHz;
  uint32_t phase = 0;
  int32_t level = 0;          // ramps to `full`, then back to 0 on stop
  const int32_t ramp = full / 2 + 1; // per buffer: 20 ms each way
  bool stopping = false;
  for (;;) {
    if (!stopping && stopRequested()) stopping = true;
    level = stopping ? max<int32_t>(0, level - ramp) : min(full, level + ramp);
    hz += (toneTargetHz - hz) * step;
    const uint32_t increment = static_cast<uint32_t>(hz * 4294967296.0f / SAMPLE_RATE);
    for (size_t i = 0; i < SAMPLES; ++i, phase += increment) {
      const int16_t value = static_cast<int16_t>((waveSample(wave, phase) * level) >> 15);
      stereo[i * 2] = value;
      stereo[i * 2 + 1] = value;
    }
    size_t written = 0;
    if (i2s_write(I2S_NUM_0, stereo, sizeof(stereo), &written, portMAX_DELAY) != ESP_OK ||
        written != sizeof(stereo)) break;
    noteWrite();
    if (stopping && level == 0) break;
  }
  int16_t silence[256] = {};
  size_t ignored = 0;
  i2s_write(I2S_NUM_0, silence, sizeof(silence), &ignored, portMAX_DELAY);
  delay(50);
  stopAudio();
  Serial.printf("[Audio] Tone ended: %lu buffers, %lu dropouts\n", static_cast<unsigned long>(stats.writes),
                static_cast<unsigned long>(stats.dropouts));
  return true;
}

void audioTask(void *) {
  cueMode = job.cue || job.tone;
  if (job.tone) {
    stats.played = toneRing(job.wave, job.volume, job.glideMs);
  } else if (job.cue) {
    // ring() counts whole seconds; a cue ends at its own length below that.
    ring("", job.tune.c_str(), job.volume, (job.cueMs + 999) / 1000, 0);
  } else {
    ring(job.path.c_str(), job.tune.c_str(), job.volume, job.maxDurationSec, job.fadeInSec);
  }
  cueMode = false;
  // Bytes never touched: how much of AUDIO_STACK a real ring left spare.
  Serial.printf("[Audio] Task stack spare: %u bytes\n",
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  playing = false;
  vTaskDelete(nullptr);
}

// Above the Arduino loop (priority 1) on the same app core, so a refill is
// never late behind a panel refresh; i2s_write blocks the task between
// buffers, which is when the loop runs. Wi-Fi keeps core 0.
constexpr UBaseType_t AUDIO_PRIORITY = 3;
constexpr BaseType_t AUDIO_CORE = 1;
constexpr uint32_t AUDIO_STACK = 12288; // bytes; see "Task stack spare" in the log

bool startJob(const char *path, const char *tune, uint8_t volume,
              uint32_t maxDurationSec, uint32_t fadeInSec, uint32_t cueMs = 0, bool tone = false) {
  if (playing) return false;
  job.tone = tone;
  job.cue = cueMs > 0;
  job.cueMs = cueMs;
  job.path = path ? path : "";
  job.tune = tune ? tune : "";
  job.volume = volume;
  job.maxDurationSec = maxDurationSec;
  job.fadeInSec = fadeInSec;
  stopFlag = false;
  resetStats(); // before the task: an early failure must not report the last ring
  playing = true;
  if (xTaskCreatePinnedToCore(audioTask, "audio", AUDIO_STACK, nullptr, AUDIO_PRIORITY,
                              nullptr, AUDIO_CORE) != pdPASS) {
    playing = false;
    Serial.println("[Audio] Could not start the audio task");
    return false;
  }
  return true;
}

} // namespace

namespace AudioHAL {

bool playAlarm(const char *path, const char *tune, uint8_t volume,
               uint32_t maxDurationSec, uint32_t fadeInSec) {
  if (!startAlarm(path, tune, volume, maxDurationSec, fadeInSec)) return false;
  while (playing) delay(20);
  return stats.played;
}

bool playTest(const char *tune, uint8_t volume, uint32_t seconds, bool (*shouldStop)()) {
  extraStop = shouldStop;
  // No WAV path: the caller validated the tune; a refused one beeps.
  const bool ok = startTest("", tune, volume, seconds);
  while (ok && playing) delay(20);
  extraStop = nullptr;
  return ok && stats.played;
}

bool startAlarm(const char *path, const char *tune, uint8_t volume,
                uint32_t maxDurationSec, uint32_t fadeInSec) {
  return startJob(path, tune, volume, constrain(maxDurationSec, 30U, 3600U), fadeInSec);
}

bool startTest(const char *path, const char *tune, uint8_t volume, uint32_t seconds) {
  return startJob(path, tune, volume, constrain(seconds, 1U, 3600U), 0);
}

bool startCue(const char *tune, uint8_t volume, uint32_t maxMs) {
  if (!tune || !*tune) return false;
  return startJob("", tune, volume, 0, 0, constrain(maxMs, 1U, 10000U));
}

bool isRinging() { return playing && !job.cue && !job.tone; }

bool startTone(Wave wave, float hz, uint8_t volume, uint32_t glideMs) {
  toneTargetHz = hz;
  job.wave = wave;
  job.glideMs = glideMs;
  return startJob("", "", volume, 0, 0, 0, true);
}

void setToneHz(float hz) { toneTargetHz = hz; }

bool isPlaying() { return playing; }

void stop() { stopFlag = true; }

uint32_t watchSeconds = 0;

void watchTask(void *) {
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  pinMode(KEY_BUTTON, INPUT_PULLUP);
  pinMode(PWR_BUTTON, PWR_MODE);
  const int pins[3] = {BOOT_BUTTON, KEY_BUTTON, PWR_BUTTON};
  const char *names[3] = {"BOOT", "KEY", "PWR"};
  const int pressed[3] = {LOW, LOW, PWR_PRESSED};
  int level[3];
  uint32_t since[3];
  const uint32_t start = millis();
  for (int i = 0; i < 3; ++i) {
    level[i] = digitalRead(pins[i]);
    since[i] = start;
  }
  Serial.printf("[bench] buttons: watching %lu s; now BOOT=%s KEY=%s PWR=%s\n",
                static_cast<unsigned long>(watchSeconds),
                level[0] == LOW ? "pressed" : "up", level[1] == LOW ? "pressed" : "up",
                level[2] == PWR_PRESSED ? "pressed" : "up");
  uint32_t changes = 0;
  while (millis() - start < watchSeconds * 1000UL) {
    const uint32_t now = millis();
    for (int i = 0; i < 3; ++i) {
      const int read = digitalRead(pins[i]);
      if (read == level[i]) continue;
      ++changes;
      Serial.printf("[bench] buttons: +%lu ms %s %s (was %s for %lu ms)\n",
                    static_cast<unsigned long>(now - start), names[i],
                    read == pressed[i] ? "PRESSED" : "released",
                    level[i] == pressed[i] ? "pressed" : "up",
                    static_cast<unsigned long>(now - since[i]));
      level[i] = read;
      since[i] = now;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  Serial.printf("[bench] buttons: done, %lu changes in %lu s\n",
                static_cast<unsigned long>(changes), static_cast<unsigned long>(watchSeconds));
  watchSeconds = 0;
  vTaskDelete(nullptr);
}

void watchButtons(uint32_t seconds) {
  if (watchSeconds) {
    Serial.println("[bench] buttons: already watching");
    return;
  }
  watchSeconds = constrain(seconds, 1U, 3600U);
  xTaskCreatePinnedToCore(watchTask, "buttons", 4096, nullptr, 1, nullptr, 1);
}

Stats lastStats() { return stats; }

} // namespace AudioHAL

#else

namespace AudioHAL {
bool playAlarm(const char *, const char *, uint8_t, uint32_t, uint32_t) { return false; }
bool playTest(const char *, uint8_t, uint32_t, bool (*)()) { return false; }
bool startAlarm(const char *, const char *, uint8_t, uint32_t, uint32_t) { return false; }
bool startTest(const char *, const char *, uint8_t, uint32_t) { return false; }
bool isPlaying() { return false; }
bool startCue(const char *, uint8_t, uint32_t) { return false; }
bool isRinging() { return false; }
bool startTone(Wave, float, uint8_t, uint32_t) { return false; }
void setToneHz(float) {}
void watchButtons(uint32_t) {}
void stop() {}
Stats lastStats() { return Stats(); }
} // namespace AudioHAL

#endif
