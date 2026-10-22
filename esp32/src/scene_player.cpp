/**
 * Scene player (scene_player.h, docs/scenes.md, docs/scene_runtime.md).
 *
 * The server keeps every pixel: backgrounds arrive as RGB565, pictures and
 * sprite sheets as RGB565A8, already drawn. What happens when is the scene
 * runtime's (scene_runtime.cpp): its variables, bindings, guarded actions and
 * timers. This file is its host: LVGL places, swaps, turns and moves the
 * pictures, the buttons and the tilt reach the runtime, its sounds and tone
 * play. Its flush goes through DisplayHAL::pushRegion(), the same turn and
 * colour calibration as every page. lv_timer_handler runs in its own task;
 * anything that touches LVGL or the runtime from the loop takes `lock` first.
 */
#include "scene_player.h"

#if defined(WIGNET_SCENE_PLAYER)

#include "board.h"
#include "config.h"
#include "hal/display_hal.h"
#include "orientation_hal.h"
#include "power_hold_hal.h"
#include "scene_runtime.h"
#if defined(WIGNET_HAS_AUDIO)
#include "audio_hal.h"
#endif
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp32s3/rom/miniz.h> // tinfl, in ROM: compressed assets cost no flash
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <lvgl.h>
#include <mbedtls/sha256.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

using SceneRuntime::Prop;
using SceneRuntime::Target;

constexpr int BUFFER_LINES = 40;
constexpr uint32_t SHORT_MAX_MS = 600;    // shorter: short; longer: long
constexpr uint32_t HOLD_MS = 5000;        // the way out of a scene, never the scene's
constexpr uint32_t AFTER_SOUND_MS = 1000; // a press just after a ring is still the ring's
constexpr size_t REPORTS_MAX = 32;
constexpr size_t BUNDLE_MAX_BYTES = 3000000; // held in PSRAM, raw (scenes.LIMITS bundle_bytes)

SemaphoreHandle_t lock = nullptr;
struct Locked {
  Locked() { xSemaphoreTakeRecursive(lock, portMAX_DELAY); }
  ~Locked() { xSemaphoreGiveRecursive(lock); }
};

// ------------------------------------------------------------- the scene --

struct Asset {
  uint8_t *data = nullptr;
  size_t bytes = 0;
  int w = 0, h = 0, frames = 1;
  bool alpha = false;
  bool indexed = false; // i4: per frame a 16-entry ARGB8888 palette, then 4-bit indices
  std::vector<lv_image_dsc_t> dsc;
  ~Asset() {
    if (data) heap_caps_free(data);
  }
};

struct State {
  std::vector<const void *> frames;
  uint32_t ms = 200, forMs = 0;
  std::string then;
};

// img, sprite, rect or group (a container: the cells of a glyphs shorthand).
struct Obj {
  int id = 0, page = 0, parent = 0, x = 0, y = 0;
  int moved = -1; // the last `move` it took (its ordinal in the scene), -1: at its own place
  bool sprite = false, rect = false, group = false, hidden = false;
  bool framed = false; // a sprite whose frame is bound: one picture of its sheet at a time
  Asset *asset = nullptr;
  std::map<std::string, State> states;
  std::string state;
  bool wanders = false, wandering = false;
  int wx0 = 0, wy0 = 0, wx1 = 0, wy1 = 0, speed = 30;
  int tx = 0, ty = 0; // where its current move goes (top-left)
  // do.avoid: the part of its frame that meets others.
  bool avoids = false;
  int ax = 0, ay = 0, aw = 0, ah = 0;
  int w = 0, h = 0;      // a rect's or a group's size
  uint32_t color = 0;    // a rect's
  float dx = 0, dy = 0;  // bound offsets, in design units
  lv_obj_t *o = nullptr;
  lv_timer_t *timed = nullptr; // a state with `for`
};

struct Page {
  int number = 0;
  Asset *bg = nullptr; // or
  lv_obj_t *bgImage = nullptr;
  uint32_t color = 0; // 0xRRGGBB
  lv_obj_t *o = nullptr;
};

struct Report {
  std::string value, event;
  int page = 0;
};

struct Sound {
  std::string tune;
  uint8_t volume = 50;
};

// A tone: its pitch (a MIDI note) and on/off are bound (docs/scene_runtime.md).
struct Tone {
  bool present = false;
  uint8_t volume = 50;
  uint32_t glideMs = 60;
#if defined(WIGNET_HAS_AUDIO)
  AudioHAL::Wave wave = AudioHAL::Wave::Triangle;
#endif
};

// Where everything sits on one page shape (manifest `layouts`): the board
// picks the one for how it is held and moves the scene there itself.
struct Layout {
  int w = 0, h = 0;
  std::map<int, std::pair<int, int>> objects; // id: x, y
  std::map<int, std::vector<int>> wander;     // id: x0, y0, x1, y1
  std::vector<std::pair<int, int>> moves;     // by ordinal
  std::map<int, Asset *> bg;                  // page: its background for this shape
};

struct Scene {
  String id;
  int start = 1;
  // The page as laid out for how the board is held (manifest w, h, turn): a
  // board that reports it (orientation_report1, the 2") gets a portrait page
  // turned a quarter into its landscape frame.
  int w = 0, h = 0, turn = 0;
  float scale = 1; // screen pixels per design unit, for bound dx, dy, w, h
  // A slide (/schedule scene.slide): the slideshow sets the pace, so `end`
  // does nothing and a 5 s hold starts it again (issue #244).
  bool slide = false;
  std::map<std::string, Sound> sounds;
  Tone tone;
  bool followsTilt = false; // a binding or a when reads it, or a motion event: read the tilt
  std::map<std::string, std::unique_ptr<Asset>> assets;
  std::vector<Page> pages;
  std::vector<std::unique_ptr<Obj>> objects;
  std::vector<std::pair<int, int>> moves; // each move's target, by ordinal
  // "landscape"/"portrait" (a square page has one), and the one in use;
  // empty for a bundle laid out for one page only.
  std::map<std::string, Layout> layouts;
  const Layout *layout = nullptr;
  SceneRuntime::Runtime runtime;
};

std::unique_ptr<Scene> scene; // the one on screen
lv_display_t *display = nullptr;
volatile int pageTurn = 0; // the playing scene's turn, for flush()
// How the board is held (OrientationHAL), from onTurn(): picks the layout.
volatile int heldTurn = 0;
int currentPage = 0;
std::vector<Report> reports;
String stopReason; // set in the LVGL task, acted on by service()
bool restartWanted = false; // with stopReason: a slide's hold, played again from its first page
// A `sound` asked while another cue plays: it stops that one and waits here
// for the audio task to end (playCues). Empty: none.
std::string pendingSound;
constexpr uint32_t SOUND_MAX_MS = 10000;
// The tone, as its bindings set it.
bool toneWanted = false;
float toneHz = 261.63f;

// The board's clock: serverEpoch and utcOffsetSec from the last /schedule,
// carried forward with millis(). 0: not known yet. clockFresh: not yet
// given to the scene playing.
uint32_t clockEpoch = 0, clockMillis = 0;
int32_t clockOffset = 0;
bool clockFresh = false;

// What /schedule last said, applied by service() from the loop.
String wantedId, wantedUrl;
bool wantedSlide = false;
String endedId; // ended here (`end`, a hold): not played again while named
uint32_t retryAtMs = 0; // after a failed download
constexpr uint32_t RETRY_MS = 30000;
int lastEventSeq = 0;
std::vector<std::string> pendingEvents;

String baseUrl() { return String("http://") + WIGNET_HOST + ":" + String(WIGNET_PORT); }

Obj *find(int id) {
  if (!scene) return nullptr;
  for (auto &obj : scene->objects)
    if (obj->id == id) return obj.get();
  return nullptr;
}

Obj *findByLv(const void *lv) {
  if (!scene || !lv) return nullptr;
  for (auto &obj : scene->objects)
    if (obj->o == lv) return obj.get();
  return nullptr;
}

// Under the lock (an LVGL timer, or the runtime).
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
  if (!AudioHAL::startCue(found->second.tune.c_str(), found->second.volume, SOUND_MAX_MS))
    Serial.printf("[Scene] Sound %s not played\n", name.c_str());
#else
  (void)name;
#endif
}

void playCues(lv_timer_t *) {
#if defined(WIGNET_HAS_AUDIO)
  if (scene && !pendingSound.empty() && !AudioHAL::isPlaying()) playSound(pendingSound);
#endif
}

void stopTone() {
#if defined(WIGNET_HAS_AUDIO)
  if (AudioHAL::isPlaying() && !AudioHAL::isRinging() && pendingSound.empty()) AudioHAL::stop();
#endif
}

// ---------------------------------------------------------------- layouts --

// The turn of the page into the frame for how the board is held: a board
// that turns its whole frame itself (self_orient1, the square 1.54") draws
// the page at 0; one that reports it (the 2") turns the page.
int pageTurnFor(int held) {
#if defined(WIGNET_SELF_ORIENT)
  (void)held;
  return 0;
#else
  return held;
#endif
}

// The layout for the page the board has when held at `held`, or nullptr.
const Layout *layoutFor(const Scene &s, int held) {
  const int turn = pageTurnFor(held);
  const bool sideways = turn == 90 || turn == 270;
  const int w = sideways ? DisplayHAL::getHeight() : DisplayHAL::getWidth();
  const int h = sideways ? DisplayHAL::getWidth() : DisplayHAL::getHeight();
  for (const auto &item : s.layouts)
    if (item.second.w == w && item.second.h == h) return &item.second;
  return nullptr;
}

// The scene's places, backgrounds and moves from `layout`: data only, no LVGL.
void applyLayout(Scene &s, const Layout &layout, int turn) {
  s.layout = &layout;
  s.w = layout.w;
  s.h = layout.h;
  s.turn = turn;
  for (Page &page : s.pages) {
    auto bg = layout.bg.find(page.number);
    if (bg != layout.bg.end()) page.bg = bg->second;
  }
  for (auto &obj : s.objects) {
    auto at = layout.objects.find(obj->id);
    if (at != layout.objects.end()) {
      obj->x = at->second.first;
      obj->y = at->second.second;
    }
    auto box = layout.wander.find(obj->id);
    if (box != layout.wander.end() && box->second.size() == 4) {
      obj->wx0 = box->second[0];
      obj->wy0 = box->second[1];
      obj->wx1 = box->second[2];
      obj->wy1 = box->second[3];
    }
  }
  if (layout.moves.size() == s.moves.size()) s.moves = layout.moves;
}

// ---------------------------------------------------------------- display --

void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
  DisplayHAL::pushRegion(reinterpret_cast<const uint16_t *>(pixels), area->x1, area->y1,
                         lv_area_get_width(area), lv_area_get_height(area), pageTurn);
  lv_display_flush_ready(display);
}

#if defined(WIGNET_DRAW_TIMING)
// Bench only: how long each refresh of the display takes, drawing and pushing
// to the panel, logged every 5 s (docs/scene_runtime.md "Indexed pictures").
uint32_t refreshStartUs = 0, refreshCount = 0, refreshTotalUs = 0, refreshMaxUs = 0, refreshLogMs = 0;

void onRefresh(lv_event_t *event) {
  if (lv_event_get_code(event) == LV_EVENT_REFR_START) {
    refreshStartUs = micros();
    return;
  }
  const uint32_t took = micros() - refreshStartUs;
  refreshCount++;
  refreshTotalUs += took;
  if (took > refreshMaxUs) refreshMaxUs = took;
  if (millis() - refreshLogMs >= 5000) {
    Serial.printf("[Draw] %u refreshes, %.2f ms on average, %.2f ms at most; %u B of PSRAM free\n",
                  static_cast<unsigned>(refreshCount), refreshCount ? refreshTotalUs / 1000.0 / refreshCount : 0.0,
                  refreshMaxUs / 1000.0, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    refreshCount = refreshTotalUs = refreshMaxUs = 0;
    refreshLogMs = millis();
  }
}
#endif

void lvglTask(void *) {
  for (;;) {
    uint32_t next;
    {
      Locked held;
      next = lv_timer_handler();
    }
    if (next == LV_NO_TIMER_READY || next > LV_DEF_REFR_PERIOD) next = LV_DEF_REFR_PERIOD;
    vTaskDelay(pdMS_TO_TICKS(max<uint32_t>(next, 2)));
  }
}

// ------------------------------------------------------------- movement --

void setX(void *o, int32_t v) { lv_obj_set_x(static_cast<lv_obj_t *>(o), v); }
void setY(void *o, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t *>(o), v); }

void stopMoving(Obj *obj) {
  lv_anim_delete(obj->o, setX);
  lv_anim_delete(obj->o, setY);
}

void wanderNext(lv_anim_t *a);

void moveTo(Obj *obj, int x, int y, uint32_t delayMs) {
  obj->tx = x;
  obj->ty = y;
  const int fromX = lv_obj_get_x(obj->o), fromY = lv_obj_get_y(obj->o);
  const float distance = sqrtf(float((x - fromX) * (x - fromX) + (y - fromY) * (y - fromY)));
  const uint32_t duration = max<uint32_t>(200, distance * 1000 / max(1, obj->speed));
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj->o);
  lv_anim_set_duration(&a, duration);
  lv_anim_set_delay(&a, delayMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&a, setY);
  lv_anim_set_values(&a, fromY, y);
  lv_anim_start(&a);
  lv_anim_set_exec_cb(&a, setX);
  lv_anim_set_values(&a, fromX, x);
  // Arriving: go on wandering if it still does.
  lv_anim_set_completed_cb(&a, wanderNext);
  lv_anim_start(&a);
}

// A random point of the area, the sprite's top-left kept so it stays inside.
void wanderTo(Obj *obj) {
  const int x1 = max(obj->wx0, obj->wx1 - static_cast<int>(obj->asset->w));
  const int y1 = max(obj->wy0, obj->wy1 - static_cast<int>(obj->asset->h));
  moveTo(obj, lv_rand(obj->wx0, x1), lv_rand(obj->wy0, y1), lv_rand(200, 1500));
}

// Off to a random point of its area away from (ux, uy), which points back at
// what it met: the first of a few random picks within 60 degrees of
// straight away, else the one that goes most away. A point merely not
// towards it (nearly sideways) would meet it again a moment later.
void wanderAway(Obj *obj, long ux, long uy) {
  const int fromX = lv_obj_get_x(obj->o), fromY = lv_obj_get_y(obj->o);
  const int x1 = max(obj->wx0, obj->wx1 - static_cast<int>(obj->asset->w));
  const int y1 = max(obj->wy0, obj->wy1 - static_cast<int>(obj->asset->h));
  const float u = sqrtf(float(ux) * ux + float(uy) * uy);
  int toX = fromX, toY = fromY;
  float best = -2;
  for (int i = 0; i < 12; i++) {
    const int x = lv_rand(obj->wx0, x1), y = lv_rand(obj->wy0, y1);
    const float d = sqrtf(float(x - fromX) * (x - fromX) + float(y - fromY) * (y - fromY));
    if (d < 1 || u < 1) continue;
    const float away = ((x - fromX) * ux + (y - fromY) * uy) / (d * u); // cosine
    if (away > best) {
      best = away;
      toX = x;
      toY = y;
    }
    if (away >= 0.5f) break;
  }
  moveTo(obj, toX, toY, 0);
}

bool shown(const Obj *obj) { return obj->page == currentPage && !lv_obj_has_flag(obj->o, LV_OBJ_FLAG_HIDDEN); }

// A wandering sprite whose box meets other avoiding sprites while it heads
// towards them (their centres summed, so one caught between two turns away
// from both, not from one into the other) turns away (docs/scenes.md
// "Sprites that avoid each other"). One already heading away is left alone,
// so sprites that overlap part instead of shaking.
void tickAvoid(lv_timer_t *) {
  if (!scene) return;
  for (auto &a : scene->objects) {
    if (!a->avoids || !a->wandering || !shown(a.get())) continue;
    const int x = lv_obj_get_x(a->o), y = lv_obj_get_y(a->o);
    const int ax = x + a->ax, ay = y + a->ay;
    long towardX = 0, towardY = 0;
    int met = 0, first = 0;
    for (auto &b : scene->objects) {
      if (b.get() == a.get() || !b->avoids || !shown(b.get())) continue;
      const int bx = lv_obj_get_x(b->o) + b->ax, by = lv_obj_get_y(b->o) + b->ay;
      if (ax >= bx + b->aw || bx >= ax + a->aw || ay >= by + b->ah || by >= ay + a->ah) continue;
      towardX += (bx + b->aw / 2) - (ax + a->aw / 2);
      towardY += (by + b->ah / 2) - (ay + a->ah / 2);
      if (!met++) first = b->id;
    }
    if (!met) continue;
    const long headX = a->tx - x, headY = a->ty - y;
    if (!towardX && !towardY) {
      // Right on top of each other: away from where it was going.
      towardX = headX;
      towardY = headY;
    }
    if (headX * towardX + headY * towardY <= 0) continue;
    stopMoving(a.get());
    wanderAway(a.get(), -towardX, -towardY);
    Serial.printf("[Scene] %d met %d (%d in all) at (%d, %d): off to (%d, %d)\n", a->id, first, met, x, y, a->tx,
                  a->ty);
  }
}

void wanderNext(lv_anim_t *a) {
  Obj *obj = findByLv(a->var);
  if (obj && obj->wandering) wanderTo(obj);
}

void enterState(Obj *obj, const std::string &name);

void endTimed(lv_timer_t *timer) {
  Obj *obj = static_cast<Obj *>(lv_timer_get_user_data(timer));
  obj->timed = nullptr; // a single run: LVGL deletes it
  const std::string then = obj->states[obj->state].then;
  if (!then.empty()) enterState(obj, then);
}

void enterState(Obj *obj, const std::string &name) {
  auto found = obj->states.find(name);
  if (found == obj->states.end() || found->second.frames.empty()) return;
  if (obj->timed) {
    lv_timer_delete(obj->timed);
    obj->timed = nullptr;
  }
  obj->state = name;
  State &s = found->second;
  lv_animimg_set_src(obj->o, s.frames.data(), s.frames.size());
  lv_animimg_set_duration(obj->o, s.ms * s.frames.size());
  lv_animimg_set_repeat_count(obj->o, LV_ANIM_REPEAT_INFINITE);
  lv_animimg_start(obj->o);
  if (s.forMs) {
    obj->timed = lv_timer_create(endTimed, s.forMs, obj);
    lv_timer_set_repeat_count(obj->timed, 1);
  }
}

void showPage(int number) {
  for (Page &page : scene->pages) {
    if (page.number == number) lv_obj_remove_flag(page.o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(page.o, LV_OBJ_FLAG_HIDDEN);
  }
  currentPage = number;
  Serial.printf("[Scene] Page %d\n", number);
}

// Its place (its layout's, or its last move's target) plus its bound offset.
void place(Obj *obj) {
  int x = obj->x, y = obj->y;
  if (obj->moved >= 0 && obj->moved < static_cast<int>(scene->moves.size())) {
    x = scene->moves[obj->moved].first;
    y = scene->moves[obj->moved].second;
  }
  lv_obj_set_pos(obj->o, x + static_cast<int>(floorf(obj->dx * scene->scale + 0.5f)),
                 y + static_cast<int>(floorf(obj->dy * scene->scale + 0.5f)));
}

// ----------------------------------------------------------------- the host --
// What the runtime asks for, under the lock.

class PlayerHost : public SceneRuntime::Host {
public:
  void page(int number) override { showPage(number); }
  void state(int id, const std::string &name) override {
    if (Obj *obj = find(id)) enterState(obj, name);
  }
  void move(int id, int ordinal) override {
    Obj *obj = find(id);
    if (!obj || ordinal < 0 || ordinal >= static_cast<int>(scene->moves.size())) return;
    stopMoving(obj);
    obj->moved = ordinal; // where it goes back to after a turn
    moveTo(obj, scene->moves[ordinal].first, scene->moves[ordinal].second, 0);
  }
  void wander(int id, bool on) override {
    if (Obj *obj = find(id)) {
      stopMoving(obj);
      obj->moved = -1;
      obj->wandering = on && obj->wanders;
      if (obj->wandering) wanderTo(obj);
    }
  }
  void show(int id, bool visible) override {
    if (Obj *obj = find(id)) {
      if (visible) lv_obj_remove_flag(obj->o, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(obj->o, LV_OBJ_FLAG_HIDDEN);
    }
  }
  void sound(const std::string &name) override { playSound(name); }
  void report(const std::string &value, const std::string &event, int page) override {
    if (reports.size() < REPORTS_MAX) {
      Report report;
      report.value = value;
      report.event = event;
      report.page = page;
      reports.push_back(report);
    }
    Serial.printf("[Scene] Report \"%s\"\n", value.c_str());
  }
  void end() override {
    // Taken down by service(), outside LVGL's own timers. A slide's runtime
    // never asks: its `end` does nothing.
    stopReason = "the scene ended itself";
  }
  void prop(Target target, Prop prop, float value) override {
    if (target.kind == 't') {
      if (prop == SceneRuntime::PITCH) toneHz = 440.0f * powf(2.0f, (value - 69.0f) / 12.0f);
      if (prop == SceneRuntime::ON) {
        toneWanted = value != 0;
        if (!toneWanted) stopTone();
      }
      return;
    }
    if (target.kind == 'p') {
      for (Page &page : scene->pages)
        if (page.number == target.id && !page.bg) {
          const lv_color_t next = lv_color_hex(static_cast<uint32_t>(value) & 0xFFFFFF);
          // Redrawn only when the panel would show a change (RGB565).
          if (lv_color_to_u16(next) != lv_color_to_u16(lv_color_hex(page.color)))
            lv_obj_set_style_bg_color(page.o, next, 0);
          page.color = static_cast<uint32_t>(value) & 0xFFFFFF;
        }
      return;
    }
    Obj *obj = find(target.id);
    if (!obj) return;
    switch (prop) {
    case SceneRuntime::DX:
    case SceneRuntime::DY:
      (prop == SceneRuntime::DX ? obj->dx : obj->dy) = value;
      place(obj);
      break;
    case SceneRuntime::FRAME:
      if (obj->framed) {
        long frame = static_cast<long>(floorf(value)) % obj->asset->frames;
        if (frame < 0) frame += obj->asset->frames;
        lv_image_set_src(obj->o, &obj->asset->dsc[frame]);
      }
      break;
    case SceneRuntime::ROTATION: {
      long tenths = static_cast<long>(floorf(value * 10 + 0.5f)) % 3600;
      if (tenths < 0) tenths += 3600;
      if (!obj->rect && !obj->group) lv_image_set_rotation(obj->o, tenths);
      break;
    }
    case SceneRuntime::VISIBLE:
      if (value != 0) lv_obj_remove_flag(obj->o, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(obj->o, LV_OBJ_FLAG_HIDDEN);
      break;
    case SceneRuntime::OPACITY: {
      const float clamped = value < 0 ? 0 : value > 100 ? 100 : value;
      lv_obj_set_style_opa(obj->o, static_cast<lv_opa_t>(clamped * 255 / 100), 0);
      break;
    }
    case SceneRuntime::W:
    case SceneRuntime::H: {
      const int size = max(0, static_cast<int>(floorf(value * scene->scale + 0.5f)));
      if (prop == SceneRuntime::W) lv_obj_set_width(obj->o, size);
      else lv_obj_set_height(obj->o, size);
      break;
    }
    case SceneRuntime::COLOR:
      if (obj->rect) lv_obj_set_style_bg_color(obj->o, lv_color_hex(static_cast<uint32_t>(value) & 0xFFFFFF), 0);
      break;
    default:
      break;
    }
  }
};
PlayerHost host;

// The runtime's timers and clock, the tilt it reads and the motion events,
// and the tone its bindings set: every 20 ms.
struct Tilt {
  float x = 0, y = 0, z = 1, g = 1;
  bool known = false;
};

// The tilt, -1 to 1 in the upright page's frame: x is the page's right edge
// going down, y its top edge going down; full at 45 degrees. z: 1 face up,
// -1 face down.
Tilt readTilt() {
  Tilt tilt;
  float gx, gy, gz;
  if (!OrientationHAL::gravity(gx, gy, gz)) return tilt;
  // Downhill in the panel's frame (x right, y up) is against the up reading.
  const float dx = -gx, dy = -gy;
  // The page's right and up in the panel's frame, the page turned clockwise.
  float rx = 1, ry = 0, ux = 0, uy = 1;
  switch ((DisplayHAL::turn() + pageTurn) % 360) {
  case 90: rx = 0; ry = -1; ux = 1; uy = 0; break;
  case 180: rx = -1; ry = 0; ux = 0; uy = -1; break;
  case 270: rx = 0; ry = 1; ux = -1; uy = 0; break;
  default: break;
  }
  auto scaled = [](float v) { return constrain(v / 0.7071f, -1.0f, 1.0f); };
  tilt.x = scaled(dx * rx + dy * ry);
  tilt.y = scaled(dx * ux + dy * uy); // downhill toward the top: the top edge goes down
  tilt.z = gz;
  tilt.g = sqrtf(gx * gx + gy * gy + gz * gz);
  tilt.known = true;
  return tilt;
}

// Motion events fire once, then wait for the board to come back.
bool tiltLatched[4] = {}; // left, right, up, down
bool faceDownLatched = false;
uint32_t lastShakeMs = 0;

void dispatch(const std::string &event, bool input) {
  if (!scene || stopReason.length()) return;
  Serial.printf("[Scene] %s%s\n", event.c_str(), scene->runtime.listens(event) ? "" : " (not in the scene)");
  scene->runtime.dispatch(event, input, millis());
}

void tickScene(lv_timer_t *) {
  if (!scene || stopReason.length()) return;
  const uint32_t now = millis();
  scene->runtime.tick(now);
  if (scene->followsTilt) {
    const Tilt tilt = readTilt();
    if (tilt.known) {
      scene->runtime.setTilt(tilt.x, tilt.y, tilt.z, now);
      const char *names[4] = {"tilt.left", "tilt.right", "tilt.up", "tilt.down"};
      const float amounts[4] = {-tilt.x, tilt.x, -tilt.y, tilt.y};
      for (int i = 0; i < 4; i++) {
        if (!tiltLatched[i] && amounts[i] > 0.6f) {
          tiltLatched[i] = true;
          if (scene->runtime.listens(names[i])) dispatch(names[i], true);
        } else if (tiltLatched[i] && amounts[i] < 0.3f) {
          tiltLatched[i] = false;
        }
      }
      if (!faceDownLatched && tilt.z < -0.8f) {
        faceDownLatched = true;
        if (scene->runtime.listens("face_down")) dispatch("face_down", true);
      } else if (faceDownLatched && tilt.z > 0.0f) {
        faceDownLatched = false;
      }
      if (fabsf(tilt.g - 1.0f) > 0.9f && now - lastShakeMs > 500) {
        lastShakeMs = now;
        if (scene->runtime.listens("shake")) dispatch("shake", true);
      }
    }
  }
#if defined(WIGNET_HAS_AUDIO)
  if (scene && scene->tone.present) {
    AudioHAL::setToneHz(toneHz);
    // Wanted and silent (not yet started, after a cue, after an alarm): start it.
    if (toneWanted && !AudioHAL::isPlaying() && pendingSound.empty() &&
        !AudioHAL::startTone(scene->tone.wave, toneHz, scene->tone.volume, scene->tone.glideMs))
      Serial.println("[Scene] Tone not started");
  }
#endif
}

// ---------------------------------------------------------------- buttons --
// Polled from an LVGL timer, so every action runs in the LVGL task. PWR is
// never read here: power_hold_hal.cpp owns it (2 s: off).

struct Button {
  const char *name;
  int pin;
  bool down, consumed;
  uint32_t since;
};
Button buttons[] = {
    {"a", 0, false, false, 0}, // BOOT
#if defined(WIGNET_BUTTON_KEY)
    {"b", WIGNET_BUTTON_KEY, false, false, 0}, // PLUS
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

void pollButtons(lv_timer_t *) {
  const uint32_t now = millis();
  // PWR: the board keeps the 2 s hold (switching off); a scene may have the short press.
  if (PowerHoldHAL::takeShortPress() && scene && !soundTakesPress()) dispatch("pwr.short", true);
  for (Button &button : buttons) {
    const bool pressed = digitalRead(button.pin) == LOW;
    if (pressed && !button.down) {
      button.down = true;
      button.since = now;
      // A ringing alarm takes the press to stop it, and no scene plays: never the scene's.
      button.consumed = !scene || soundTakesPress();
      if (!button.consumed) dispatch(std::string(button.name) + ".down", true);
    } else if (pressed && !button.consumed && now - button.since >= HOLD_MS) {
      button.consumed = true;
      Serial.printf("[Scene] %s held 5 s\n", button.name);
      stopReason = "a 5 s hold";
      restartWanted = scene && scene->slide;
    } else if (!pressed && button.down) {
      button.down = false;
      if (button.consumed || !scene) continue;
      const uint32_t held = now - button.since;
      dispatch(std::string(button.name) + ".up", true);
      dispatch(std::string(button.name) + (held < SHORT_MAX_MS ? ".short" : ".long"), true);
    }
  }
}

// ------------------------------------------------------------- download --

bool getText(const String &url, String &text) {
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setTimeout(15000);
  const int code = http.GET();
  if (code == HTTP_CODE_OK) text = http.getString();
  else Serial.printf("[Scene] GET %s: %d\n", url.c_str(), code);
  http.end();
  return code == HTTP_CODE_OK && text.length();
}

// A zlib stream into exactly `outLen` bytes, with the ROM's inflater.
bool inflateInto(const uint8_t *src, size_t srcLen, uint8_t *out, size_t outLen) {
  auto *inflater = static_cast<tinfl_decompressor *>(heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_8BIT));
  if (!inflater) return false;
  tinfl_init(inflater);
  size_t in = srcLen, produced = outLen;
  const tinfl_status status = tinfl_decompress(inflater, src, &in, out, out, &produced,
                                               TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  heap_caps_free(inflater);
  return status == TINFL_STATUS_DONE && produced == outLen;
}

// `length` bytes of the body into `into`.
bool readBody(HTTPClient &http, uint8_t *into, size_t length) {
  WiFiClient *stream = http.getStreamPtr();
  size_t got = 0;
  uint32_t last = millis();
  while (got < length && http.connected() && millis() - last < 15000) {
    const int available = stream->available();
    if (available <= 0) {
      delay(2);
      continue;
    }
    got += stream->readBytes(into + got, min(length - got, static_cast<size_t>(available)));
    last = millis();
  }
  return got == length;
}

// An asset of `bytes` bytes, asked for deflated (z=1, docs/scenes.md
// "Compressed assets"): a body of exactly `bytes` is the raw asset (a server
// that does not compress), a shorter one a zlib stream inflated into `into`.
bool getBytes(const String &url, uint8_t *into, size_t bytes) {
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, url + "&z=1")) return false;
  http.setTimeout(15000);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[Scene] GET %s: %d\n", url.c_str(), code);
    http.end();
    return false;
  }
  const int size = http.getSize();
  bool ok = false;
  if (size == static_cast<int>(bytes) || size < 0) {
    ok = readBody(http, into, bytes);
  } else if (size > 0 && size < static_cast<int>(bytes)) {
    auto *packed = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    ok = packed && readBody(http, packed, size) && inflateInto(packed, size, into, bytes);
    if (packed) heap_caps_free(packed);
    if (!ok) Serial.printf("[Scene] %d B deflated: not inflated into %u B\n", size, static_cast<unsigned>(bytes));
  }
  http.end();
  return ok;
}

bool sha256Matches(const uint8_t *data, size_t bytes, const char *hex) {
  uint8_t digest[32];
  mbedtls_sha256(data, bytes, digest, 0);
  char out[65];
  for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", digest[i]);
  return strncmp(out, hex, 64) == 0;
}

// --------------------------------------------------------------- loading --

// Bytes a frame holds: RGB565 (2 a pixel), RGB565A8 (3), or i4, its palette
// (16 ARGB8888 entries, what LVGL reads right before the indices) then two
// pixels a byte, each row whole.
size_t frameBytesOf(const Asset &asset) {
  if (asset.indexed) return 16 * 4 + static_cast<size_t>((asset.w + 1) / 2) * asset.h;
  return static_cast<size_t>(asset.w) * asset.h * (asset.alpha ? 3 : 2);
}

// One lv_image_dsc_t per frame of an asset whose data is in place.
void describe(Asset &asset) {
  const size_t frameBytes = frameBytesOf(asset);
  asset.dsc.resize(asset.frames);
  for (int f = 0; f < asset.frames; f++) {
    lv_image_dsc_t &dsc = asset.dsc[f];
    memset(&dsc, 0, sizeof(dsc));
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = asset.indexed ? LV_COLOR_FORMAT_I4
                    : asset.alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    dsc.header.w = asset.w;
    dsc.header.h = asset.h;
    // Of the RGB565 plane (an A8 plane follows), or of the i4 indices.
    dsc.header.stride = asset.indexed ? (asset.w + 1) / 2 : asset.w * 2;
    dsc.data_size = frameBytes;
    dsc.data = asset.data + f * frameBytes;
  }
}

// The manifest and every asset, checked, into a Scene not yet on screen.
// Network only: no LVGL call, so it runs from the loop without the lock.
std::unique_ptr<Scene> load(const String &id, const String &url) {
  const String mac = WiFi.macAddress();
  String text;
  if (!getText(baseUrl() + url + "?mac=" + mac, text)) return nullptr;
  JsonDocument doc;
  if (deserializeJson(doc, text)) {
    Serial.println("[Scene] Manifest: invalid JSON");
    return nullptr;
  }
  if ((doc["v"] | 0) != 2) {
    Serial.printf("[Scene] Manifest version %d: not played (runtime 2 only)\n", doc["v"] | 0);
    return nullptr;
  }
  // The frame, or the frame turned a quarter (a page laid out for a board
  // held the other way). A bundle with `layouts` is checked against those.
  const int turn = doc["turn"] | 0;
  const bool sideways = turn == 90 || turn == 270;
  const int w = doc["w"] | 0, h = doc["h"] | 0;
  const int fw = DisplayHAL::getWidth(), fh = DisplayHAL::getHeight();
  const bool laidOut = doc["layouts"].is<JsonObjectConst>();
  if (!laidOut &&
      (turn % 90 || turn < 0 || turn >= 360 || w != (sideways ? fh : fw) || h != (sideways ? fw : fh))) {
    Serial.printf("[Scene] Laid out for %dx%d turned %d, the screen is %dx%d: not played\n", w, h, turn,
                  fw, fh);
    return nullptr;
  }
  std::unique_ptr<Scene> next(new Scene());
  next->id = id;
  next->w = w;
  next->h = h;
  next->turn = turn;
  next->start = doc["start"] | 1;
  next->scale = doc["scale"] | 1.0f;
  std::string error;
  if (!next->runtime.load(doc.as<JsonObjectConst>(), error)) {
    Serial.printf("[Scene] Runtime: %s: not played\n", error.c_str());
    return nullptr;
  }
  size_t total = 0;
  for (JsonPairConst item : doc["assets"].as<JsonObjectConst>()) {
    const String name = item.key().c_str();
    JsonObjectConst info = item.value();
    std::unique_ptr<Asset> asset(new Asset());
    asset->w = info["w"] | 0;
    asset->h = info["h"] | 0;
    asset->frames = info["frames"] | 1;
    const String format = info["format"] | "";
    asset->alpha = format == "rgb565a8";
    asset->indexed = format == "i4";
    asset->bytes = info["bytes"] | 0;
    const size_t frameBytes = frameBytesOf(*asset);
    total += asset->bytes;
    if (!frameBytes || asset->frames < 1 || asset->bytes != frameBytes * asset->frames || total > BUNDLE_MAX_BYTES ||
        !info["file"].is<const char *>()) {
      Serial.printf("[Scene] Asset %s: sizes do not add up\n", name.c_str());
      return nullptr;
    }
    asset->data = static_cast<uint8_t *>(heap_caps_malloc(asset->bytes, MALLOC_CAP_SPIRAM));
    if (!asset->data) {
      Serial.printf("[Scene] Asset %s: no memory for %u B\n", name.c_str(), static_cast<unsigned>(asset->bytes));
      return nullptr;
    }
    if (!getBytes(baseUrl() + "/scene/" + id + "/" + name + ".bin?mac=" + mac, asset->data, asset->bytes) ||
        !sha256Matches(asset->data, asset->bytes, info["sha256"] | "")) {
      Serial.printf("[Scene] Asset %s: download or checksum failed\n", name.c_str());
      return nullptr;
    }
    describe(*asset);
    next->assets[name.c_str()] = std::move(asset);
  }
  auto assetNamed = [&](const char *name) -> Asset * {
    auto found = next->assets.find(name ? name : "");
    return found == next->assets.end() ? nullptr : found->second.get();
  };
  for (JsonObjectConst item : doc["pages"].as<JsonArrayConst>()) {
    Page page;
    page.number = item["page"] | 0;
    if (item["bg"].is<const char *>()) {
      page.bg = assetNamed(item["bg"]);
      if (!page.bg) return nullptr;
    } else {
      page.color = item["color"] | 0;
    }
    next->pages.push_back(page);
  }
  if (next->pages.empty()) return nullptr;
  for (JsonObjectConst item : doc["objects"].as<JsonArrayConst>()) {
    std::unique_ptr<Obj> obj(new Obj());
    const String kind = item["obj"] | "";
    obj->id = item["id"] | 0;
    obj->page = item["page"] | 0;
    obj->parent = item["parent"] | 0;
    obj->x = item["x"] | 0;
    obj->y = item["y"] | 0;
    obj->hidden = item["hidden"] | false;
    obj->sprite = kind == "sprite";
    obj->rect = kind == "rect";
    obj->group = kind == "group";
    if (obj->rect || obj->group) {
      obj->w = item["w"] | 0;
      obj->h = item["h"] | 0;
      obj->color = item["color"] | 0;
    } else if (kind == "img" || obj->sprite) {
      obj->asset = assetNamed(item["asset"]);
      if (!obj->asset) return nullptr;
    } else {
      Serial.printf("[Scene] Object %d is a %s: not played\n", obj->id, kind.c_str());
      return nullptr;
    }
    if (obj->parent) {
      // Its group comes before it in the manifest.
      bool known = false;
      for (auto &other : next->objects) known |= other->id == obj->parent && other->group;
      if (!known) {
        Serial.printf("[Scene] Object %d: no group %d before it\n", obj->id, obj->parent);
        return nullptr;
      }
    }
    if (obj->sprite) {
      obj->framed = next->runtime.binds('o', obj->id, SceneRuntime::FRAME);
      for (JsonPairConst state : item["states"].as<JsonObjectConst>()) {
        State &s = obj->states[state.key().c_str()];
        for (int index : state.value()["frames"].as<JsonArrayConst>())
          if (index >= 0 && index < obj->asset->frames) s.frames.push_back(&obj->asset->dsc[index]);
        s.ms = state.value()["ms"] | 200;
        s.forMs = state.value()["for"] | 0;
        s.then = state.value()["then"] | "";
      }
      obj->state = item["state"] | "";
      if (item["wander"].is<JsonArrayConst>()) {
        obj->wanders = true;
        obj->wx0 = item["wander"][0];
        obj->wy0 = item["wander"][1];
        obj->wx1 = item["wander"][2];
        obj->wy1 = item["wander"][3];
        obj->speed = item["speed"] | 30;
      }
      if (item["avoid"].is<JsonArrayConst>()) {
        obj->avoids = true;
        obj->ax = item["avoid"][0];
        obj->ay = item["avoid"][1];
        obj->aw = item["avoid"][2];
        obj->ah = item["avoid"][3];
      }
    }
    next->objects.push_back(std::move(obj));
  }
  JsonObjectConst tone = doc["tone"];
  if (!tone.isNull()) {
    Tone &t = next->tone;
    t.present = true;
    t.volume = tone["volume"] | 50;
    t.glideMs = tone["glide_ms"] | 60;
#if defined(WIGNET_HAS_AUDIO)
    const std::string wave = tone["wave"] | "triangle";
    t.wave = wave == "sine" ? AudioHAL::Wave::Sine : wave == "square" ? AudioHAL::Wave::Square
             : wave == "saw" ? AudioHAL::Wave::Saw : AudioHAL::Wave::Triangle;
#endif
  }
  for (JsonPairConst item : doc["sounds"].as<JsonObjectConst>()) {
    Sound sound;
    sound.tune = item.value()["tune"] | "";
    sound.volume = item.value()["volume"] | 50;
    next->sounds[item.key().c_str()] = sound;
  }
  // Each move's target, in the order of the entries and their actions: the
  // runtime numbers them the same way.
  for (JsonObjectConst item : doc["on"].as<JsonArrayConst>()) {
    const std::string event = item["event"] | "";
    next->followsTilt |= event.rfind("tilt.", 0) == 0 || event == "shake" || event == "face_down";
    for (JsonArrayConst op : item["do"].as<JsonArrayConst>())
      if (String(op[0] | "") == "move") next->moves.push_back({op[2] | 0, op[3] | 0});
  }
  next->followsTilt |= next->runtime.readsTilt();
  // Where everything sits on each page shape.
  for (JsonPairConst item : doc["layouts"].as<JsonObjectConst>()) {
    JsonObjectConst info = item.value();
    Layout &layout = next->layouts[item.key().c_str()];
    layout.w = info["w"] | 0;
    layout.h = info["h"] | 0;
    for (JsonPairConst at : info["objects"].as<JsonObjectConst>())
      layout.objects[atoi(at.key().c_str())] = {at.value()[0] | 0, at.value()[1] | 0};
    for (JsonPairConst box : info["wander"].as<JsonObjectConst>()) {
      std::vector<int> &area = layout.wander[atoi(box.key().c_str())];
      for (JsonVariantConst v : box.value().as<JsonArrayConst>()) area.push_back(v.as<int>());
    }
    for (JsonArrayConst at : info["moves"].as<JsonArrayConst>()) layout.moves.push_back({at[0] | 0, at[1] | 0});
    for (JsonPairConst bg : info["bg"].as<JsonObjectConst>()) {
      Asset *asset = assetNamed(bg.value().as<const char *>());
      if (!asset || asset->w != layout.w || asset->h != layout.h) {
        Serial.printf("[Scene] Layout %s: background %s does not fit\n", item.key().c_str(), bg.value().as<const char *>());
        return nullptr;
      }
      layout.bg[atoi(bg.key().c_str())] = asset;
    }
    if (layout.moves.size() != next->moves.size()) {
      Serial.printf("[Scene] Layout %s: %u moves for %u: not played\n", item.key().c_str(),
                    static_cast<unsigned>(layout.moves.size()), static_cast<unsigned>(next->moves.size()));
      return nullptr;
    }
  }
  if (laidOut) {
    const Layout *layout = layoutFor(*next, heldTurn);
    if (!layout) {
      Serial.printf("[Scene] No layout for the page held at %d (frame %dx%d): not played\n", heldTurn, fw, fh);
      return nullptr;
    }
    applyLayout(*next, *layout, pageTurnFor(heldTurn));
  }
  Serial.printf("[Scene] Loaded %s: %u pages, %u objects, %u layouts, %u B of assets, %u B of PSRAM free\n",
                id.c_str(), static_cast<unsigned>(next->pages.size()), static_cast<unsigned>(next->objects.size()),
                static_cast<unsigned>(next->layouts.size()), static_cast<unsigned>(total),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  return next;
}

// -------------------------------------------------------------- on screen --
// Both under the lock.

void takeDown() {
  if (!scene) return;
  for (auto &obj : scene->objects)
    if (obj->timed) lv_timer_delete(obj->timed);
  lv_obj_clean(lv_screen_active()); // its animations go with it
#if defined(WIGNET_HAS_AUDIO)
  pendingSound.clear();
  if (AudioHAL::isPlaying() && !AudioHAL::isRinging()) AudioHAL::stop(); // its cue and tone go with it
#endif
  toneWanted = false;
  OrientationHAL::follow(false);
  scene.reset();
  stopReason = "";
  DisplayHAL::lendPanel(false);
  DisplayHAL::showFrame(); // the page the loop kept decoding meanwhile
}

void putUp(std::unique_ptr<Scene> next) {
  takeDown();
  scene = std::move(next);
  // Turned while it downloaded: the layout for how the board is held now.
  if (const Layout *layout = scene->layouts.empty() ? nullptr : layoutFor(*scene, heldTurn))
    applyLayout(*scene, *layout, pageTurnFor(heldTurn));
  pageTurn = scene->turn;
  lv_display_set_resolution(display, scene->w, scene->h);
  lv_obj_t *screen = lv_screen_active();
  for (Page &page : scene->pages) {
    page.o = lv_obj_create(screen);
    lv_obj_remove_style_all(page.o);
    lv_obj_set_size(page.o, scene->w, scene->h);
    lv_obj_remove_flag(page.o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(page.o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(page.o, LV_OBJ_FLAG_HIDDEN); // until the runtime shows it
    if (page.bg) {
      page.bgImage = lv_image_create(page.o);
      lv_image_set_src(page.bgImage, &page.bg->dsc[0]);
      lv_obj_set_pos(page.bgImage, 0, 0);
    } else {
      lv_obj_set_style_bg_color(page.o, lv_color_hex(page.color), 0);
      lv_obj_set_style_bg_opa(page.o, LV_OPA_COVER, 0);
    }
  }
  for (auto &obj : scene->objects) {
    lv_obj_t *parent = nullptr;
    if (obj->parent) {
      if (Obj *group = find(obj->parent)) parent = group->o;
    } else {
      for (Page &page : scene->pages)
        if (page.number == obj->page) parent = page.o;
    }
    if (obj->rect || obj->group) {
      obj->o = lv_obj_create(parent);
      lv_obj_remove_style_all(obj->o);
      lv_obj_remove_flag(obj->o, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_remove_flag(obj->o, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_size(obj->o, obj->w, obj->h);
      if (obj->rect) {
        lv_obj_set_style_bg_color(obj->o, lv_color_hex(obj->color), 0);
        lv_obj_set_style_bg_opa(obj->o, LV_OPA_COVER, 0);
      }
    } else if (obj->sprite && !obj->framed) {
      obj->o = lv_animimg_create(parent);
    } else {
      obj->o = lv_image_create(parent);
      lv_image_set_src(obj->o, &obj->asset->dsc[0]);
    }
    if (obj->asset) lv_image_set_pivot(obj->o, obj->asset->w / 2, obj->asset->h / 2); // turned around its centre
    lv_obj_set_pos(obj->o, obj->x, obj->y);
    if (obj->hidden) lv_obj_add_flag(obj->o, LV_OBJ_FLAG_HIDDEN);
    if (!obj->sprite || obj->framed) continue;
    std::string first = obj->state;
    if (!obj->states.count(first) && !obj->states.empty()) first = obj->states.begin()->first;
    enterState(obj.get(), first);
    if (obj->wanders) {
      obj->wandering = true;
      wanderTo(obj.get());
    }
  }
  toneWanted = false;
  toneHz = 261.63f;
  for (bool &latched : tiltLatched) latched = true; // nothing fires before the board moves
  faceDownLatched = true;
  OrientationHAL::follow(scene->followsTilt);
  DisplayHAL::lendPanel(true);
  // The runtime shows the first page and sends every bound value.
  scene->runtime.setHost(&host);
  scene->runtime.setSlide(scene->slide);
  scene->runtime.setSeed(esp_random());
  scene->runtime.start(millis());
  if (clockEpoch) scene->runtime.setClock(clockEpoch, clockOffset, clockMillis);
  clockFresh = false;
  lv_obj_invalidate(screen);
  Serial.printf("[Scene] Playing %s\n", scene->id.c_str());
}

// The board turned: the page for how it is held now, every object at its
// place there (or where its last `move` sends it), wander areas and moves of
// that layout. States, timers, the page shown, variables and pending events
// stay: the same scene, nothing downloaded. Under the lock.
void relayout() {
  lv_obj_t *screen = lv_screen_active();
  const Layout *layout = scene->layouts.empty() ? nullptr : layoutFor(*scene, heldTurn);
  const int turn = pageTurnFor(heldTurn);
  if (!layout || (layout == scene->layout && turn == pageTurn)) {
    // One page only, or the frame turned itself.
    lv_obj_invalidate(screen);
    return;
  }
  const uint32_t started = millis();
  applyLayout(*scene, *layout, turn);
  pageTurn = turn;
  lv_display_set_resolution(display, scene->w, scene->h);
  for (Page &page : scene->pages) {
    lv_obj_set_size(page.o, scene->w, scene->h);
    if (page.bg && page.bgImage) lv_image_set_src(page.bgImage, &page.bg->dsc[0]);
  }
  for (auto &obj : scene->objects) {
    if (obj->parent) continue; // placed in its group
    stopMoving(obj.get());
    place(obj.get());
    if (obj->wandering) wanderTo(obj.get());
  }
  lv_obj_invalidate(screen);
  Serial.printf("[Scene] Turned to %d: %dx%d page, laid out in %lu ms\n", turn, scene->w, scene->h,
                static_cast<unsigned long>(millis() - started));
}

} // namespace

namespace ScenePlayer {

void begin() {
  lock = xSemaphoreCreateRecursiveMutex();
  heldTurn = OrientationHAL::turn();
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  const int w = DisplayHAL::getWidth(), h = DisplayHAL::getHeight();
  display = lv_display_create(w, h);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  const size_t bufferBytes = static_cast<size_t>(w) * BUFFER_LINES * 2;
  void *buf1 = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, bufferBytes, MALLOC_CAP_SPIRAM);
  void *buf2 = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, bufferBytes, MALLOC_CAP_SPIRAM);
  if (!buf1 || !buf2) {
    Serial.println("[Scene] No memory for LVGL's draw buffers: scenes off");
    return;
  }
  lv_display_set_buffers(display, buf1, buf2, bufferBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(display, flush);
#if defined(WIGNET_DRAW_TIMING)
  lv_display_add_event_cb(display, onRefresh, LV_EVENT_REFR_START, nullptr);
  lv_display_add_event_cb(display, onRefresh, LV_EVENT_REFR_READY, nullptr);
#endif
  lv_obj_t *screen = lv_screen_active();
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  for (Button &button : buttons) pinMode(button.pin, INPUT_PULLUP);
  lv_timer_create(pollButtons, 20, nullptr);
  lv_timer_create(tickScene, 20, nullptr);
  lv_timer_create(playCues, 50, nullptr);
  lv_timer_create(tickAvoid, 40, nullptr);
  xTaskCreatePinnedToCore(lvglTask, "lvgl", 8192, nullptr, 2, nullptr, 1);
  Serial.println("[Scene] Player ready (runtime 2)");
}

String scheduleParams() {
  String out = "&in=a";
#if defined(WIGNET_BUTTON_KEY)
  out += ",b";
#endif
  if (lastEventSeq > 0) out += "&sev=" + String(lastEventSeq);
  return out;
}

void onSchedule(const String &payload) {
  JsonDocument filter;
  filter["scene"] = true;
  filter["sceneEvents"] = true;
  filter["serverEpoch"] = true;
  filter["utcOffsetSec"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) return;
  const uint32_t epoch = doc["serverEpoch"] | 0;
  if (epoch > 1700000000UL && lock) {
    Locked held;
    clockEpoch = epoch;
    clockMillis = millis();
    clockOffset = doc["utcOffsetSec"] | 0;
    clockFresh = true;
  }
  // Absent (an older server) or null: no scene, the loop's pages.
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
  {
    Locked held;
    if (stopReason.length()) {
      Serial.printf("[Scene] Ended by %s%s\n", stopReason.c_str(), restartWanted ? ": starting the slide again" : "");
      // A slide is not remembered as ended: the next pass loads it again.
      if (scene && !restartWanted) endedId = scene->id;
      restartWanted = false;
      takeDown();
    }
  }
  // A scene the server no longer names, or another one.
  const bool showing = scene != nullptr;
  if (showing && wantedId != scene->id) {
    Locked held;
    Serial.println("[Scene] No longer named by /schedule");
    takeDown();
  }
  if (wantedId != endedId) endedId = "";
  if (wantedId.length() && !scene && wantedId != endedId &&
      (!retryAtMs || static_cast<int32_t>(millis() - retryAtMs) >= 0)) {
    std::unique_ptr<Scene> next = load(wantedId, wantedUrl);
    if (next) {
      next->slide = wantedSlide;
      retryAtMs = 0;
      Locked held;
      putUp(std::move(next));
    } else {
      retryAtMs = millis() + RETRY_MS;
      Serial.println("[Scene] Not played; trying again in 30 s");
    }
  }
  if (scene && (clockFresh || !pendingEvents.empty())) {
    Locked held;
    if (scene && clockFresh) scene->runtime.setClock(clockEpoch, clockOffset, clockMillis);
    clockFresh = false;
    for (const std::string &event : pendingEvents) dispatch(event, false);
  }
  pendingEvents.clear();
  std::vector<Report> outgoing;
  String id;
  {
    Locked held;
    outgoing.swap(reports);
    if (scene) id = scene->id;
  }
  if (outgoing.empty() || id.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  JsonDocument body;
  body["publication_id"] = id;
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
    Serial.printf("[Scene] Sent %u report(s): %d\n", static_cast<unsigned>(outgoing.size()), code);
    http.end();
  }
}

bool playing() { return scene != nullptr; }

void onTurn(int turn) {
  heldTurn = turn;
  if (!lock || !scene) return;
  Locked held;
  if (scene) relayout();
}

} // namespace ScenePlayer

#endif // WIGNET_SCENE_PLAYER
