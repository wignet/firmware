/**
 * LVGL spike on the Waveshare ESP32-S3-LCD-2 (docs/lvgl_spike.md).
 *
 * The server keeps every pixel: the background and the sprite sheet arrive
 * already drawn, in LVGL's native formats (spike/make_assets.py), and LVGL
 * only composes and moves them. The scene is a JSON string, standing in for
 * what /schedule could one day carry:
 * - a 320x240 RGB565 background;
 * - a 48x48 RGB565A8 sprite sheet, its frames named by state;
 * - the sprite wanders (lv_anim on x and y) and walks (lv_animimg);
 * - BOOT (GPIO0, an LVGL keypad) makes it happy for a few seconds.
 *
 * LVGL draws into two partial buffers in PSRAM; the flush goes through
 * DisplayHAL::pushRegion(), the same turn and colour calibration as every
 * other screen. lv_timer_handler runs in its own task on the loop's core,
 * one priority above it, and prints its own measurements every 5 s.
 */
#if defined(WIGNET_LVGL_SPIKE)

#include "lvgl_spike.h"
#include "hal/display_hal.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <map>
#include <string>
#include <vector>

// The pictures, embedded as they are (board_build.embed_files).
#if defined(WIGNET_LVGL_BG_240)
extern const uint8_t backgroundStart[] asm("_binary_spike_background_240_bin_start");
extern const uint8_t backgroundEnd[] asm("_binary_spike_background_240_bin_end");
#else
extern const uint8_t backgroundStart[] asm("_binary_spike_background_bin_start");
extern const uint8_t backgroundEnd[] asm("_binary_spike_background_bin_end");
#endif
extern const uint8_t sheetStart[] asm("_binary_spike_walker_bin_start");
extern const uint8_t sheetEnd[] asm("_binary_spike_walker_bin_end");

namespace {

// What an agent would describe. Frames index the sheet, left to right.
const char SCENE_JSON[] = R"json({
  "background": "background",
  "sprite": {
    "sheet": "walker", "w": 48, "h": 48,
    "start": "walk",
    "states": {
      "walk":      {"frames": [0, 1, 2, 3], "frame_ms": 140},
      "walk_left": {"frames": [6, 7, 8, 9], "frame_ms": 140},
      "happy":     {"frames": [4, 5], "frame_ms": 220, "seconds": 3, "hop": 10}
    },
    "wander": {"speed": 40, "pause_ms": [200, 1500]}
  },
  "on": {"button": "happy"}
})json";

constexpr int BUFFER_LINES = 40;
constexpr int BOOT_PIN = 0;

struct State {
  std::vector<const void *> frames;
  uint32_t frameMs = 150;
  uint32_t seconds = 0; // 0: until another state replaces it
  int hop = 0;
};

struct Scene {
  std::map<std::string, State> states;
  std::string start;
  std::string onButton;
  int speed = 40;
  int pauseMin = 200, pauseMax = 1500;
  int areaX0 = 0, areaY0 = 0, areaX1 = 0, areaY1 = 0;
};

Scene scene;
std::vector<lv_image_dsc_t> frames;
lv_image_dsc_t backgroundImage;
lv_obj_t *sprite = nullptr;
std::string currentState;
bool busy = false; // in a timed state: no wandering
int restY = 0;

// ---------------------------------------------------------- measurements --
struct Stats {
  uint32_t frames = 0, flushes = 0, pixels = 0;
  uint64_t flushUs = 0, handlerUs = 0;
  uint32_t maxGapUs = 0, maxHandlerUs = 0;
} stats;

// ---------------------------------------------------------------- display --

void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
  const uint32_t t0 = micros();
  const int w = lv_area_get_width(area), h = lv_area_get_height(area);
  DisplayHAL::pushRegion(reinterpret_cast<const uint16_t *>(pixels), area->x1,
                         area->y1, w, h);
  stats.flushUs += micros() - t0;
  stats.flushes++;
  stats.pixels += w * h;
  if (lv_display_flush_is_last(display)) stats.frames++;
  lv_display_flush_ready(display);
}

void readBoot(lv_indev_t *, lv_indev_data_t *data) {
  data->key = LV_KEY_ENTER;
  data->state = digitalRead(BOOT_PIN) == LOW ? LV_INDEV_STATE_PRESSED
                                             : LV_INDEV_STATE_RELEASED;
}

// ------------------------------------------------------------------ scene --

bool parseScene() {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, SCENE_JSON);
  if (error) {
    Serial.printf("[LVGL] Scene JSON: %s\n", error.c_str());
    return false;
  }
  const size_t sheetBytes = sheetEnd - sheetStart;
  const int w = doc["sprite"]["w"] | 48, h = doc["sprite"]["h"] | 48;
  const size_t frameBytes = static_cast<size_t>(w) * h * 3; // RGB565 + A8
  const size_t count = sheetBytes / frameBytes;
  frames.resize(count);
  for (size_t i = 0; i < count; i++) {
    lv_image_dsc_t &dsc = frames[i];
    memset(&dsc, 0, sizeof(dsc));
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc.header.w = w;
    dsc.header.h = h;
    dsc.header.stride = w * 2; // of the RGB565 plane; the A8 plane follows
    dsc.data_size = frameBytes;
    dsc.data = sheetStart + i * frameBytes;
  }
  for (JsonPair state : doc["sprite"]["states"].as<JsonObject>()) {
    State &s = scene.states[state.key().c_str()];
    for (int index : state.value()["frames"].as<JsonArray>()) {
      if (index >= 0 && static_cast<size_t>(index) < count)
        s.frames.push_back(&frames[index]);
    }
    s.frameMs = state.value()["frame_ms"] | 150;
    s.seconds = state.value()["seconds"] | 0;
    s.hop = state.value()["hop"] | 0;
  }
  scene.start = doc["sprite"]["start"] | "walk";
  scene.onButton = doc["on"]["button"] | "";
  JsonObject wander = doc["sprite"]["wander"];
  scene.speed = max(1, wander["speed"] | 40);
  scene.pauseMin = wander["pause_ms"][0] | 200;
  scene.pauseMax = wander["pause_ms"][1] | 1500;
  // Without an area: the lower part of whatever screen this is.
  const int screenW = DisplayHAL::getWidth(), screenH = DisplayHAL::getHeight();
  scene.areaX0 = wander["area"][0] | 0;
  scene.areaY0 = wander["area"][1] | screenH * 140 / 240;
  scene.areaX1 = wander["area"][2] | screenW - w;
  scene.areaY1 = wander["area"][3] | screenH - h;
  Serial.printf("[LVGL] Scene: %u frames of %dx%d, %u states\n",
                static_cast<unsigned>(count), w, h,
                static_cast<unsigned>(scene.states.size()));
  return count > 0 && scene.states.count(scene.start);
}

void showState(const std::string &name) {
  auto found = scene.states.find(name);
  if (found == scene.states.end() || found->second.frames.empty()) return;
  if (name == currentState) return;
  currentState = name;
  State &s = found->second;
  lv_animimg_set_src(sprite, s.frames.data(), s.frames.size());
  lv_animimg_set_duration(sprite, s.frameMs * s.frames.size());
  lv_animimg_set_repeat_count(sprite, LV_ANIM_REPEAT_INFINITE);
  lv_animimg_start(sprite);
}

void setX(void *obj, int32_t v) { lv_obj_set_x(static_cast<lv_obj_t *>(obj), v); }
void setY(void *obj, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t *>(obj), v); }

void wanderNext(lv_anim_t * = nullptr);

void wanderTo(int x, int y, uint32_t delayMs) {
  const int fromX = lv_obj_get_x(sprite), fromY = lv_obj_get_y(sprite);
  const float distance = sqrtf(float((x - fromX) * (x - fromX) + (y - fromY) * (y - fromY)));
  const uint32_t duration = max<uint32_t>(300, distance * 1000 / scene.speed);
  if (scene.states.count("walk_left"))
    showState(x < fromX ? "walk_left" : scene.start);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, sprite);
  lv_anim_set_duration(&a, duration);
  lv_anim_set_delay(&a, delayMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_exec_cb(&a, setY);
  lv_anim_set_values(&a, fromY, y);
  lv_anim_start(&a);
  lv_anim_set_exec_cb(&a, setX);
  lv_anim_set_values(&a, fromX, x);
  lv_anim_set_completed_cb(&a, wanderNext);
  lv_anim_start(&a);
}

void wanderNext(lv_anim_t *) {
  if (busy) return;
  wanderTo(lv_rand(scene.areaX0, scene.areaX1), lv_rand(scene.areaY0, scene.areaY1),
           lv_rand(scene.pauseMin, scene.pauseMax));
}

void stopWandering() {
  lv_anim_delete(sprite, setX);
  lv_anim_delete(sprite, setY);
}

void endTimedState(lv_timer_t *) {
  lv_anim_delete(sprite, setY);
  lv_obj_set_y(sprite, restY);
  busy = false;
  currentState.clear();
  showState(scene.start);
  wanderNext();
}

void enterState(const std::string &name) {
  auto found = scene.states.find(name);
  if (found == scene.states.end() || busy) return;
  const State &s = found->second;
  stopWandering();
  showState(name);
  if (!s.seconds) return;
  busy = true;
  restY = lv_obj_get_y(sprite);
  if (s.hop) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, sprite);
    lv_anim_set_exec_cb(&a, setY);
    lv_anim_set_values(&a, restY, restY - s.hop);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_reverse_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
  }
  lv_timer_t *t = lv_timer_create(endTimedState, s.seconds * 1000, nullptr);
  lv_timer_set_repeat_count(t, 1);
}

void onPressed(lv_event_t *) {
  Serial.println("[LVGL] BOOT pressed");
  if (!scene.onButton.empty()) enterState(scene.onButton);
}

// ------------------------------------------------------------------- task --

void lvglTask(void *) {
  uint32_t lastStart = 0, windowStart = millis();
  for (;;) {
    const uint32_t t0 = micros();
    if (lastStart) stats.maxGapUs = max(stats.maxGapUs, t0 - lastStart);
    lastStart = t0;
    uint32_t next = lv_timer_handler();
    const uint32_t spent = micros() - t0;
    stats.handlerUs += spent;
    stats.maxHandlerUs = max(stats.maxHandlerUs, spent);

    const uint32_t now = millis();
    if (now - windowStart >= 5000) {
      const float seconds = (now - windowStart) / 1000.0f;
      Serial.printf(
          "[LVGL] %.1f fps, CPU %.1f%% (flush %.1f%%), %u px/frame, "
          "handler max %.1f ms, gap max %.1f ms, %u MHz, heap %u int / %u psram, "
          "stack free %u\n",
          stats.frames / seconds, stats.handlerUs / (seconds * 1e4f),
          stats.flushUs / (seconds * 1e4f),
          stats.frames ? stats.pixels / stats.frames : 0,
          stats.maxHandlerUs / 1000.0f, stats.maxGapUs / 1000.0f,
          getCpuFrequencyMhz(),
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
          uxTaskGetStackHighWaterMark(nullptr));
      stats = Stats();
      windowStart = now;
    }
    if (next == LV_NO_TIMER_READY || next > LV_DEF_REFR_PERIOD) next = LV_DEF_REFR_PERIOD;
    vTaskDelay(pdMS_TO_TICKS(max<uint32_t>(next, 1)));
  }
}

} // namespace

namespace LvglSpike {

void start() {
#if defined(WIGNET_LVGL_CPU_MHZ)
  setCpuFrequencyMhz(WIGNET_LVGL_CPU_MHZ); // setup() runs at 80 MHz
#endif
  const size_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t psramBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  const int w = DisplayHAL::getWidth(), h = DisplayHAL::getHeight();
  lv_display_t *display = lv_display_create(w, h);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  const size_t bufferBytes = static_cast<size_t>(w) * BUFFER_LINES * 2;
#if defined(WIGNET_LVGL_BUF_INTERNAL)
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#else
  const uint32_t caps = MALLOC_CAP_SPIRAM;
#endif
  void *buf1 = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, bufferBytes, caps);
  void *buf2 = heap_caps_aligned_alloc(LV_DRAW_BUF_ALIGN, bufferBytes, caps);
  if (!buf1 || !buf2) {
    Serial.println("[LVGL] No memory for the draw buffers");
    return;
  }
  lv_display_set_buffers(display, buf1, buf2, bufferBytes,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(display, flush);

  pinMode(BOOT_PIN, INPUT_PULLUP);
  lv_indev_t *boot = lv_indev_create();
  lv_indev_set_type(boot, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_read_cb(boot, readBoot);

  if (!parseScene()) return;

  lv_obj_t *screen = lv_screen_active();
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  memset(&backgroundImage, 0, sizeof(backgroundImage));
  backgroundImage.header.magic = LV_IMAGE_HEADER_MAGIC;
  backgroundImage.header.cf = LV_COLOR_FORMAT_RGB565;
  backgroundImage.header.w = w;
  backgroundImage.header.h = h;
  backgroundImage.header.stride = w * 2;
  backgroundImage.data_size = backgroundEnd - backgroundStart;
  if (backgroundImage.data_size != static_cast<uint32_t>(w) * h * 2) {
    Serial.printf("[LVGL] Background is %u B, the screen needs %d\n",
                  static_cast<unsigned>(backgroundImage.data_size), w * h * 2);
    return;
  }
  backgroundImage.data = backgroundStart;
  lv_obj_t *background = lv_image_create(screen);
  lv_image_set_src(background, &backgroundImage);
  lv_obj_set_pos(background, 0, 0);

  sprite = lv_animimg_create(screen);
  lv_obj_set_pos(sprite, (scene.areaX0 + scene.areaX1) / 2,
                 (scene.areaY0 + scene.areaY1) / 2);
  showState(scene.start);
  // BOOT reaches the sprite as the keypad's focused object.
  lv_group_t *group = lv_group_create();
  lv_group_add_obj(group, sprite);
  lv_indev_set_group(boot, group);
  lv_obj_add_event_cb(sprite, onPressed, LV_EVENT_PRESSED, nullptr);
  wanderNext();

  Serial.printf("[LVGL] Started: buffers 2 x %u B in %s; LVGL took %d B "
                "internal, %d B PSRAM\n",
                static_cast<unsigned>(bufferBytes),
#if defined(WIGNET_LVGL_BUF_INTERNAL)
                "internal RAM",
#else
                "PSRAM",
#endif
                static_cast<int>(internalBefore - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<int>(psramBefore - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  DisplayHAL::lendPanel(true);
  // The loop's core, one priority above it: the loop only runs while LVGL
  // waits for its next frame.
  xTaskCreatePinnedToCore(lvglTask, "lvgl", 8192, nullptr, 2, nullptr, 1);
}

} // namespace LvglSpike

#endif // WIGNET_LVGL_SPIKE
