# Scene runtime 2: values, variables, bindings

A scene was meant to be a small program an agent sends without a firmware
build. Until 2026-10-21 every new kind of scene added its own feature and its
own token: glyphs for a clock, hands for a dial, ranges, sweeps, octaves and a
tone for the instrument, colour variants, avoidance; nine tokens, each a
firmware release. The format had objects and an event → action table, but
nothing to compute with, so anything that changes over time became a feature.

Runtime 2 is the small core those features were special cases of: values the
board has, variables, expressions, properties bound to expressions, guarded
actions, and two more events. The firmware declares one token, `scene2`, and
hardware tokens besides. The old features stay as shorthands the server
compiles down to the core (below), so they cost the firmware nothing.

Code: `backoffice/scene_runtime.py` (the expression compiler and the
reference runtime), `backoffice/scenes.py` (validation, the shorthands,
`_runtime_manifest`, `render_view`), `esp32/src/scene_runtime.cpp`
(the runtime on the board) and `scene_player.cpp` (its host: LVGL, buttons,
tilt, sounds). Tests: `tests/test_scene_runtime.py`. The agent's guide:
`agent_skill/skills/wignet/references/scenes.md`.

## Where it stops

The language stops here, on purpose: **no user functions, no loops, no
nested ifs, no strings, no objects made while playing.** A scene that needs
more is a reason to look at a real interpreter (Berry, Lua) behind the same
host, not to grow this one. Before adding a token, express the scene with
what is here; `tests/scene_runtime/targets/` holds the ten that must keep
compiling without one.

## The scene an agent writes

On a Wignet whose `get_capabilities` scene block has `"runtime": 2`:

```json
{"design": [240, 240],
 "vars": {"left": 1500, "running": 0},
 "assets": {"digits": {"image_url": "…", "frame": [35, 49], "pixel": true}},
 "sounds": {"ding": {"tune": "A t120 o5 c8 e8 g4"}},
 "pages": [{"page": 1, "bg": {"color": "#1d1d1d"}, "bind": {"color": "running ? #1d2b1d : #1d1d1d"}}],
 "objects": [{"page": 1, "id": 1, "obj": "sprite", "sheet": "digits", "anchor": "center", "x": -63,
              "bind": {"frame": "floor(left / 600)"}},
             {"page": 1, "id": 5, "obj": "rect", "w": 200, "h": 8, "color": "#d56668", "anchor": "bottom-left",
              "x": 20, "y": -30, "bind": {"w": "200 * left / 1500"}}],
 "on": {"a.short": "set running !running",
        "every:1000": {"if": "running && left > 0", "do": "set left left - 1"},
        "when:left == 0": "set running 0; sound ding; report done",
        "b.long": "set left 1500; set running 0"}}
```

- **`vars`**: up to 16 numbers, by name, with their value at the start.
- **`lists`**: up to 16 constant lists of numbers or `#rrggbb` colours (32
  items), or lists of such lists (one level).
- **Expressions**: `+ - * / %`, `< <= > >= == !=`, `&& || !`, `?:`,
  `name[i]`, numbers, `#rrggbb`, `true`/`false`; functions `min max clamp
  floor round abs lerp blend len`, and `rand(lo, hi)` in `set` only (a
  binding shows the same for the same values). At most 200 characters and 64
  compiled steps. The values: `time.h`, `time.m`, `time.s` (the board's local
  time), `time.known` (0 until the first /schedule gave it), `tilt.x`,
  `tilt.y`, `tilt.z`, `page`.
- **Numbers** are 32-bit floats. `x / 0` and `x % 0` are 0; `%` takes the
  sign of the divisor (`-7 % 3` is 2); `list[i]` wraps both ways (`[-1]` is
  the last); `round` is half up; `blend(list, t)` mixes colour stops, `t`
  from 0 to 1.
- **`bind`** on an object: `img` takes `dx dy rotation visible opacity`,
  `sprite` also `frame`, `rect` (a box of one colour, `w` x `h`, `color`)
  `dx dy w h color visible opacity`. A page with a `color` background binds
  `color`. `dx`, `dy`, `w`, `h` are in design units (pixels without design);
  `rotation` in degrees clockwise around the picture's centre; `opacity` 0
  to 100; `frame` wraps over the sheet. A bound property decides alone: a
  sprite whose frame is bound has no states, one whose `dx`/`dy` is bound
  does not move or wander, one whose `visible` is bound is not shown or
  hidden by an action.
- **Entries**: an action string as before, or `{"if": expression, "do":
  actions}`, or a list of up to 4 of them: the first whose `if` holds runs.
  The page's entries come before the scene's, as before.
- **Events**: as before, plus `every:<ms>` (100 to 86400000, from the start,
  whatever the page) and `when:<expression>` (when it turns true: not at the
  start, once per rising edge). Both take a `p<n>:` scope.
- **Actions**: as before, plus `set <variable> <expression>`; a `report`
  value may hold `{expressions}`: `report score {score} of 3` (a whole
  number as an integer, else two decimals at most).
- **The tone** (`scene.tone`, with a speaker): `{"wave", "volume",
  "glide_ms", "bind": {"pitch": <MIDI note: 60 middle C>, "on": <0 or not>}}`.

`push_scene` with `dry_run` takes `simulate`: steps from the start,
`{"at": ms, "event": name}`, `{"at": ms, "tilt": [x, y]}` or `{"at": ms}`,
and `rendering.scene.simulated` answers the page, the variables and the
reports after them; the preview is that page.

## Shorthands

Validated by the server and compiled down to the core, so a scene written
for runtime 1 plays unchanged:

| Shorthand | Becomes |
|---|---|
| `glyphs` (`HH:MM`, `H:MM`, `HH:MM:SS`, `blink`, `gap`) | a `group` (a container, ids as the glyphs') of sprites 100+ whose `frame` is bound to a digit of the time (`scenes.GLYPH_CELLS`); a colon blinks by `visible`; `H:MM` before 10 shifts its digits by binding |
| `hand` | an `img` at its top-left, `rotation` bound (`scenes.HAND_ROTATION`) |
| `ranges` and a `sweep` background | the list `_ranges`, the variable `_range`, the page's `color` bound to `blend(_ranges[_range], (tilt + 1) / 2)` |
| the tilt `tone` (`source`, `octaves`, `span`, `on`) | `_octaves`, `_octave`, `_tone`; `pitch` bound to `60 + 12 * (_octaves[_octave] - 4) + (tilt + 1) / 2 * span` |
| `range`, `octave`, `tone` actions | `set` on those variables |
| a colour variant (`from`, `map`) | a file of its own (deflated, it costs next to nothing to download; held raw, within `bundle_bytes`, 3 MB) |

Agents' names never start with `_`.

## The manifest (v 2)

As runtime 1's (`docs/scenes.md`), with:

- `v: 2`, `scale` (screen pixels per design unit), `vars` (their values,
  numbered in the scene's order), `lists` (numbers; a nested list is
  `{"l": <index>}`, its lists numbered after the named ones).
- objects `img`, `sprite`, `rect` (`w`, `h`, `color` as `0xRRGGBB`) and
  `group` (`w`, `h`); a group's sprites follow it with `parent` and a place
  relative to it. No `glyphs`, `hand`, `ranges`, `sweep` or `scene_recolor1`
  variants.
- `bind` on objects, pages and `tone`: `{prop: program}`.
- an entry may carry `if` (a program); a `when` entry is `{"event": "when",
  "cond": program}`; actions add `["set", variable, program]`, and a report
  is `["report", [text or program, …]]`.

A program is postfix: numbers, `$n` a variable, `#n` a list, `@h @m @s @k`
time, `@x @y @z` tilt, `@p` the page, and the operations `+ - * / % < <= >
>= == != && || ! neg ? min max clamp floor round abs lerp blend len idx
rand`. The board needs no parser; it reads what each program depends on
from it.

## The runtime

`Runtime` (Python) and `SceneRuntime::Runtime` (C++) answer the same calls:
`start`, `tick`, `dispatch`, `set_clock`, `set_tilt`; what the host does
comes back as effects (`page`, `state`, `move` by ordinal, `wander`,
`show`/`hide`, `sound`, `report`, `end`, `prop`). Rules both keep:

- A binding is sent when its value changes, never twice the same.
- After anything changes (an action, a page, a new second, the tilt), the
  bindings and `when`s that read it are evaluated again; a `when` that turns
  true runs, which may change more, 8 rounds at most.
- Timers: `after:` from the page showing, `every:` from the start (each
  period fires; a host 10 periods behind fires once and goes on from now),
  `idle:` once per quiet spell. Several due: the earliest, then after,
  every, idle, then the order of their events.
- `end` stops the actions that follow; in a slide it does nothing.
- `rand` is xorshift32; the board seeds it from `esp_random`.

**They agree to the float.** `tests/scene_runtime/corpus/` holds timelines
(the ten targets and one of every operation) with the lines the Python
reference prints; `test_the_firmware_runtime_answers_every_timeline_like_the_reference`
builds `scene_runtime.cpp` with the host compiler (`-std=c++11
-ffp-contract=off`, as the firmware: a fused multiply-add would round
differently) against ArduinoJson from PlatformIO's library folder, and
compares line for line. Change the reference, then
`WIGNET_WRITE_CORPUS=1 pytest tests/test_scene_runtime.py` and make the C++
agree.

## The player

`scene_player.cpp` reads only `v: 2` bundles. A scene compiled for runtime
1 before is compiled again when the board first declares `scene2`: the
scene runtime is part of what renders again when it changes
(`schedule_response._screen`), and `publications.scene_keeps_turn` keeps a
bundle only for its own runtime.

**Runtime 1 is retired** (2026-10-21): the server compiles runtime 2 only and
knows none of `scene1`, `scene_sound1`, `scene_tone1`, `scene_turn1`,
`scene_recolor1`, `scene_avoid1`. A board still declaring them gets no
scenes, its slides as before, until it runs a `scene2` firmware. With
colour variants now files, `bundle_bytes` became what the board holds (3
MB, every asset raw) rather than 1.5 MB downloaded.
It calls `tick` every 20 ms, reads the tilt then only when a binding, a
`when` or a motion event needs it, and turns each `prop` into LVGL: a
position (its place plus `dx * scale`), an image source (`frame`),
`lv_image_set_rotation` around the centre, the hidden flag, `opa`, a size,
a colour. The tone's pitch becomes `440 * 2^((pitch - 69) / 12)` Hz.

### Seen on the board (2026-10-21)

SideWig (the 1.54", `02:00:00:00:00:08`) against production, after #261
was deployed, the target scenes pushed with stand-in pictures inline:

- **A scene from before the deploy** (the robot, 973,140 B of assets) was
  compiled again for runtime 2 on its own when the board first declared
  `scene2`, downloaded and played: no push needed.
- **Instrument**: the tone's pitch followed the left/right tilt and the
  background the forward/back tilt; PLUS changed the colour range, BOOT the
  octave, a short PWR press stopped the tone (`[Audio] Tone ended: 5196
  buffers, 0 dropouts`).
- **Mail**: three `send_scene_event mail` reached it at the next poll and
  the badge read 3; BOOT cleared it and `read` came back in `get_inputs`
  within a second.
- **Dice**: seven rolls by BOOT and shake, each a new face, each reported
  (`rolled 4`, `rolled 1`…): `rand`, `set` and `{face}` on the board.
- **Pomodoro**, started at 10 s: the digits counted down, the bar shrank,
  the background turned green while running, and at zero the `when` fired
  (`done` reported 9 s after BOOT). The chime's action ran; nobody was
  sure to have heard it.
- **Quiz**: page-scoped answers, then `when:page == 4` reported the score
  (`score 0 of 3` for B, A, B).
- **Analogue clock**: the right time, the second hand ticking (the `hand`
  shorthand as `rotation` bindings).

Not yet seen: the digital clock, the pet, the Clawd field; a turn on the 2".
One firm shake fired `shake` four times, a second apart (the motion re-arm,
unchanged from runtime 1).

Measured 2026-10-21: the 1.54" build is 1,477,217 B of flash (22.5%, up
13.5 KB from the runtime 1 player with recolor), static RAM 104,948 B; the
2" 1,334,333 B, 105,404 B.

## Not yet: indexed pictures

If a scene ever needs more than `bundle_bytes` (3 MB held), the next step is
palette pictures, not another shorthand. Looked at on 2026-10-21 and left
for when memory pinches:

- LVGL 9.6 draws I4/I8 pictures (a palette of ARGB8888 entries, then
  indices). With `LV_BIN_DECODER_RAM_LOAD` 0, as here, it converts them a
  line at a time while drawing, so the board holds only the indices. The
  Clawd sheet has 10 colours with transparency: I4 holds a Clawd in about
  61 KB instead of 355 KB, the field of eight in about 485 KB instead of
  2.84 MB. A colour variant is the same indices with another palette (a
  copy each: LVGL wants the palette right before them).
- A picture decoded line by line cannot be turned or scaled
  (`lv_draw_image.c`: "Partially decoded images cannot be transformed"):
  the server would keep RGB565A8 for any picture with a bound `rotation`.
- It needs `LV_DRAW_SW_SUPPORT_ARGB8888` on: +12,388 B of flash on the
  1.54", the same RAM (built 2026-10-21). Each redraw converts every line
  through the palette: draw time to be measured on the 1.54" first.
- The server would choose the format per picture; agents would see nothing
  but a smaller `bundle_bytes`. The firmware needs a flash and a token for
  the format.
