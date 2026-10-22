#include "alarm_clock_hal.h"
#include "board.h"

#if defined(WIGNET_HAS_AUDIO)

#include "audio_hal.h"
#include "sound_list.h"
#include <utility>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

namespace {

// A PCF85063 on the board's I2C bus, active-low alarm output on
// WIGNET_RTC_INT (PhotoPainter: GPIO47/48, GPIO6). A board without one
// (begin(): "timer backup only") wakes for a sound by the ESP32's timer.
constexpr uint8_t RTC_ADDRESS = 0x51;
constexpr int RTC_SDA = WIGNET_I2C_SDA;
constexpr int RTC_SCL = WIGNET_I2C_SCL;
#if defined(WIGNET_RTC_INT)
constexpr gpio_num_t RTC_INT = static_cast<gpio_num_t>(WIGNET_RTC_INT);
#endif
constexpr uint8_t CONTROL_2 = 0x01;
constexpr uint8_t SECONDS = 0x04;
constexpr uint8_t SECOND_ALARM = 0x0B;
constexpr uint8_t AIE = 0x80;
constexpr uint8_t AF = 0x40;
constexpr int64_t MIN_VALID_EPOCH = 1735689600; // 2025-01-01 UTC
constexpr int64_t ALARM_GRACE_SECONDS = 120;
// Volume rises from silence over this long before holding for maxDurationSec.
// The platform sends `fadeInSec` per alarm; this covers a platform that
// predates the setting.
constexpr uint32_t DEFAULT_FADE_IN_SEC = 600;

struct Snapshot {
  int64_t epoch = 0;
  String occurrenceId;
  String sound = "morning_elasticity";
  uint8_t volume = 70;
  uint32_t maxDurationSec = 600;
  uint32_t fadeInSec = DEFAULT_FADE_IN_SEC;
  uint32_t revision = 0;
  String tune; // normalized MML v1; empty rings `sound`
};

// Same limit as the platform (backoffice/mml.py MAX_BYTES). NVS strings hold
// up to about 4000 bytes, and the partition is shared: keep the cap.
constexpr size_t MAX_TUNE_BYTES = 2048;

Snapshot current;

// The list of sounds (sound_list1): upcoming occurrences and their tunes,
// stored in LittleFS beside the WAV so they ring from deep sleep and without
// Wi-Fi. `listMode` is set while the platform sends `sounds`; a platform
// that does not leaves the single `alarm` snapshot in charge.
constexpr const char *SOUNDS_PATH = "/sounds.json";
bool listMode = false;
uint32_t soundsRevision = 0;
std::vector<SoundList::Entry> held;
std::vector<std::pair<std::string, std::string>> heldTunes; // tuneId, MML text
// Occurrences already rung (SoundList::markDone), in NVS: a reset or the PWR
// button must not ring one again.
std::vector<std::string> doneOccurrences;
// The file as last written: every poll resends the list, and rewriting it
// unchanged would wear the flash.
String storedSounds;

// USB bench alarm (setTestAlarm). RTC slow memory keeps it through deep
// sleep; `magic` tells it from the garbage left there by a power cycle.
constexpr uint32_t TEST_ALARM_MAGIC = 0x7E57A1A3;
struct TestAlarm {
  uint32_t magic;
  int64_t epoch;
  uint8_t volume;
  uint32_t fadeInSec;
  uint32_t maxDurationSec;
  char tune[MAX_TUNE_BYTES + 1];
};
RTC_DATA_ATTR TestAlarm testAlarm;
int timerBackupSlack = 0;

// What happened to the last test alarm. After a deep-sleep wake it rings
// in setup(), before the computer has reattached to USB, so its log lines
// are lost; `alarm` prints this instead.
struct BenchReport {
  uint32_t magic;
  int64_t due;
  int64_t rang;
  int wakeCause; // esp_sleep_wakeup_cause_t at the ring
  bool missed;
};
RTC_DATA_ATTR BenchReport benchReport;

// The last platform sound rung, for the same reason: after a deep-sleep wake
// it rings before USB is back, and the next list no longer has it.
struct LastRing {
  uint32_t magic;
  char occurrenceId[64];
  int64_t due;
  int64_t rang;
  int wakeCause;
};
RTC_DATA_ATTR LastRing lastRing;

void recordRing(const char *occurrenceId, int64_t due, int64_t rang) {
  lastRing.magic = TEST_ALARM_MAGIC;
  strncpy(lastRing.occurrenceId, occurrenceId, sizeof(lastRing.occurrenceId) - 1);
  lastRing.occurrenceId[sizeof(lastRing.occurrenceId) - 1] = '\0';
  lastRing.due = due;
  lastRing.rang = rang;
  lastRing.wakeCause = static_cast<int>(esp_sleep_get_wakeup_cause());
}

const char *wakeCauseName(int cause) {
  switch (cause) {
  case ESP_SLEEP_WAKEUP_UNDEFINED: return "none (the frame was awake)";
  case ESP_SLEEP_WAKEUP_EXT0: return "RTC interrupt (ext0)";
  case ESP_SLEEP_WAKEUP_EXT1: return "PMIC interrupt (ext1)";
  case ESP_SLEEP_WAKEUP_TIMER: return "timer backup";
  default: return "other";
  }
}

bool testAlarmSet() {
  return testAlarm.magic == TEST_ALARM_MAGIC && testAlarm.epoch >= MIN_VALID_EPOCH;
}

// The tune as last written to NVS. Every schedule poll resaves the snapshot;
// rewriting 2 KB of flash each time for an unchanged tune would wear it.
String storedTune;
String lastOccurrenceId;
bool rtcPresent = false;
volatile bool ntpSynchronized = false;

void onTimeSynchronized(struct timeval *) { ntpSynchronized = true; }

uint8_t bcd(uint8_t value) { return (value / 10) * 16 + value % 10; }
uint8_t decimal(uint8_t value) { return (value >> 4) * 10 + (value & 0x0F); }

bool readRegisters(uint8_t start, uint8_t *values, size_t length) {
  Wire.beginTransmission(RTC_ADDRESS);
  Wire.write(start);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(RTC_ADDRESS, static_cast<uint8_t>(length)) != length) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) values[i] = Wire.read();
  return true;
}

bool writeRegisters(uint8_t start, const uint8_t *values, size_t length) {
  Wire.beginTransmission(RTC_ADDRESS);
  Wire.write(start);
  for (size_t i = 0; i < length; ++i) Wire.write(values[i]);
  return Wire.endTransmission(true) == 0;
}

bool readRtcEpoch(int64_t &epoch) {
  uint8_t regs[7] = {};
  if (!readRegisters(SECONDS, regs, sizeof(regs)) || (regs[0] & 0x80)) {
    return false; // oscillator stopped, so calendar contents are untrusted
  }
  tm utc = {};
  utc.tm_sec = decimal(regs[0] & 0x7F);
  utc.tm_min = decimal(regs[1] & 0x7F);
  utc.tm_hour = decimal(regs[2] & 0x3F);
  utc.tm_mday = decimal(regs[3] & 0x3F);
  utc.tm_mon = decimal(regs[5] & 0x1F) - 1;
  utc.tm_year = 100 + decimal(regs[6]);
  if (utc.tm_sec > 59 || utc.tm_min > 59 || utc.tm_hour > 23 ||
      utc.tm_mday < 1 || utc.tm_mday > 31 || utc.tm_mon < 0 || utc.tm_mon > 11) {
    return false;
  }
  // Device system time is explicitly configured as UTC in begin(), so
  // mktime has the same meaning as timegm here (newlib lacks timegm).
  epoch = mktime(&utc);
  return epoch >= MIN_VALID_EPOCH;
}

bool writeRtcEpoch(int64_t epoch) {
  const time_t seconds = static_cast<time_t>(epoch);
  tm utc = {};
  if (!gmtime_r(&seconds, &utc) || utc.tm_year < 125 || utc.tm_year > 199) {
    return false;
  }
  const uint8_t regs[7] = {
      bcd(utc.tm_sec), bcd(utc.tm_min), bcd(utc.tm_hour),
      bcd(utc.tm_mday), bcd(utc.tm_wday), bcd(utc.tm_mon + 1),
      bcd(utc.tm_year - 100),
  };
  return writeRegisters(SECONDS, regs, sizeof(regs));
}

void setSystemEpoch(int64_t epoch) {
  timeval tv = {static_cast<time_t>(epoch), 0};
  settimeofday(&tv, nullptr);
}

void clearRtcInterrupt() {
  uint8_t control = 0;
  if (readRegisters(CONTROL_2, &control, 1)) {
    control &= static_cast<uint8_t>(~(AIE | AF));
    writeRegisters(CONTROL_2, &control, 1);
  }
}

bool armRtcAlarm(int64_t epoch) {
  if (!rtcPresent || epoch < MIN_VALID_EPOCH) return false;
  const time_t seconds = static_cast<time_t>(epoch);
  tm utc = {};
  if (!gmtime_r(&seconds, &utc)) return false;
  clearRtcInterrupt();
  // Match UTC second, minute, hour and day. Weekday compare is disabled.
  const uint8_t alarm[5] = {
      bcd(utc.tm_sec), bcd(utc.tm_min), bcd(utc.tm_hour),
      bcd(utc.tm_mday), 0x80,
  };
  if (!writeRegisters(SECOND_ALARM, alarm, sizeof(alarm))) return false;
  uint8_t control = 0;
  if (!readRegisters(CONTROL_2, &control, 1)) return false;
  control = static_cast<uint8_t>((control & ~AF) | AIE);
  if (!writeRegisters(CONTROL_2, &control, 1)) return false;
#if defined(WIGNET_RTC_INT)
  pinMode(static_cast<int>(RTC_INT), INPUT_PULLUP);
#endif
  return true;
}

// The string value of `key` with its JSON escapes decoded (a tune's lines
// arrive as \n). "" when the key is absent, null or not a string. Only a
// match followed by ':' is the key: "sound": "tune" does not name the tune.
String jsonString(const String &json, const char *key) {
  const String quotedKey = String('"') + key + '"';
  const int length = json.length();
  for (int keyPos = json.indexOf(quotedKey); keyPos >= 0;
       keyPos = json.indexOf(quotedKey, keyPos + 1)) {
    int i = keyPos + quotedKey.length();
    while (i < length && json[i] == ' ') ++i;
    if (i >= length || json[i] != ':') continue;
    ++i;
    while (i < length && json[i] == ' ') ++i;
    if (i >= length || json[i] != '"') return "";
    String value;
    for (++i; i < length; ++i) {
      const char c = json[i];
      if (c == '"') return value;
      if (c != '\\') {
        value += c;
        continue;
      }
      if (++i >= length) break;
      switch (json[i]) {
      case 'n': value += '\n'; break;
      case 'r': value += '\r'; break;
      case 't': value += '\t'; break;
      case 'u': {
        if (i + 4 >= length) return "";
        const long code = strtol(json.substring(i + 1, i + 5).c_str(), nullptr, 16);
        // The platform sends ASCII only; a wider character is dropped.
        if (code > 0 && code < 128) value += static_cast<char>(code);
        i += 4;
        break;
      }
      default: value += json[i]; break; // \" \\ \/
      }
    }
    return ""; // unterminated
  }
  return "";
}

int64_t jsonInteger(const String &json, const char *key, int64_t fallback = 0) {
  const String quotedKey = String('"') + key + '"';
  const int keyPos = json.indexOf(quotedKey);
  if (keyPos < 0) return fallback;
  const int colon = json.indexOf(':', keyPos + quotedKey.length());
  if (colon < 0) return fallback;
  int start = colon + 1;
  while (start < json.length() && json[start] == ' ') ++start;
  int end = start;
  while (end < json.length() && json[end] >= '0' && json[end] <= '9') ++end;
  if (end == start) return fallback;
  return strtoll(json.substring(start, end).c_str(), nullptr, 10);
}

void loadSnapshot() {
  Preferences prefs;
  if (!prefs.begin("wignet-alarm", true)) return;
  current.epoch = prefs.getLong64("epoch", 0);
  current.occurrenceId = prefs.getString("occurrence", "");
  current.sound = prefs.getString("sound", "morning_elasticity");
  current.volume = prefs.getUChar("volume", 70);
  current.maxDurationSec = prefs.getUInt("maxDuration", 600);
  current.fadeInSec = prefs.getUInt("fadeIn", DEFAULT_FADE_IN_SEC);
  current.revision = prefs.getUInt("revision", 0);
  // isKey first: getString logs an error for a missing key, and most
  // frames have no tune.
  current.tune = prefs.isKey("tune") ? prefs.getString("tune", "") : "";
  storedTune = current.tune;
  lastOccurrenceId = prefs.getString("lastDone", "");
  prefs.end();
}

void saveSnapshot() {
  Preferences prefs;
  if (!prefs.begin("wignet-alarm", false)) return;
  prefs.putLong64("epoch", current.epoch);
  prefs.putString("occurrence", current.occurrenceId);
  prefs.putString("sound", current.sound);
  prefs.putUChar("volume", current.volume);
  prefs.putUInt("maxDuration", current.maxDurationSec);
  prefs.putUInt("fadeIn", current.fadeInSec);
  prefs.putUInt("revision", current.revision);
  if (current.tune != storedTune) {
    if (current.tune.isEmpty()) {
      prefs.remove("tune");
    } else {
      prefs.putString("tune", current.tune);
    }
    storedTune = current.tune;
  }
  prefs.end();
}

void markOccurrenceDone() {
  lastOccurrenceId = current.occurrenceId;
  Preferences prefs;
  if (prefs.begin("wignet-alarm", false)) {
    prefs.putString("lastDone", lastOccurrenceId);
    prefs.end();
  }
}

bool validSound(const String &sound) {
  if (sound.isEmpty() || sound.length() > 64) return false;
  for (char c : sound) {
    if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') {
      return false;
    }
  }
  return true;
}

void loadDone() {
  doneOccurrences.clear();
  Preferences prefs;
  if (!prefs.begin("wignet-alarm", true)) return;
  const String joined = prefs.isKey("done") ? prefs.getString("done", "") : "";
  prefs.end();
  int start = 0;
  while (start < static_cast<int>(joined.length())) {
    int end = joined.indexOf('\n', start);
    if (end < 0) end = joined.length();
    if (end > start) doneOccurrences.push_back(joined.substring(start, end).c_str());
    start = end + 1;
  }
}

void markDone(const std::string &occurrenceId) {
  SoundList::markDone(doneOccurrences, occurrenceId);
  String joined;
  for (const std::string &id : doneOccurrences) {
    joined += id.c_str();
    joined += '\n';
  }
  Preferences prefs;
  if (prefs.begin("wignet-alarm", false)) {
    prefs.putString("done", joined);
    prefs.end();
  }
}

const char *heldTune(const std::string &tuneId) {
  for (const auto &tune : heldTunes) {
    if (tune.first == tuneId) return tune.second.c_str();
  }
  return "";
}

// The held list as stored: {"revision", "sounds": [...], "tunes": {...}},
// the fields /schedule sends, so loading and receiving share one reader.
String serializeHeld() {
  JsonDocument doc;
  doc["revision"] = soundsRevision;
  JsonArray sounds = doc["sounds"].to<JsonArray>();
  for (const SoundList::Entry &entry : held) {
    JsonObject item = sounds.add<JsonObject>();
    item["occurrenceId"] = entry.occurrenceId;
    item["eventId"] = entry.eventId;
    item["epoch"] = entry.epoch;
    item["pushed"] = entry.pushed;
    item["sound"] = entry.sound;
    item["volume"] = entry.volume;
    item["fadeInSec"] = entry.fadeInSec;
    item["maxDurationSec"] = entry.maxDurationSec;
    if (!entry.tuneId.empty()) item["tuneId"] = entry.tuneId;
  }
  JsonObject tunes = doc["tunes"].to<JsonObject>();
  for (const auto &tune : heldTunes) tunes[tune.first] = tune.second;
  String out;
  serializeJson(doc, out);
  return out;
}

// Entries and tunes from a /schedule answer or the stored file. A sound
// with a bad name or time is dropped, not the whole list.
void readList(JsonVariantConst root, std::vector<SoundList::Entry> &entries,
              std::vector<std::pair<std::string, std::string>> &tunes) {
  for (JsonObjectConst item : root["sounds"].as<JsonArrayConst>()) {
    SoundList::Entry entry;
    entry.occurrenceId = item["occurrenceId"] | "";
    entry.eventId = item["eventId"] | "";
    entry.sound = item["sound"] | "";
    entry.tuneId = item["tuneId"] | "";
    entry.epoch = item["epoch"] | static_cast<int64_t>(0);
    entry.pushed = item["pushed"] | false;
    entry.volume = static_cast<uint8_t>(constrain(item["volume"] | 70, 0, 100));
    entry.fadeInSec = constrain(item["fadeInSec"] | DEFAULT_FADE_IN_SEC, 0U, 3600U);
    entry.maxDurationSec = constrain(item["maxDurationSec"] | 600U, 30U, 3600U);
    if (entry.epoch < MIN_VALID_EPOCH || entry.occurrenceId.empty() ||
        !validSound(entry.sound.c_str())) {
      Serial.printf("[Alarm] Sound %s ignored: invalid\n", entry.occurrenceId.c_str());
      continue;
    }
    entries.push_back(entry);
  }
  for (JsonPairConst tune : root["tunes"].as<JsonObjectConst>()) {
    const char *text = tune.value() | "";
    if (strlen(text) > MAX_TUNE_BYTES) {
      Serial.printf("[Alarm] Tune %s too large; its sounds ring the WAV\n", tune.key().c_str());
      continue;
    }
    tunes.emplace_back(tune.key().c_str(), text);
  }
}

void saveHeld() {
  const String serialized = serializeHeld();
  if (serialized == storedSounds) return;
  File file = LittleFS.open(SOUNDS_PATH, "w");
  if (!file || file.print(serialized) != serialized.length()) {
    Serial.println("[Alarm] Could not store the list of sounds");
    if (file) file.close();
    return;
  }
  file.close();
  storedSounds = serialized;
}

void loadHeld() {
  File file = LittleFS.open(SOUNDS_PATH, "r");
  if (!file) return;
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error) {
    Serial.printf("[Alarm] Stored list of sounds unreadable (%s); waiting for the platform\n",
                  error.c_str());
    return;
  }
  readList(doc.as<JsonVariantConst>(), held, heldTunes);
  soundsRevision = doc["revision"] | 0U;
  listMode = true;
  storedSounds = serializeHeld();
}

// The `sounds` and `tunes` of a /schedule answer, when it has them.
void updateList(const String &json) {
  if (json.indexOf("\"sounds\"") < 0) {
    // A platform without the list: the single `alarm` rings, as before.
    if (listMode) Serial.println("[Alarm] Platform sent no list of sounds; using its alarm");
    listMode = false;
    return;
  }
  JsonDocument filter;
  filter["soundsRevision"] = true;
  filter["sounds"] = true;
  filter["tunes"] = true;
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, json, DeserializationOption::Filter(filter));
  if (error || !doc["sounds"].is<JsonArrayConst>()) {
    Serial.printf("[Alarm] List of sounds unreadable (%s); keeping the held one\n",
                  error ? error.c_str() : "no array");
    return;
  }
  std::vector<SoundList::Entry> incoming;
  std::vector<std::pair<std::string, std::string>> tunes;
  readList(doc.as<JsonVariantConst>(), incoming, tunes);
  const int64_t now = time(nullptr);
  std::vector<SoundList::Entry> next = SoundList::merge(held, incoming, doneOccurrences, now);
  // A carried pushed sound keeps its tune: the new answer lists only its own.
  for (const SoundList::Entry &entry : next) {
    if (entry.tuneId.empty()) continue;
    bool have = false;
    for (const auto &tune : tunes) have = have || tune.first == entry.tuneId;
    if (!have && *heldTune(entry.tuneId)) tunes.emplace_back(entry.tuneId, heldTune(entry.tuneId));
  }
  held = next;
  heldTunes = tunes;
  soundsRevision = doc["soundsRevision"] | 0U;
  listMode = true;
  saveHeld();
  Serial.printf("[Alarm] Holding %u sounds (revision %lu, %u tunes)\n",
                static_cast<unsigned>(held.size()), static_cast<unsigned long>(soundsRevision),
                static_cast<unsigned>(heldTunes.size()));
}

} // namespace

namespace AlarmClockHAL {

void begin() {
  setenv("TZ", "UTC0", 1);
  tzset();
  Wire.begin(RTC_SDA, RTC_SCL);
  Wire.setClock(100000);
  Wire.beginTransmission(RTC_ADDRESS);
  rtcPresent = Wire.endTransmission(true) == 0;
  loadSnapshot();
  loadDone();
  // Mounted once and kept: the list is read and written here while the audio
  // task streams the WAV. Never formatted: that would erase the WAV.
  if (LittleFS.begin(false)) {
    loadHeld();
  } else {
    Serial.println("[Alarm] LittleFS unavailable; the list of sounds is not kept");
  }
  if (rtcPresent) {
    int64_t rtcEpoch = 0;
    if (readRtcEpoch(rtcEpoch)) {
      setSystemEpoch(rtcEpoch);
      Serial.printf("[Alarm] RTC UTC epoch %lld\n", static_cast<long long>(rtcEpoch));
    } else {
      Serial.println("[Alarm] RTC clock unset; waiting for network time");
    }
    clearRtcInterrupt();
  } else {
    Serial.println("[Alarm] PCF85063 not detected; timer backup only");
  }
}

void syncFromNtp() {
  ntpSynchronized = false;
  sntp_set_time_sync_notification_cb(onTimeSynchronized);
  configTime(0, 0, "time.cloudflare.com", "time.google.com", "pool.ntp.org");
  const uint32_t started = millis();
  while (millis() - started < 5000) {
    const int64_t now = time(nullptr);
    if (ntpSynchronized && now >= MIN_VALID_EPOCH) {
      if (rtcPresent && writeRtcEpoch(now)) {
        Serial.printf("[Alarm] RTC synced to UTC epoch %lld\n",
                      static_cast<long long>(now));
      }
      return;
    }
    delay(100);
  }
  Serial.println("[Alarm] NTP unavailable; using RTC or platform epoch");
}

void updateFromSchedule(const String &json) {
  const int64_t serverEpoch = jsonInteger(json, "serverEpoch");
  const int64_t localEpoch = time(nullptr);
  if (serverEpoch >= MIN_VALID_EPOCH) {
    if (!ntpSynchronized || localEpoch < MIN_VALID_EPOCH) {
      setSystemEpoch(serverEpoch);
      if (rtcPresent) writeRtcEpoch(serverEpoch);
      Serial.println("[Alarm] Clock synchronized from platform epoch");
    } else if (llabs(serverEpoch - localEpoch) > 300) {
      Serial.printf("[Alarm] NTP/platform clock discrepancy: %lld sec\n",
                    static_cast<long long>(localEpoch - serverEpoch));
    }
  }

  updateList(json);

  const int alarmKey = json.indexOf("\"alarm\"");
  if (alarmKey < 0) return; // Older platform: preserve cached alarm.
  const int colon = json.indexOf(':', alarmKey);
  if (colon < 0) return;
  const int objectStart = json.indexOf('{', colon + 1);
  const int nullStart = json.indexOf("null", colon + 1);
  if (nullStart >= 0 && (objectStart < 0 || nullStart < objectStart)) {
    current = Snapshot();
    saveSnapshot();
    clearRtcInterrupt();
    return;
  }
  if (objectStart < 0) return;
  const int objectEnd = json.indexOf('}', objectStart + 1);
  if (objectEnd < 0) return;
  const String object = json.substring(objectStart, objectEnd + 1);
  const int64_t epoch = jsonInteger(object, "nextOccurrenceEpoch");
  const String occurrence = jsonString(object, "occurrenceId");
  const String sound = jsonString(object, "sound");
  if (epoch < MIN_VALID_EPOCH || occurrence.isEmpty() || !validSound(sound)) {
    Serial.println("[Alarm] Invalid snapshot ignored");
    return;
  }
  current.epoch = epoch;
  current.occurrenceId = occurrence;
  current.sound = sound;
  current.volume = static_cast<uint8_t>(constrain(
      jsonInteger(object, "volume", 70), 0LL, 100LL));
  current.maxDurationSec = static_cast<uint32_t>(constrain(
      jsonInteger(object, "maxDurationSec", 600), 30LL, 3600LL));
  current.fadeInSec = static_cast<uint32_t>(constrain(
      jsonInteger(object, "fadeInSec", DEFAULT_FADE_IN_SEC), 0LL, 3600LL));
  current.revision = static_cast<uint32_t>(jsonInteger(object, "revision"));
  // Sent only to firmware reporting tune_mml1; absent or null rings `sound`.
  // The platform validated it; AudioHAL validates again before playing.
  current.tune = jsonString(object, "tune");
  if (current.tune.length() > MAX_TUNE_BYTES) {
    Serial.println("[Alarm] Tune too large; ringing the sound instead");
    current.tune = "";
  }
  saveSnapshot();
  Serial.printf("[Alarm] Cached revision %lu occurrence %s at %lld%s\n",
                static_cast<unsigned long>(current.revision),
                current.occurrenceId.c_str(), static_cast<long long>(epoch),
                current.tune.isEmpty() ? "" : " with a tune");
}

// Whether ringIfDue would ring something now, were nothing playing.
static bool dueNow(int64_t now) {
  if (testAlarmSet() && now >= testAlarm.epoch) return true;
  if (listMode) return SoundList::dueIndex(held, doneOccurrences, now) >= 0;
  return current.epoch >= MIN_VALID_EPOCH && !current.occurrenceId.isEmpty() &&
         current.occurrenceId != lastOccurrenceId &&
         !SoundList::isDone(doneOccurrences, current.occurrenceId.c_str()) &&
         now >= current.epoch && now <= current.epoch + ALARM_GRACE_SECONDS;
}

void ringIfDue() {
  // One sound at a time; a due one waits (within its grace) for this to end.
  if (AudioHAL::isRinging()) return;
  const int64_t now = time(nullptr);
  // A scene's cue gives way to a due alarm: stopped now, the alarm rings on a
  // following call, once the audio task has ended.
  if (AudioHAL::isPlaying()) {
    if (dueNow(now)) AudioHAL::stop();
    return;
  }
  if (testAlarmSet() && now >= testAlarm.epoch) {
    const int64_t late = now - testAlarm.epoch;
    // Cleared before playback, like markOccurrenceDone: a reset or the PWR
    // button cannot replay it. The fields stay readable for the player.
    testAlarm.magic = 0;
    benchReport = {TEST_ALARM_MAGIC, testAlarm.epoch, now,
                   static_cast<int>(esp_sleep_get_wakeup_cause()),
                   late > ALARM_GRACE_SECONDS};
    if (late > ALARM_GRACE_SECONDS) {
      Serial.printf("[Alarm] USB test alarm missed by %lld s; dropped\n",
                    static_cast<long long>(late));
    } else {
      Serial.printf("[Alarm] Ringing USB test alarm (%lld s after its time)\n",
                    static_cast<long long>(late));
      const String path = String('/') + current.sound + ".wav";
      // Returns once it plays: the loop joins Wi-Fi and draws meanwhile.
      AudioHAL::startAlarm(path.c_str(), testAlarm.tune, testAlarm.volume,
                           testAlarm.maxDurationSec, testAlarm.fadeInSec);
      return;
    }
  }
  if (listMode) {
    const int due = SoundList::dueIndex(held, doneOccurrences, now);
    if (due < 0) return;
    const SoundList::Entry entry = held[due];
    // Persisted before playback, like markOccurrenceDone below.
    markDone(entry.occurrenceId);
    recordRing(entry.occurrenceId.c_str(), entry.epoch, now);
    Serial.printf("[Alarm] Ringing %s%s (%lld s after its time)\n", entry.pushed ? "pushed sound " : "",
                  entry.occurrenceId.c_str(), static_cast<long long>(now - entry.epoch));
    const String path = String('/') + entry.sound.c_str() + ".wav";
    AudioHAL::startAlarm(path.c_str(), heldTune(entry.tuneId), entry.volume,
                         entry.maxDurationSec, entry.fadeInSec);
    return;
  }
  if (current.epoch < MIN_VALID_EPOCH || current.occurrenceId.isEmpty() ||
      current.occurrenceId == lastOccurrenceId ||
      SoundList::isDone(doneOccurrences, current.occurrenceId.c_str())) return;
  if (now < current.epoch || now > current.epoch + ALARM_GRACE_SECONDS) return;
  // Persist before playback so PWR shutdown or reset cannot replay the same
  // occurrence on boot. The next server snapshot has a new occurrence id.
  markOccurrenceDone();
  markDone(current.occurrenceId.c_str());
  recordRing(current.occurrenceId.c_str(), current.epoch, now);
  Serial.printf("[Alarm] Ringing occurrence %s\n", current.occurrenceId.c_str());
  const String path = String('/') + current.sound + ".wav";
  AudioHAL::startAlarm(path.c_str(), current.tune.c_str(), current.volume,
                       current.maxDurationSec, current.fadeInSec);
}

void prepareSleep(int &seconds) {
  const int64_t now = time(nullptr);
  // The RTC has one alarm: arm the earlier of the platform's and the bench's.
  int64_t next = 0;
  if (listMode) {
    next = SoundList::nextEpoch(held, doneOccurrences, now);
  } else if (current.epoch >= MIN_VALID_EPOCH && current.occurrenceId != lastOccurrenceId) {
    next = current.epoch;
  }
  if (testAlarmSet() && testAlarm.epoch > now && (next == 0 || testAlarm.epoch < next)) {
    next = testAlarm.epoch;
  }
  if (next == 0) {
    clearRtcInterrupt();
    return;
  }
  if (now < MIN_VALID_EPOCH || next <= now) return;
  const int64_t until = next - now;
  if (until < seconds) seconds = static_cast<int>(until) + timerBackupSlack;
#if defined(WIGNET_RTC_INT)
  if (armRtcAlarm(next)) {
    esp_sleep_enable_ext0_wakeup(RTC_INT, 0);
  }
#endif
}

bool setTestAlarm(int64_t epoch, const char *tune, uint8_t volume,
                  uint32_t fadeInSec, uint32_t maxDurationSec) {
  const size_t length = tune ? strlen(tune) : 0;
  if (epoch < MIN_VALID_EPOCH || length > MAX_TUNE_BYTES) return false;
  testAlarm.epoch = epoch;
  testAlarm.volume = min(volume, static_cast<uint8_t>(100));
  testAlarm.fadeInSec = min(fadeInSec, 3600U);
  testAlarm.maxDurationSec = constrain(maxDurationSec, 30U, 3600U);
  memcpy(testAlarm.tune, tune ? tune : "", length);
  testAlarm.tune[length] = '\0';
  testAlarm.magic = TEST_ALARM_MAGIC;
  return true;
}

void clearTestAlarm() { testAlarm.magic = 0; }

void setTimerBackupSlack(int seconds) { timerBackupSlack = max(0, seconds); }

void printStatus(const char *prefix) {
  const auto utc = [](int64_t epoch) {
    const time_t seconds = static_cast<time_t>(epoch);
    tm parts = {};
    char text[24] = "?";
    if (gmtime_r(&seconds, &parts)) strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &parts);
    return String(text);
  };
  const int64_t now = time(nullptr);
  int64_t rtcEpoch = 0;
  const bool rtcRead = rtcPresent && readRtcEpoch(rtcEpoch);
  Serial.printf("%s clock %s UTC (epoch %lld); RTC %s\n", prefix, utc(now).c_str(),
                static_cast<long long>(now),
                rtcRead ? (utc(rtcEpoch) + " UTC").c_str() : "unavailable");
  if (testAlarmSet()) {
    Serial.printf("%s test alarm in %lld s (%s UTC), volume %u, fade-in %lu s, then %lu s, %s\n",
                  prefix, static_cast<long long>(testAlarm.epoch - now),
                  utc(testAlarm.epoch).c_str(), testAlarm.volume,
                  static_cast<unsigned long>(testAlarm.fadeInSec),
                  static_cast<unsigned long>(testAlarm.maxDurationSec),
                  testAlarm.tune[0] ? "tune" : "sound");
  } else {
    Serial.printf("%s test alarm: none\n", prefix);
  }
  if (benchReport.magic == TEST_ALARM_MAGIC) {
    Serial.printf("%s last test alarm %s %lld s after its time (%s UTC), woken by: %s\n",
                  prefix, benchReport.missed ? "MISSED, seen" : "rang",
                  static_cast<long long>(benchReport.rang - benchReport.due),
                  utc(benchReport.due).c_str(), wakeCauseName(benchReport.wakeCause));
  }
  if (lastRing.magic == TEST_ALARM_MAGIC) {
    Serial.printf("%s last sound rang: %s, %lld s after its time (%s UTC), woken by: %s\n", prefix,
                  lastRing.occurrenceId, static_cast<long long>(lastRing.rang - lastRing.due),
                  utc(lastRing.due).c_str(), wakeCauseName(lastRing.wakeCause));
  }
  if (listMode) {
    Serial.printf("%s list of sounds: %u held, revision %lu, %u tunes\n", prefix,
                  static_cast<unsigned>(held.size()), static_cast<unsigned long>(soundsRevision),
                  static_cast<unsigned>(heldTunes.size()));
    for (const SoundList::Entry &entry : held) {
      const bool done = SoundList::isDone(doneOccurrences, entry.occurrenceId);
      Serial.printf("%s   %s %s UTC (in %lld s), %s, volume %u, fade-in %lu s, then %lu s%s%s\n",
                    prefix, entry.occurrenceId.c_str(), utc(entry.epoch).c_str(),
                    static_cast<long long>(entry.epoch - now),
                    entry.tuneId.empty() ? entry.sound.c_str() : "tune", entry.volume,
                    static_cast<unsigned long>(entry.fadeInSec),
                    static_cast<unsigned long>(entry.maxDurationSec), entry.pushed ? ", pushed" : "",
                    done ? ", rung" : "");
    }
    return;
  }
  if (current.epoch < MIN_VALID_EPOCH || current.occurrenceId.isEmpty()) {
    Serial.printf("%s platform alarm: none cached\n", prefix);
  } else {
    Serial.printf("%s platform alarm %s in %lld s (%s UTC), %s%s\n", prefix,
                  current.occurrenceId.c_str(), static_cast<long long>(current.epoch - now),
                  utc(current.epoch).c_str(), current.tune.isEmpty() ? "sound " : "tune",
                  current.tune.isEmpty() ? current.sound.c_str() : "");
  }
}

} // namespace AlarmClockHAL

#else

namespace AlarmClockHAL {
void begin() {}
void syncFromNtp() {}
void updateFromSchedule(const String &) {}
void ringIfDue() {}
void prepareSleep(int &) {}
bool setTestAlarm(int64_t, const char *, uint8_t, uint32_t, uint32_t) { return false; }
void clearTestAlarm() {}
void setTimerBackupSlack(int) {}
void printStatus(const char *) {}
} // namespace AlarmClockHAL

#endif
