#pragma once
/**
 * The scene runtime 2 (docs/scene_runtime.md): variables, lists, bindings,
 * guarded actions, after:/every:/idle: timers and when: edges, run from a
 * manifest's compiled programs. No LVGL, no Arduino: the scene player is its
 * host (it shows pages, moves sprites, plays sounds) and the same code runs
 * on a computer against tests/scene_runtime/corpus (tests/test_scene_runtime.py),
 * which backoffice/scene_runtime.py, its reference, wrote.
 *
 * Numbers are 32-bit floats, each operation in the order the reference
 * rounds it: build with -ffp-contract=off, so no a * b + c is fused.
 */
#include <ArduinoJson.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace SceneRuntime {

// What a binding sets.
enum Prop : uint8_t { DX, DY, FRAME, ROTATION, VISIBLE, OPACITY, W, H, COLOR, PITCH, ON, PROP_COUNT };
const char *propName(Prop prop);

// A binding's target: an object (o<id>), a page (p<n>), or the tone.
struct Target {
  char kind; // 'o', 'p' or 't'
  int id;
};

// What the runtime asks of its host, in the order it happens.
class Host {
public:
  virtual ~Host() {}
  virtual void page(int number) = 0;
  virtual void state(int id, const std::string &name) = 0;
  virtual void move(int id, int ordinal) = 0; // the ordinal-th move of the scene: a layout's moves[ordinal]
  virtual void wander(int id, bool on) = 0;
  virtual void show(int id, bool shown) = 0;
  virtual void sound(const std::string &name) = 0;
  virtual void report(const std::string &value, const std::string &event, int page) = 0;
  virtual void end() = 0;
  virtual void prop(Target target, Prop prop, float value) = 0;
};

struct Op {
  uint8_t code = 0;
  int16_t arg = 0;
  float n = 0;
};
typedef std::vector<Op> Program;

struct Part {
  bool text = true;
  std::string value;
  Program program;
};

struct Action {
  uint8_t verb = 0;
  int a = 0;      // an object id, a page number, a variable
  int ordinal = -1; // a move's
  bool flag = false;
  std::string text; // a state, a sound, page next|prev
  Program program;  // set
  std::vector<Part> parts; // report
};

struct Entry {
  int page = 0; // 0: every page
  std::string event;
  bool guarded = false;
  Program guard;
  Program cond; // event "when"
  uint32_t condDeps = 0;
  std::vector<Action> actions;
};

struct Binding {
  Target target;
  Prop prop;
  Program program;
  uint32_t deps = 0;
  bool sent = false;
  float last = 0;
};

struct Timer {
  std::string event;
  int order = 0;
  uint32_t due = 0;
  bool armed = false;
};

class Runtime {
public:
  // A v2 manifest; false, with `error` saying why, when it cannot be played.
  bool load(JsonObjectConst manifest, std::string &error);
  void setHost(Host *host) { host_ = host; }
  void setSeed(uint32_t seed) { rng_ = seed ? seed : 1; }
  void setSlide(bool slide) { slide_ = slide; }

  void start(uint32_t now);
  void tick(uint32_t now);
  void setClock(uint32_t epoch, int32_t offset, uint32_t now);
  void setTilt(float x, float y, float z, uint32_t now);
  void dispatch(const std::string &event, bool input, uint32_t now);

  bool readsTilt() const { return readsTilt_; }
  bool ended() const { return ended_; }
  int page() const { return page_; }
  // An entry for `event` on the page shown (or every page).
  bool listens(const std::string &event) const;
  // Its bound properties: a host draws an object with a bound frame as one picture.
  bool binds(char kind, int id, Prop prop) const;

private:
  struct Value {
    float n;
    int list; // -1: a number
  };
  float evaluate(const Program &program);
  Value item(int list, float index) const;
  float source(int which) const;
  float rand(float lo, float hi);
  int64_t local() const;
  void send(Binding &binding);
  void settle(uint32_t changed);
  uint32_t run(const Entry &entry, const std::string &event);
  void armAfter();
  bool idleActive(const std::string &event) const;
  std::string format(float value) const;

  Host *host_ = nullptr;
  std::vector<int> pages_;
  int start_ = 1, page_ = 1;
  std::vector<float> vars_;
  std::vector<std::vector<Value>> lists_;
  std::vector<Binding> bindings_;
  std::vector<Entry> entries_;
  std::vector<bool> whenState_;
  std::vector<Timer> after_, every_, idle_;
  uint32_t rng_ = 1;
  bool slide_ = false, ended_ = false, readsTilt_ = false;
  bool clockKnown_ = false;
  uint32_t epoch_ = 0, clockAt_ = 0, now_ = 0, lastInput_ = 0;
  int32_t offset_ = 0;
  int64_t second_ = -1;
  float tilt_[3] = {0, 0, 1};
};

} // namespace SceneRuntime
