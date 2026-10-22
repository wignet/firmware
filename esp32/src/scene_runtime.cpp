/**
 * The scene runtime 2 (scene_runtime.h, docs/scene_runtime.md): the C++ twin
 * of backoffice/scene_runtime.py's Runtime, step for step.
 */
#include "scene_runtime.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace SceneRuntime {

namespace {

enum Code : uint8_t {
  NUM, VAR, LIST, SRC, ADD, SUB, MUL, DIV, MOD, LT, LE, GT, GE, EQ, NE, AND, OR, NOT, NEG, COND,
  MIN, MAX, CLAMP, FLOOR, ROUND, ABS, LERP, BLEND, LEN, IDX, RAND
};
// Sources, as SRC's arg: time.h, m, s, known, tilt.x, y, z, page.
const char SOURCES[] = "hmskxyzp";
const struct {
  const char *name;
  Code code;
} OPS[] = {{"+", ADD}, {"-", SUB}, {"*", MUL}, {"/", DIV}, {"%", MOD}, {"<", LT}, {"<=", LE}, {">", GT},
           {">=", GE}, {"==", EQ}, {"!=", NE}, {"&&", AND}, {"||", OR}, {"!", NOT}, {"neg", NEG}, {"?", COND},
           {"min", MIN}, {"max", MAX}, {"clamp", CLAMP}, {"floor", FLOOR}, {"round", ROUND}, {"abs", ABS},
           {"lerp", LERP}, {"blend", BLEND}, {"len", LEN}, {"idx", IDX}, {"rand", RAND}};
const char *PROPS[PROP_COUNT] = {"dx", "dy", "frame", "rotation", "visible", "opacity", "w", "h", "color", "pitch", "on"};

// What a program reads: variables in bits 0-15, then time, tilt, page.
constexpr uint32_t TIME = 1u << 16, TILT = 1u << 17, PAGE = 1u << 18;
constexpr int VARS_MAX = 16, STACK = 64, WHEN_DEPTH = 8;

enum Verb : uint8_t { SET, PAGE_TO, REPORT, END, STATE, MOVE, WANDER, SHOW, HIDE, SOUND };

bool parseProgram(JsonArrayConst list, Program &out, uint32_t &deps, std::string &error) {
  for (JsonVariantConst item : list) {
    Op op;
    if (item.is<float>() || item.is<int>()) {
      op.code = NUM;
      op.n = item.as<float>();
    } else {
      const char *text = item | "";
      if (text[0] == '$' || text[0] == '#') {
        op.code = text[0] == '$' ? VAR : LIST;
        op.arg = static_cast<int16_t>(atoi(text + 1));
        if (op.code == VAR) {
          if (op.arg < 0 || op.arg >= VARS_MAX) {
            error = "a variable beyond 16";
            return false;
          }
          deps |= 1u << op.arg;
        }
      } else if (text[0] == '@' && text[1] && !text[2] && strchr(SOURCES, text[1])) {
        op.code = SRC;
        op.arg = static_cast<int16_t>(strchr(SOURCES, text[1]) - SOURCES);
        deps |= op.arg < 4 ? TIME : op.arg < 7 ? TILT : PAGE;
      } else {
        bool known = false;
        for (const auto &named : OPS)
          if (!strcmp(named.name, text)) {
            op.code = named.code;
            known = true;
          }
        if (!known) {
          error = std::string("unknown operation ") + text;
          return false;
        }
      }
    }
    out.push_back(op);
  }
  if (out.empty() || out.size() > STACK) {
    error = "a program of 1 to 64 steps";
    return false;
  }
  return true;
}

bool parseProp(const char *name, Prop &prop) {
  for (int i = 0; i < PROP_COUNT; i++)
    if (!strcmp(PROPS[i], name)) {
      prop = static_cast<Prop>(i);
      return true;
    }
  return false;
}

// Python's min(a, b) and max(a, b): the first one when they are equal.
inline float lesser(float a, float b) { return b < a ? b : a; }
inline float greater(float a, float b) { return b > a ? b : a; }

} // namespace

const char *propName(Prop prop) { return prop < PROP_COUNT ? PROPS[prop] : "?"; }

// ----------------------------------------------------------------- loading --

bool Runtime::load(JsonObjectConst manifest, std::string &error) {
  for (JsonObjectConst page : manifest["pages"].as<JsonArrayConst>()) {
    pages_.push_back(page["page"] | 0);
  }
  if (pages_.empty()) {
    error = "no pages";
    return false;
  }
  start_ = manifest["start"] | pages_[0];
  for (JsonVariantConst v : manifest["vars"].as<JsonArrayConst>()) vars_.push_back(v.as<float>());
  if (vars_.size() > VARS_MAX) {
    error = "more than 16 variables";
    return false;
  }
  for (JsonArrayConst table : manifest["lists"].as<JsonArrayConst>()) {
    std::vector<Value> items;
    for (JsonVariantConst v : table) {
      Value value = {0, -1};
      if (v.is<JsonObjectConst>()) value.list = v["l"] | 0;
      else value.n = v.as<float>();
      items.push_back(value);
    }
    lists_.push_back(items);
  }
  for (const auto &table : lists_)
    for (const Value &v : table)
      if (v.list >= static_cast<int>(lists_.size()) || table.empty()) {
        error = "a list refers to no list";
        return false;
      }
  auto bind = [&](JsonObjectConst bound, char kind, int id) {
    for (JsonPairConst item : bound) {
      Binding binding;
      binding.target = {kind, id};
      if (!parseProp(item.key().c_str(), binding.prop)) {
        error = std::string("unknown property ") + item.key().c_str();
        return false;
      }
      if (!parseProgram(item.value().as<JsonArrayConst>(), binding.program, binding.deps, error)) return false;
      bindings_.push_back(binding);
    }
    return true;
  };
  for (JsonObjectConst obj : manifest["objects"].as<JsonArrayConst>())
    if (!bind(obj["bind"].as<JsonObjectConst>(), 'o', obj["id"] | 0)) return false;
  for (JsonObjectConst page : manifest["pages"].as<JsonArrayConst>())
    if (!bind(page["bind"].as<JsonObjectConst>(), 'p', page["page"] | 0)) return false;
  if (!bind(manifest["tone"]["bind"].as<JsonObjectConst>(), 't', 0)) return false;
  int moves = 0;
  for (JsonObjectConst item : manifest["on"].as<JsonArrayConst>()) {
    Entry entry;
    entry.page = item["page"].isNull() ? 0 : item["page"].as<int>();
    entry.event = item["event"] | "";
    uint32_t unused = 0;
    if (!item["if"].isNull()) {
      entry.guarded = true;
      if (!parseProgram(item["if"].as<JsonArrayConst>(), entry.guard, unused, error)) return false;
    }
    if (entry.event == "when" &&
        !parseProgram(item["cond"].as<JsonArrayConst>(), entry.cond, entry.condDeps, error))
      return false;
    for (JsonArrayConst op : item["do"].as<JsonArrayConst>()) {
      Action action;
      const std::string verb = op[0] | "";
      if (verb == "set") {
        action.verb = SET;
        action.a = op[1] | 0;
        if (action.a < 0 || action.a >= static_cast<int>(vars_.size()) ||
            !parseProgram(op[2].as<JsonArrayConst>(), action.program, unused, error)) {
          if (error.empty()) error = "set: no such variable";
          return false;
        }
      } else if (verb == "page") {
        action.verb = PAGE_TO;
        if (op[1].is<int>()) action.a = op[1];
        else action.text = op[1] | "";
      } else if (verb == "report") {
        action.verb = REPORT;
        for (JsonVariantConst part : op[1].as<JsonArrayConst>()) {
          Part p;
          if (part.is<JsonArrayConst>()) {
            p.text = false;
            if (!parseProgram(part.as<JsonArrayConst>(), p.program, unused, error)) return false;
          } else {
            p.value = part | "";
          }
          action.parts.push_back(p);
        }
      } else if (verb == "end") {
        action.verb = END;
      } else if (verb == "state") {
        action.verb = STATE;
        action.a = op[1] | 0;
        action.text = op[2] | "";
      } else if (verb == "move") {
        action.verb = MOVE;
        action.a = op[1] | 0;
        action.ordinal = moves++;
      } else if (verb == "wander") {
        action.verb = WANDER;
        action.a = op[1] | 0;
        action.flag = op[2] | false;
      } else if (verb == "show" || verb == "hide") {
        action.verb = verb == "show" ? SHOW : HIDE;
        action.a = op[1] | 0;
      } else if (verb == "sound") {
        action.verb = SOUND;
        action.text = op[1] | "";
      } else {
        error = "unknown action " + verb;
        return false;
      }
      entry.actions.push_back(action);
    }
    entries_.push_back(entry);
  }
  // Timers, by event, in the order the events first appear.
  std::vector<std::string> seen;
  for (const Entry &entry : entries_) {
    bool known = false;
    for (const std::string &event : seen) known |= event == entry.event;
    if (known) continue;
    seen.push_back(entry.event);
    Timer timer;
    timer.event = entry.event;
    timer.order = static_cast<int>(seen.size()) - 1;
    if (entry.event.rfind("after:", 0) == 0) after_.push_back(timer);
    else if (entry.event.rfind("every:", 0) == 0) every_.push_back(timer);
    else if (entry.event.rfind("idle:", 0) == 0) idle_.push_back(timer);
  }
  for (const Binding &binding : bindings_) readsTilt_ |= (binding.deps & TILT) != 0;
  for (const Entry &entry : entries_) readsTilt_ |= (entry.condDeps & TILT) != 0;
  whenState_.assign(entries_.size(), false);
  return true;
}

bool Runtime::listens(const std::string &event) const {
  for (const Entry &entry : entries_)
    if (entry.event == event && (entry.page == 0 || entry.page == page_)) return true;
  return false;
}

bool Runtime::binds(char kind, int id, Prop prop) const {
  for (const Binding &binding : bindings_)
    if (binding.target.kind == kind && binding.target.id == id && binding.prop == prop) return true;
  return false;
}

// ------------------------------------------------------------------ values --

int64_t Runtime::local() const {
  return static_cast<int64_t>(epoch_) + static_cast<uint32_t>(now_ - clockAt_) / 1000 + offset_;
}

float Runtime::source(int which) const {
  if (which == 7) return static_cast<float>(page_);
  if (which >= 4) return tilt_[which - 4];
  if (!clockKnown_) return 0;
  if (which == 3) return 1;
  const int64_t t = local();
  return static_cast<float>(which == 0 ? (t / 3600) % 24 : which == 1 ? (t / 60) % 60 : t % 60);
}

float Runtime::rand(float lo, float hi) {
  uint32_t x = rng_;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  rng_ = x;
  long a = static_cast<long>(floorf(lo)), b = static_cast<long>(floorf(hi));
  if (b < a) {
    const long swap = a;
    a = b;
    b = swap;
  }
  return static_cast<float>(a + static_cast<long>(x % static_cast<uint32_t>(b - a + 1)));
}

Runtime::Value Runtime::item(int list, float index) const {
  const std::vector<Value> &items = lists_[list];
  long k = static_cast<long>(floorf(index)) % static_cast<long>(items.size());
  if (k < 0) k += static_cast<long>(items.size());
  return items[k];
}

float Runtime::evaluate(const Program &program) {
  Value stack[STACK];
  int sp = 0;
  auto num = [](float n) { return Value{n, -1}; };
  for (const Op &op : program) {
    if (sp >= STACK) return 0;
    switch (op.code) {
    case NUM: stack[sp++] = num(op.n); continue;
    case VAR: stack[sp++] = num(op.arg < static_cast<int>(vars_.size()) ? vars_[op.arg] : 0); continue;
    case LIST: stack[sp++] = Value{0, op.arg}; continue;
    case SRC: stack[sp++] = num(source(op.arg)); continue;
    default: break;
    }
    const int arity = op.code == COND || op.code == CLAMP || op.code == LERP ? 3
                      : op.code == NOT || op.code == NEG || op.code == FLOOR || op.code == ROUND || op.code == ABS ||
                              op.code == LEN
                          ? 1
                          : 2;
    if (sp < arity) return 0;
    sp -= arity;
    const Value *in = stack + sp;
    const float a = in[0].n, b = arity > 1 ? in[1].n : 0, c = arity > 2 ? in[2].n : 0;
    Value out = num(0);
    switch (op.code) {
    case ADD: out.n = a + b; break;
    case SUB: out.n = a - b; break;
    case MUL: out.n = a * b; break;
    case DIV: out.n = b == 0 ? 0 : a / b; break;
    case MOD:
      if (b != 0) {
        const float q = a / b;
        const float whole = floorf(q);
        const float p = b * whole;
        out.n = a - p;
      }
      break;
    case LT: out.n = a < b; break;
    case LE: out.n = a <= b; break;
    case GT: out.n = a > b; break;
    case GE: out.n = a >= b; break;
    case EQ: out.n = a == b; break;
    case NE: out.n = a != b; break;
    case AND: out.n = a != 0 && b != 0; break;
    case OR: out.n = a != 0 || b != 0; break;
    case NOT: out.n = a == 0; break;
    case NEG: out.n = -a; break;
    case COND: out.n = a != 0 ? b : c; break;
    case MIN: out.n = lesser(a, b); break;
    case MAX: out.n = greater(a, b); break;
    case CLAMP: out.n = lesser(greater(a, b), c); break;
    case FLOOR: out.n = floorf(a); break;
    case ROUND: {
      const float half = a + 0.5f;
      out.n = floorf(half);
      break;
    }
    case ABS: out.n = fabsf(a); break;
    case LERP: {
      const float d = b - a;
      const float m = d * c;
      out.n = a + m;
      break;
    }
    case LEN: out.n = in[0].list >= 0 ? static_cast<float>(lists_[in[0].list].size()) : 0; break;
    case IDX:
      if (in[0].list < 0) return 0;
      out = item(in[0].list, b);
      break;
    case RAND: out.n = rand(a, b); break;
    case BLEND: {
      if (in[0].list < 0) return 0;
      std::vector<float> stops;
      for (const Value &v : lists_[in[0].list])
        if (v.list < 0) stops.push_back(v.n);
      if (stops.empty()) break;
      if (stops.size() == 1) {
        out.n = stops[0];
        break;
      }
      const float t = lesser(greater(b, 0.0f), 1.0f);
      const size_t n = stops.size();
      const float tt = t * static_cast<float>(n - 1);
      size_t index = static_cast<size_t>(tt);
      if (index > n - 2) index = n - 2;
      const float mix = tt - static_cast<float>(index);
      const uint32_t from = static_cast<uint32_t>(stops[index]), to = static_cast<uint32_t>(stops[index + 1]);
      uint32_t colour = 0;
      for (int shift = 16; shift >= 0; shift -= 8) {
        const float ca = static_cast<float>((from >> shift) & 0xFF), cb = static_cast<float>((to >> shift) & 0xFF);
        const float d = cb - ca;
        const float m = d * mix;
        const float v = ca + m;
        const float half = v + 0.5f;
        colour |= static_cast<uint32_t>(floorf(half)) << shift;
      }
      out.n = static_cast<float>(colour);
      break;
    }
    default: return 0;
    }
    stack[sp++] = out;
  }
  return sp ? stack[sp - 1].n : 0;
}

std::string Runtime::format(float value) const {
  char text[48];
  if (isfinite(value) && value == floorf(value) && fabsf(value) < 1e9f) {
    snprintf(text, sizeof(text), "%ld", static_cast<long>(value));
    return text;
  }
  snprintf(text, sizeof(text), "%.2f", static_cast<double>(value));
  std::string out = text;
  if (out.find('.') != std::string::npos) {
    while (!out.empty() && out.back() == '0') out.pop_back();
    if (!out.empty() && out.back() == '.') out.pop_back();
  }
  return out;
}

// ----------------------------------------------------------------- driving --

void Runtime::send(Binding &binding) {
  const float value = evaluate(binding.program);
  if (binding.sent && binding.last == value) return;
  // -0 and 0 are one value, as in Python.
  binding.sent = true;
  binding.last = value;
  if (host_) host_->prop(binding.target, binding.prop, value);
}

void Runtime::armAfter() {
  for (Timer &timer : after_) {
    timer.armed = false;
    for (const Entry &entry : entries_)
      if (entry.event == timer.event && (entry.page == 0 || entry.page == page_)) {
        timer.armed = true;
        timer.due = now_ + static_cast<uint32_t>(atol(timer.event.c_str() + 6));
        break;
      }
  }
}

void Runtime::start(uint32_t now) {
  now_ = now;
  lastInput_ = now;
  page_ = start_;
  if (host_) host_->page(page_);
  for (Timer &timer : every_) {
    timer.armed = true;
    timer.due = now + static_cast<uint32_t>(atol(timer.event.c_str() + 6));
  }
  armAfter();
  for (Binding &binding : bindings_) send(binding);
  for (size_t i = 0; i < entries_.size(); i++)
    if (entries_[i].event == "when") whenState_[i] = evaluate(entries_[i].cond) != 0;
}

void Runtime::settle(uint32_t changed) {
  for (int depth = 0; depth < WHEN_DEPTH; depth++) {
    if (!changed || ended_) return;
    for (Binding &binding : bindings_)
      if (binding.deps & changed) send(binding);
    std::vector<size_t> fired;
    for (size_t i = 0; i < entries_.size(); i++) {
      const Entry &entry = entries_[i];
      if (entry.event != "when" || !(entry.condDeps & changed)) continue;
      const bool now = evaluate(entry.cond) != 0;
      if (now && !whenState_[i]) fired.push_back(i);
      whenState_[i] = now;
    }
    changed = 0;
    for (size_t i : fired) {
      const Entry &entry = entries_[i];
      if (entry.page && entry.page != page_) continue;
      if (entry.guarded && evaluate(entry.guard) == 0) continue;
      changed |= run(entry, "when");
      if (ended_) return;
    }
  }
}

bool Runtime::idleActive(const std::string &event) const {
  for (const Entry &entry : entries_)
    if (entry.event == event && (entry.page == 0 || entry.page == page_)) return true;
  return false;
}

void Runtime::tick(uint32_t now) {
  now_ = now;
  while (!ended_) {
    // The earliest timer due: after, then every, then idle, then the order of their events.
    Timer *chosen = nullptr;
    int kind = 0;
    uint32_t at = 0;
    auto consider = [&](Timer &timer, int k, uint32_t due) {
      if (static_cast<int32_t>(now - due) < 0) return;
      if (!chosen || static_cast<int32_t>(due - at) < 0 ||
          (due == at && (k < kind || (k == kind && timer.order < chosen->order)))) {
        chosen = &timer;
        kind = k;
        at = due;
      }
    };
    for (Timer &timer : after_)
      if (timer.armed) consider(timer, 0, timer.due);
    for (Timer &timer : every_)
      if (timer.armed) consider(timer, 1, timer.due);
    for (Timer &timer : idle_)
      if (!timer.armed && idleActive(timer.event))
        consider(timer, 2, lastInput_ + static_cast<uint32_t>(atol(timer.event.c_str() + 5)) * 1000u);
    if (!chosen) break;
    if (kind == 0) {
      chosen->armed = false;
    } else if (kind == 1) {
      const uint32_t period = static_cast<uint32_t>(atol(chosen->event.c_str() + 6));
      // Each period fires, unless the host fell far behind: then from now.
      chosen->due = static_cast<int32_t>(at + 10 * period - now) > 0 ? at + period : now + period;
    } else {
      chosen->armed = true; // fired: not again until an input
    }
    const std::string event = chosen->event;
    dispatch(event, false, now);
  }
  if (clockKnown_ && !ended_) {
    const int64_t second = local();
    if (second != second_) {
      second_ = second;
      settle(TIME);
    }
  }
}

void Runtime::setClock(uint32_t epoch, int32_t offset, uint32_t now) {
  now_ = now;
  clockKnown_ = true;
  epoch_ = epoch;
  offset_ = offset;
  clockAt_ = now;
  second_ = local();
  settle(TIME);
}

void Runtime::setTilt(float x, float y, float z, uint32_t now) {
  now_ = now;
  if (x == tilt_[0] && y == tilt_[1] && z == tilt_[2]) return;
  tilt_[0] = x;
  tilt_[1] = y;
  tilt_[2] = z;
  settle(TILT);
}

void Runtime::dispatch(const std::string &event, bool input, uint32_t now) {
  now_ = now;
  if (ended_) return;
  if (input) {
    lastInput_ = now;
    for (Timer &timer : idle_) timer.armed = false;
  }
  for (int pass = 0; pass < 2; pass++)
    for (const Entry &entry : entries_) {
      if (entry.event != event || (pass == 0 ? entry.page != page_ : entry.page != 0)) continue;
      if (entry.guarded && evaluate(entry.guard) == 0) continue;
      settle(run(entry, event));
      return;
    }
}

uint32_t Runtime::run(const Entry &entry, const std::string &event) {
  uint32_t changed = 0;
  for (const Action &action : entry.actions) {
    switch (action.verb) {
    case SET: {
      const float value = evaluate(action.program);
      if (value != vars_[action.a]) {
        vars_[action.a] = value;
        changed |= 1u << action.a;
      }
      break;
    }
    case PAGE_TO: {
      int number = action.a;
      if (!action.text.empty()) {
        const int n = static_cast<int>(pages_.size());
        int index = 0;
        for (int i = 0; i < n; i++)
          if (pages_[i] == page_) index = i;
        number = pages_[(index + (action.text == "next" ? 1 : n - 1)) % n];
      }
      bool known = false;
      for (int p : pages_) known |= p == number;
      if (!known) break;
      page_ = number;
      if (host_) host_->page(number);
      armAfter();
      changed |= PAGE;
      break;
    }
    case REPORT: {
      std::string text;
      for (const Part &part : action.parts) text += part.text ? part.value : format(evaluate(part.program));
      if (text.size() > 64) text.resize(64);
      if (host_) host_->report(text, event, page_);
      break;
    }
    case END:
      if (slide_) break;
      if (host_) host_->end();
      ended_ = true;
      return changed;
    case STATE:
      if (host_) host_->state(action.a, action.text);
      break;
    case MOVE:
      if (host_) host_->move(action.a, action.ordinal);
      break;
    case WANDER:
      if (host_) host_->wander(action.a, action.flag);
      break;
    case SHOW:
    case HIDE:
      if (host_) host_->show(action.a, action.verb == SHOW);
      break;
    case SOUND:
      if (host_) host_->sound(action.text);
      break;
    }
  }
  return changed;
}

} // namespace SceneRuntime
