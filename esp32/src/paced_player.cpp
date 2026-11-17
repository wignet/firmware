/**
 * Paced scene player (paced_player.h): scenes on e-paper.
 *
 * Two tasks share the scene, each taking `lock`: the scene task (buttons
 * every 10 ms, the runtime's timers, sounds) and the loop (service(): the
 * bundle, events, reports, and drawing). The loop draws without the lock, so
 * during a refresh of 4 to 20 s the scene task still reads every press and
 * runs what it asks at once; a page it asks for meanwhile only changes
 * `wanted`, and the loop draws the last one wanted when the lock is over.
 */
#include "paced_player.h"

#if defined(WIGNET_PACED_PLAYER)

#include "board.h"
#include "config.h"
#include "scene_runtime.h"
#if defined(WIGNET_HAS_AUDIO)
#include "audio_hal.h"
#endif
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

// D, the time a drawn page stays up past its refresh (the platform's
// PACED_HOLD_SECONDS): rule 1's lock is the refresh (R) and then this.
constexpr uint32_t HOLD_MS = 5000;
constexpr uint32_t POLL_MS = 10;          // buttons and timers
constexpr uint32_t SHORT_MAX_MS = 600;    // shorter: short; longer: long
constexpr uint32_t EXIT_HOLD_MS = 5000;   // the way out of a scene, never the scene's
constexpr uint32_t AFTER_SOUND_MS = 1000; // a press just after a ring is still the ring's
constexpr uint32_t SOUND_MAX_MS = 10000;
constexpr uint32_t LOOP_MAX_MS = 3600000; // a looping sound: until silence, at most an hour
constexpr size_t REPORTS_MAX = 32;
constexpr uint32_t RETRY_MS = 30000;
// Flash left free besides the pages: LittleFS needs a few blocks to write.
constexpr size_t FLASH_SPARE = 16 * 1024;
const char *const DIR = "/paced";

SemaphoreHandle_t lock = nullptr;
struct Locked {
  Locked() { xSemaphoreTakeRecursive(lock, portMAX_DELAY); }
  ~Locked() { xSemaphoreGiveRecursive(lock); }
};

PacedPlayer::Hooks hooks;

struct Sound {
  std::string tune;
  uint8_t volume = 50;
  bool loop = false; // played until silence or another sound (scene_volume1)
};

struct Report {
  std::string value, event;
  int page = 0;
};

struct Scene {
  String id;
  bool slide = false;
  int start = 1;
  std::map<int, String> pages; // number: its file in flash
  std::map<std::string, Sound> sounds;
  SceneRuntime::Runtime runtime;
};

std::unique_ptr<Scene> scene; // the one playing

// ------------------------------------------------------------- the screen --

// The page file on screen; "" for a picture the loop drew (/pic, which is
// the start page of the scene /schedule names, or anything else).
String shownPath;
int wantedPage = 0;    // the runtime's page: what the screen should show
int requested = 0;     // page requests since the last draw
volatile bool drawing = false;
volatile uint32_t lockUntilMs = 0;
volatile bool lockSet = false;
// A scene taken down while one of its other pages shows: its start page
// (what /pic serves) goes back up when the lock allows.
String restorePath;

String pathOf(int page) {
  if (!scene) return "";
  auto found = scene->pages.find(page);
  return found == scene->pages.end() ? String() : found->second;
}

// What is on screen, as a file: /pic stands for the start page.
String shownFile() {
  if (shownPath.length() || !scene) return shownPath;
  return pathOf(scene->start);
}

// --------------------------------------------------------- the scene clock --
// Rule 2: the runtime's milliseconds stand still from a page request until
// that page is on screen, so its timers count from the end of the refresh.

uint32_t stoodStillMs = 0, frozenAtMs = 0;
bool frozen = false;

uint32_t sceneNow() { return (frozen ? frozenAtMs : millis()) - stoodStillMs; }

void freeze() {
  if (frozen) return;
  frozen = true;
  frozenAtMs = millis();
}

// The board's clock: serverEpoch and utcOffsetSec from the last /schedule,
// carried forward with millis(). 0: not known yet.
uint32_t clockEpoch = 0, clockMillis = 0;
int32_t clockOffset = 0;
bool clockFresh = false;

// The time of day again, at the scene clock (never from a host callback).
void syncClock() {
  if (!scene || !clockEpoch) return;
  scene->runtime.setClock(clockEpoch + (millis() - clockMillis) / 1000, clockOffset, sceneNow());
}

void thaw() {
  if (!frozen) return;
  stoodStillMs += millis() - frozenAtMs;
  frozen = false;
  syncClock();
}

// ------------------------------------------------------------ the runtime --

std::vector<Report> reports;
String stopReason; // set by the task or the runtime, acted on by service()
bool restartWanted = false; // with stopReason: a slide's hold, played again from its first page
std::string pendingSound;
bool soundLoops = false; // the scene's sound playing now loops: it never outlives the scene
// A pushed scene that ended itself: the server is told (`"ended": true` on
// /input), ends the push, and /pic serves the slides again. Its start page is
// put back only if the server did not end it.
String endedToTell;
String restoreUnlessEnded;

void playSound(const std::string &name) {
#if defined(WIGNET_HAS_AUDIO)
  auto found = scene->sounds.find(name);
  if (found == scene->sounds.end()) return;
  // An alarm or a pushed sound ringing is never cut short by a scene.
  if (AudioHAL::isRinging()) return;
  if (AudioHAL::isPlaying()) {
    pendingSound = name;
    AudioHAL::stop();
    return;
  }
  pendingSound.clear();
  soundLoops = found->second.loop;
  if (!AudioHAL::startCue(found->second.tune.c_str(), found->second.volume,
                         found->second.loop ? LOOP_MAX_MS : SOUND_MAX_MS, found->second.loop))
    Serial.printf("[Paced] Sound %s not played\n", name.c_str());
#else
  (void)name;
#endif
}

// `silence` (scene_volume1): the scene's sound stops; an alarm never does.
void silenceSound() {
#if defined(WIGNET_HAS_AUDIO)
  pendingSound.clear();
  if (AudioHAL::isPlaying() && !AudioHAL::isRinging()) AudioHAL::stop();
#endif
}

class PacedHost : public SceneRuntime::Host {
public:
  void page(int number) override {
    wantedPage = number;
    requested++;
    // Not yet on screen: the scene's time stands still until it is.
    if (pathOf(number) != shownFile()) freeze();
  }
  // Refused on a paced screen by the platform's validator: never asked.
  void state(int, const std::string &) override {}
  void move(int, int) override {}
  void wander(int, bool) override {}
  void show(int, bool) override {}
  // The sounds' volume (scene_volume1): pictures never bind on a paced screen.
  void prop(SceneRuntime::Target target, SceneRuntime::Prop prop, float value) override {
#if defined(WIGNET_HAS_AUDIO)
    if (target.kind == 's' && prop == SceneRuntime::VOLUME) AudioHAL::setSceneVolume(value);
#else
    (void)target;
    (void)prop;
    (void)value;
#endif
  }
  void sound(const std::string &name) override { playSound(name); }
  void silence() override { silenceSound(); }
  void report(const std::string &value, const std::string &event, int page) override {
    if (reports.size() < REPORTS_MAX) {
      Report report;
      report.value = value;
      report.event = event;
      report.page = page;
      reports.push_back(report);
    }
    Serial.printf("[Paced] Report \"%s\"\n", value.c_str());
  }
  void end() override { stopReason = "the scene ended itself"; }
};
PacedHost host;

void dispatch(const std::string &event, bool input) {
  if (!scene || stopReason.length()) return;
  Serial.printf("[Paced] %s%s\n", event.c_str(), scene->runtime.listens(event) ? "" : " (not in the scene)");
  scene->runtime.dispatch(event, input, sceneNow());
}

// ---------------------------------------------------------------- buttons --

struct Button {
  const char *name;
  int pin;
  bool raw, down, consumed;
  uint32_t since;
};
Button buttons[] = {
    {"a", 0, false, false, false, 0}, // BOOT
#if defined(WIGNET_BUTTON_KEY) && WIGNET_BUTTON_KEY >= 0
    {"b", WIGNET_BUTTON_KEY, false, false, false, 0}, // KEY
#endif
};
uint32_t soundEndedMs = 0;

bool soundTakesPress() {
#if defined(WIGNET_HAS_AUDIO)
  if (AudioHAL::isRinging()) {
    soundEndedMs = millis();
    return true;
  }
  return soundEndedMs && millis() - soundEndedMs < AFTER_SOUND_MS;
#else
  return false;
#endif
}

// Under the lock, a scene playing. A level counts once two reads 10 ms apart agree.
void pollButtons() {
  const uint32_t now = millis();
  for (Button &button : buttons) {
    const bool raw = digitalRead(button.pin) == LOW;
    const bool steady = raw == button.raw;
    button.raw = raw;
    if (!steady) continue;
    const bool pressed = raw;
    if (pressed && !button.down) {
      button.down = true;
      button.since = now;
      // A ringing alarm takes the press to stop it: never the scene's.
      button.consumed = soundTakesPress();
      if (!button.consumed) dispatch(std::string(button.name) + ".down", true);
    } else if (pressed && !button.consumed && now - button.since >= EXIT_HOLD_MS) {
      button.consumed = true;
      Serial.printf("[Paced] %s held 5 s\n", button.name);
      stopReason = "a 5 s hold";
      restartWanted = scene->slide;
    } else if (!pressed && button.down) {
      button.down = false;
      if (button.consumed) continue;
      const uint32_t held = now - button.since;
      dispatch(std::string(button.name) + ".up", true);
      dispatch(std::string(button.name) + (held < SHORT_MAX_MS ? ".short" : ".long"), true);
    }
  }
}

void sceneTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    Locked held;
    if (!scene || stopReason.length()) {
      for (Button &button : buttons) button.down = button.raw = false;
      continue;
    }
    pollButtons();
    // The page asked for is on screen (or the scene asked for the one shown):
    // its time runs again from here.
    if (frozen && !drawing && pathOf(wantedPage) == shownFile()) thaw();
#if defined(WIGNET_HAS_AUDIO)
    if (!pendingSound.empty() && !AudioHAL::isPlaying()) playSound(pendingSound);
#endif
    scene->runtime.tick(sceneNow());
  }
}

// ------------------------------------------------------------- download --

// What /schedule last said, applied by service() from the loop.
String wantedId, wantedUrl;
bool wantedSlide = false;
String endedId;   // ended here (`end`, a hold): not played again while named
String refusedId; // a bundle this board cannot play: not asked for again while named
// Both kept across deep sleep, in RTC memory: a board that sleeps once its
// scene ended (the 7.5") does not play it again when it wakes.
RTC_DATA_ATTR char endedRtc[48];
RTC_DATA_ATTR char refusedRtc[48];

void remember(String &id, char *rtc, const String &value) {
  if (id == value) return;
  id = value;
  strlcpy(rtc, value.c_str(), 48);
}
uint32_t retryAtMs = 0;
int lastEventSeq = 0;
std::vector<std::string> pendingEvents;

String baseUrl() { return String("http://") + WIGNET_HOST + ":" + String(WIGNET_PORT); }

bool getText(const String &url, String &text) {
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setTimeout(15000);
  const int code = http.GET();
  if (code == HTTP_CODE_OK) text = http.getString();
  else Serial.printf("[Paced] GET %s: %d\n", url.c_str(), code);
  http.end();
  return code == HTTP_CODE_OK && text.length();
}

String hexOf(const uint8_t digest[32]) {
  char out[65];
  for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", digest[i]);
  return String(out);
}

// A page file of `bytes` bytes into `path`, checked against its sha256.
bool download(const String &url, const String &path, size_t bytes, const String &sha) {
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setTimeout(15000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[Paced] GET %s: %d\n", url.c_str(), code);
    http.end();
    return false;
  }
  const String part = path + ".part";
  File file = LittleFS.open(part, "w");
  if (!file) {
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  uint8_t buffer[1024];
  size_t got = 0;
  bool written = true;
  uint32_t last = millis();
  while (got < bytes && written && millis() - last < 15000) {
    const int available = stream->available();
    if (available <= 0) {
      if (!http.connected()) break;
      delay(2);
      continue;
    }
    const size_t n = stream->readBytes(buffer, min(sizeof(buffer), min(bytes - got, static_cast<size_t>(available))));
    mbedtls_sha256_update(&ctx, buffer, n);
    written = file.write(buffer, n) == n;
    got += n;
    last = millis();
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  file.close();
  http.end();
  const bool ok = written && got == bytes && hexOf(digest) == sha;
  if (!ok) {
    Serial.printf("[Paced] %s: %u of %u B%s\n", url.c_str(), static_cast<unsigned>(got),
                  static_cast<unsigned>(bytes), written ? (got == bytes ? ", wrong sha256" : "") : ", flash full");
    LittleFS.remove(part);
    return false;
  }
  LittleFS.remove(path);
  return LittleFS.rename(part, path);
}

// Every file in DIR that `keep` does not name.
void removeOthers(const std::vector<String> &keep) {
  std::vector<String> gone;
  File dir = LittleFS.open(DIR);
  if (!dir) return;
  for (File file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const String path = String(DIR) + "/" + file.name();
    bool kept = false;
    for (const String &k : keep) kept |= k == path;
    if (!kept) gone.push_back(path);
  }
  dir.close();
  for (const String &path : gone) LittleFS.remove(path);
}

enum class Loaded { Ok, Retry, Refused };

// The manifest and every page, checked, into a Scene not yet playing.
// Network and flash only: it runs from the loop without the lock.
Loaded load(const String &id, const String &url, std::unique_ptr<Scene> &out) {
  const String mac = WiFi.macAddress();
  String text;
  if (!getText(baseUrl() + url + "?mac=" + mac, text)) return Loaded::Retry;
  JsonDocument doc;
  if (deserializeJson(doc, text)) {
    Serial.println("[Paced] Manifest: invalid JSON");
    return Loaded::Refused;
  }
  const String format = doc["format"] | "";
#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
  const bool drawable = format == "wgn6" || format == "bmp1";
#else
  const bool drawable = format == "bmp1";
#endif
  if ((doc["v"] | 0) != 2 || (doc["paced"] | 0) != 1 || !drawable) {
    Serial.printf("[Paced] Manifest v%d, paced %d, format %s: not played here\n", doc["v"] | 0,
                  doc["paced"] | 0, format.c_str());
    return Loaded::Refused;
  }
  std::unique_ptr<Scene> next(new Scene());
  next->id = id;
  next->start = doc["start"] | 1;
  std::string error;
  if (!next->runtime.load(doc.as<JsonObjectConst>(), error)) {
    Serial.printf("[Paced] Runtime: %s: not played\n", error.c_str());
    return Loaded::Refused;
  }
  for (JsonPairConst item : doc["sounds"].as<JsonObjectConst>()) {
    Sound sound;
    sound.tune = item.value()["tune"] | "";
    sound.volume = item.value()["volume"] | 50;
    sound.loop = item.value()["loop"] | false;
    next->sounds[item.key().c_str()] = sound;
  }
  // Each page by its sha256: a page already in flash (the same scene again,
  // after a reboot, or another scene's identical page) is not downloaded.
  struct Wanted {
    String file, path, sha;
    size_t bytes;
  };
  std::vector<Wanted> missing;
  std::vector<String> keep;
  size_t needed = 0;
  for (JsonObjectConst page : doc["pages"].as<JsonArrayConst>()) {
    Wanted w;
    w.file = page["file"] | "";
    w.sha = page["sha256"] | "";
    w.bytes = page["bytes"] | 0;
    if (w.sha.length() != 64 || !w.file.length() || !w.bytes) {
      Serial.println("[Paced] Manifest: a page without its file, size or sha256");
      return Loaded::Refused;
    }
    w.path = String(DIR) + "/" + w.sha.substring(0, 20) + ".bin";
    next->pages[page["page"] | 0] = w.path;
    keep.push_back(w.path);
    File have = LittleFS.open(w.path);
    const bool present = have && have.size() == w.bytes;
    if (have) have.close();
    if (!present) {
      missing.push_back(w);
      needed += w.bytes;
    }
  }
  if (!next->pages.count(next->start)) {
    Serial.println("[Paced] Manifest: no start page");
    return Loaded::Refused;
  }
  // The pages of the scene before go, unless this one has them too.
  removeOthers(keep);
  const size_t free = LittleFS.totalBytes() - LittleFS.usedBytes();
  if (needed + FLASH_SPARE > free) {
    Serial.printf("[Paced] %u B of pages, %u B of flash free: not played\n", static_cast<unsigned>(needed),
                  static_cast<unsigned>(free));
    return Loaded::Refused;
  }
  for (const Wanted &w : missing)
    if (!download(baseUrl() + "/scene/" + id + "/" + w.file + "?mac=" + mac, w.path, w.bytes, w.sha))
      return Loaded::Retry;
  Serial.printf("[Paced] Loaded %s: %u pages (%u downloaded, %u B), %u B of flash free\n", id.c_str(),
                static_cast<unsigned>(next->pages.size()), static_cast<unsigned>(missing.size()),
                static_cast<unsigned>(needed),
                static_cast<unsigned>(LittleFS.totalBytes() - LittleFS.usedBytes()));
  out = std::move(next);
  return Loaded::Ok;
}

// ----------------------------------------------------------- on and off --
// Both under the lock.

// keepSound: a scene that ends itself lets its last sound finish (a "bye"
// beep), unless that sound loops.
void takeDown(bool keepSound = false) {
  if (!scene) return;
  // One of its other pages on screen: the start page, what /pic shows, goes back.
  const String shown = shownFile();
  const String start = pathOf(scene->start);
  restorePath = shown.length() && shown != start ? start : "";
#if defined(WIGNET_HAS_AUDIO)
  pendingSound.clear();
  if (!keepSound || soundLoops) {
    if (AudioHAL::isPlaying() && !AudioHAL::isRinging()) AudioHAL::stop(); // its cue goes with it
  }
#endif
  // The file stands for what is shown: /pic again for the start page.
  if (shownPath == start) shownPath = "";
  scene.reset();
  stopReason = "";
  frozen = false;
}

void putUp(std::unique_ptr<Scene> next) {
  takeDown();
#if defined(WIGNET_HAS_AUDIO)
  AudioHAL::setSceneVolume(100); // until the scene binds its own
#endif
  scene = std::move(next);
  restorePath = "";
  scene->runtime.setHost(&host);
  scene->runtime.setSeed(esp_random());
  scene->runtime.setSlide(scene->slide);
  // The loop drew /pic, the start page: the scene starts on it. Anything
  // else on screen (a page of the scene before) is replaced once the lock allows.
  wantedPage = scene->start;
  requested = 0;
  frozen = false;
  scene->runtime.start(sceneNow());
  requested = 0;
  if (pathOf(scene->start) != shownFile()) freeze();
  syncClock();
  Serial.printf("[Paced] Playing %s from page %d\n", scene->id.c_str(), scene->start);
}

bool lockOver() { return !drawing && (!lockSet || static_cast<int32_t>(millis() - lockUntilMs) >= 0); }

// From the loop, without the lock: blocks for the refresh.
void draw(const String &path, int page) {
  File file = LittleFS.open(path);
  if (!file) {
    Serial.printf("[Paced] %s missing\n", path.c_str());
    Locked held;
    if (scene) stopReason = "a page missing from flash";
    restorePath = "";
    return;
  }
  int collapsed = 0;
  {
    Locked held;
    collapsed = requested > 1 ? requested - 1 : 0;
    requested = 0;
  }
  drawing = true;
  const uint32_t started = millis();
  const bool ok = hooks.draw(file, file.size());
  const uint32_t took = millis() - started;
  file.close();
  {
    Locked held;
    lockUntilMs = millis() + HOLD_MS;
    lockSet = true;
    shownPath = ok ? path : "";
    drawing = false;
    if (!ok && scene) stopReason = "a page that could not be drawn";
  }
  if (ok)
    Serial.printf("[Paced] Page %d drawn in %lu ms; screen locked %lu ms more%s\n", page,
                  static_cast<unsigned long>(took), static_cast<unsigned long>(HOLD_MS),
                  collapsed ? (String("; ") + collapsed + " page request(s) collapsed").c_str() : "");
  else LittleFS.remove(path);
}

bool filesystemReady = false;

} // namespace

namespace PacedPlayer {

void begin(const Hooks &given) {
  hooks = given;
  lock = xSemaphoreCreateRecursiveMutex();
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
    endedRtc[47] = refusedRtc[47] = 0;
    endedId = endedRtc;
    refusedId = refusedRtc;
  } else {
    endedRtc[0] = refusedRtc[0] = 0;
  }
#if defined(WIGNET_HAS_AUDIO)
  // The alarm's sound lives here too: never format this partition.
  filesystemReady = LittleFS.begin(false);
#else
  filesystemReady = LittleFS.begin(true);
#endif
  if (filesystemReady && !LittleFS.exists(DIR)) LittleFS.mkdir(DIR);
  if (!filesystemReady) {
    Serial.println("[Paced] No filesystem: scenes off");
    return;
  }
  for (Button &button : buttons) pinMode(button.pin, INPUT_PULLUP);
  xTaskCreatePinnedToCore(sceneTask, "paced", 8192, nullptr, 2, nullptr, 1);
  Serial.printf("[Paced] Player ready (runtime 2), %u B of flash free\n",
                static_cast<unsigned>(LittleFS.totalBytes() - LittleFS.usedBytes()));
}

String scheduleParams() {
  String out = "&in=a";
#if defined(WIGNET_BUTTON_KEY) && WIGNET_BUTTON_KEY >= 0
  out += ",b";
#endif
  if (lastEventSeq > 0) out += "&sev=" + String(lastEventSeq);
  return out;
}

void onSchedule(const String &payload) {
  if (!lock) return;
  JsonDocument filter;
  filter["scene"] = true;
  filter["sceneEvents"] = true;
  filter["serverEpoch"] = true;
  filter["utcOffsetSec"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) return;
  Locked held;
  const uint32_t epoch = doc["serverEpoch"] | 0;
  if (epoch > 1700000000UL) {
    clockEpoch = epoch;
    clockMillis = millis();
    clockOffset = doc["utcOffsetSec"] | 0;
    clockFresh = true;
  }
  wantedId = doc["scene"]["id"] | "";
  wantedUrl = doc["scene"]["url"] | "";
  wantedSlide = doc["scene"]["slide"] | false;
  for (JsonObjectConst event : doc["sceneEvents"].as<JsonArrayConst>()) {
    const int seq = event["seq"] | 0;
    if (seq <= lastEventSeq) continue;
    lastEventSeq = seq;
    pendingEvents.push_back(std::string("event:") + (event["name"] | ""));
  }
}

void service() {
  if (!lock) return;
  const bool awake = !hooks.awake || hooks.awake();
  {
    Locked held;
    if (stopReason.length()) {
      Serial.printf("[Paced] Ended by %s%s\n", stopReason.c_str(),
                    restartWanted ? ": starting the slide again" : "");
      // A slide is not remembered as ended: the next pass loads it again.
      const bool ended = scene && !restartWanted;
      const bool pushed = ended && !scene->slide;
      if (ended) remember(endedId, endedRtc, scene->id);
      if (pushed) endedToTell = scene->id;
      restartWanted = false;
      takeDown(ended);
      if (pushed) {
        restoreUnlessEnded = restorePath;
        restorePath = "";
      }
    }
    if (scene && wantedId != scene->id) {
      Serial.println("[Paced] No longer named by /schedule");
      takeDown();
    }
    if (scene && !awake) {
      // On battery: the start page, inert, until the board stays awake again.
      Serial.println("[Paced] Asleep between syncs: the scene waits on its start page");
      takeDown();
    }
    if (wantedId != endedId) remember(endedId, endedRtc, "");
    if (wantedId != refusedId) remember(refusedId, refusedRtc, "");
  }
  if (filesystemReady && awake && WiFi.status() == WL_CONNECTED && wantedId.length() && !scene &&
      wantedId != endedId && wantedId != refusedId &&
      (!retryAtMs || static_cast<int32_t>(millis() - retryAtMs) >= 0)) {
    std::unique_ptr<Scene> next;
    const Loaded loaded = load(wantedId, wantedUrl, next);
    if (loaded == Loaded::Ok) {
      next->slide = wantedSlide;
      retryAtMs = 0;
      Locked held;
      putUp(std::move(next));
    } else if (loaded == Loaded::Refused) {
      remember(refusedId, refusedRtc, wantedId);
    } else {
      retryAtMs = millis() + RETRY_MS;
      Serial.println("[Paced] Not played; trying again in 30 s");
    }
  }
  {
    Locked held;
    if (scene && clockFresh && !frozen) syncClock();
    if (scene) clockFresh = false;
    for (const std::string &event : pendingEvents) dispatch(event, false);
    pendingEvents.clear();
  }
  // The page wanted, once the lock is over (rule 1): the last one asked for.
  String path;
  int page = 0;
  {
    Locked held;
    if (scene) {
      path = pathOf(wantedPage);
      page = wantedPage;
    } else {
      path = restorePath;
    }
    if (path == shownFile()) {
      path = "";
      if (!scene) restorePath = "";
    }
  }
  if (path.length() && lockOver()) {
    draw(path, page);
    Locked held;
    if (!scene && shownPath == path) restorePath = "";
  }
  std::vector<Report> outgoing;
  String id;
  bool ended = false;
  {
    Locked held;
    if (WiFi.status() != WL_CONNECTED) return;
    outgoing.swap(reports);
    if (scene) id = scene->id;
    if (endedToTell.length() && !scene) {
      id = endedToTell;
      ended = true;
      endedToTell = "";
    }
  }
  if ((outgoing.empty() && !ended) || id.isEmpty()) return;
  JsonDocument body;
  body["publication_id"] = id;
  if (ended) body["ended"] = true;
  JsonArray events = body["events"].to<JsonArray>();
  for (const Report &report : outgoing) {
    JsonObject item = events.add<JsonObject>();
    item["value"] = report.value;
    if (!report.event.empty()) item["event"] = report.event;
    if (report.page) item["page"] = report.page;
  }
  String json;
  serializeJson(body, json);
  WiFiClient client;
  HTTPClient http;
  if (http.begin(client, baseUrl() + "/input?mac=" + WiFi.macAddress())) {
    http.addHeader("Content-Type", "application/json");
    const int code = http.POST(json);
    bool pushEnded = false;
    if (ended && code == 200) {
      JsonDocument answer;
      if (!deserializeJson(answer, http.getString())) pushEnded = answer["push_ended"] | false;
    }
    Serial.printf("[Paced] Sent %u report(s)%s: %d\n", static_cast<unsigned>(outgoing.size()),
                  ended ? (pushEnded ? ", ended: the push is over" : ", ended") : "", code);
    http.end();
    if (ended) {
      Locked held;
      // Not ended by the server (a slide, an older server, no answer): the
      // start page, what /pic still serves, goes back on screen.
      if (!pushEnded && !scene && restoreUnlessEnded.length()) restorePath = restoreUnlessEnded;
      restoreUnlessEnded = "";
    }
  } else if (ended) {
    Locked held;
    if (!scene && restoreUnlessEnded.length()) restorePath = restoreUnlessEnded;
    restoreUnlessEnded = "";
  }
}

void onRedraw(uint32_t startedMs) {
  if (!lock) return;
  Locked held;
  lockUntilMs = millis() + HOLD_MS;
  lockSet = true;
  shownPath = "";
  restorePath = "";
  Serial.printf("[Paced] Screen drawn by the loop in %lu ms; locked %lu ms more\n",
                static_cast<unsigned long>(millis() - startedMs), static_cast<unsigned long>(HOLD_MS));
}

bool screenLocked() { return lock && !lockOver(); }

bool keepsAwake() {
  if (!lock || !filesystemReady) return false;
  Locked held;
  // Still up, or its start page still to put back before sleeping.
  if (scene || restorePath.length() || endedToTell.length() || restoreUnlessEnded.length()) return true;
  return wantedId.length() && wantedId != endedId && wantedId != refusedId && (!hooks.awake || hooks.awake());
}

bool playing() { return scene != nullptr; }

} // namespace PacedPlayer

#endif // WIGNET_PACED_PLAYER
