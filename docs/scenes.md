# Scenes: what a screen plays by itself

Issue #229. A scene is a small interactive experience an agent pushes without
building firmware: pages, pictures and sprites, and actions on the screen's
buttons, its motion, a touch, a timer, or an event the agent sends. The
screen plays it locally between two contacts (LVGL on the LCD boards); the
server keeps every pixel. Agent-facing guide:
`agent_skill/skills/wignet/references/scenes.md`.

**The player runs the scene runtime 2** (`docs/scene_runtime.md`, since
2026-10-21): variables, lists, properties bound to expressions, guarded
actions, `every:` and `when:`. The glyphs, hands, instrument and colour
variants below are shorthands the server compiles down to that core; the
firmware knows none of them. Runtime 1 (`scene1` and its feature tokens,
2026-10-17 to 2026-10-21) is retired: a board declaring only those gets no
scenes, its slides as before. This page covers what the agent writes, the
delivery, events, reports, turning and protected interactions.

Code: `backoffice/scenes.py` (validation, compilation, the runtime fields),
`publications._prepare_scene` (a `scene` publication), `platform_device.py`
(`/scene/…`, `/input`), `schedule_response.py` (`in=`, `sev=`, the `scene`
fields), `platform_api.scene_action` and `platform_mcp.py` (the three tools).
Tests: `tests/test_scenes.py`.

## Who can play one

Only a client that declares `scene2` on `/schedule`, and only on a
full-colour (`rgb`) screen. Firmware declares `scene2` only on a screen that
redraws in under a second (LCD, OLED, HDMI): a scene animates and reacts to a
press at once, which e-paper cannot (`HARDWARE.md`). Otherwise
`effective_capabilities()['scene']` is `null`, the three tools are left out of the catalogue, and the handlers refuse
("This Wignet can't play scenes"). The catalogue without a device (the
account-wide MCP, the generic OpenAPI, the ChatGPT app) never lists them
(`platform_mcp.SCENE_TOOLS`): until a scene firmware ships, every call
there would be refused, and the published app's tool list stays as it was.
`llms.txt` mentions scenes without naming the tools.

| Token / parameter | Meaning |
|---|---|
| `scene2` | the firmware runs the scene runtime 2 (`docs/scene_runtime.md`): it turns a scene itself ("Turning the screen"), its wandering sprites avoid each other, and with `audio_out` it plays `sound` actions and the tone |
| `audio_out` | the speaker: `scene.sound` and `scene.tone` |
| `motion1` | it reports `tilt.*`, `shake`, `face_down` |
| `touch1` | it reports `p<page>b<id>.up|down` |
| `pwr_short1` | a short press of PWR reaches a scene as `pwr.short` (the hold stays the board's) |
| `in=a,b` | the logical buttons a scene may bind (`^[a-z][a-z0-9_]{0,15}$`, 8 at most). Stored as `devices.reported_inputs` (JSON), only with `scene2`. `pwr`/`power` are dropped |

Example: `/schedule?mac=…&caps=v1:rgb,screen_size1,orientation_report1,scene2,motion1&w=320&h=240&ori=0&in=a`.

## Protected interactions

Some gestures belong to the firmware and can never reach a scene, whatever
its manifest says. The firmware's own table is the guarantee; the server's
refusals only give the agent a clear error.

- **Power.** A long press on PWR switches a board off (the 1.54" releases its
  power hold after 2 s; the PhotoPainter's PWR is wired to its power
  controller). PWR is never declared in `in=`, and a scene naming
  `pwr.*`/`power.*` is refused, except `pwr.short` on a board declaring
  `pwr_short1`: a press let go before the 2 s (`PowerHoldHAL::takeShortPress`).
- **The way out.** A hold of 5 s or more on any button leaves the scene for
  the slides. `<button>.hold` is refused; the gestures a scene may bind are
  `down`, `up`, `short`, `long`.
- **Alarms and sounds first.** A ringing alarm or a pushed sound takes every
  press (to stop it) before the scene sees it (a scene's own sound does not);
  on the PhotoPainter PWR pulses by itself while the amplifier plays (`HARDWARE.md`), hence its 300 ms
  hold rule.
- **Waking** is not an input: a press that wakes a sleeping board is consumed.
- **Turning** belongs to orientation (`orientation_report1`): the player
  moves the scene to the page held now by itself (below).

Input reaches a scene through one pipeline, in order: power → alarm/sound →
system (wake, orientation, the way out) → scene.

## The scene an agent writes

See the skill reference for the agent's view. Limits (`scenes.LIMITS`): 8
pages, 16 objects, 64 `on` entries (32 until 2026-10-21; the player has no
cap of its own), 4 actions per entry, 12 assets, sprite
frames up to 128x128 and 64 per sheet, 32 frames per state, 3 MB held on
the board (`bundle_bytes`, every asset raw, colour variants included; until
2026-10-21 1.5 MB downloaded and 3 MB held), 12 colours a variant.
Every reference (page, id, state, asset, button, gesture, sensor) is checked
by `scenes.validate`, so the firmware never meets a broken one. Pictures come
from `image_url` or `image_base64` + `image_sha256` (`publications.acquire`).

## Any screen size: design units and anchors

A scene may be drawn once for every screen: `"design": [w, h]` is the page
it is drawn for, in design units, and each object is placed against an
`anchor` of the page plus an offset (`x`, `y`, negative allowed) in those
units. Without `design` a scene is in the screen's own pixels, exactly as
before (`x`, `y` absolute; clamped on a re-compile).

```json
{"design": [320, 240],
 "assets": {"clawd": {"image_base64": "…", "image_sha256": "…", "frame": [126, 112], "pixel": true}},
 "objects": [{"page": 1, "id": 1, "obj": "sprite", "sheet": "clawd", "anchor": "center", "y": -4,
              "portrait": {"y": 36}, "states": {…},
              "do": {"wander": {"anchor": "bottom", "w": 300, "h": 60}, "speed": 30}}],
 "on": {"event:done": "move 1 bottom-right -10 -10", "event:back": "move 1 0 0"}}
```

- **Anchors** (`scenes.ANCHORS`): `top-left` (default), `top`, `top-right`,
  `left`, `center`, `right`, `bottom-left`, `bottom`, `bottom-right`. That
  point of the object (its frame; a glyphs string's whole width; a hand's
  square) goes on that point of the page, plus the offset times the scale,
  rounded half up. Objects are then kept on the page where they fit.
- **Per orientation**: `"landscape": {…}` / `"portrait": {…}` on an object
  changes `anchor`, `x`, `y` and its `wander` area for that page shape. A
  square page has one shape, the design's own.
- **`move <id> [anchor] <x> <y>`**: an offset from the object's own anchor
  (for that orientation) unless one is named; `move 1 0 0` sends it home.
  **`do.wander`** is an area `{anchor, x, y, w, h}`, placed like an object,
  its size scaled and kept within the page.
- **Scale** (`scenes.design_scale`): long side against long side, short
  against short, the smaller ratio: 1 for a 320x240 design on the 2" either
  way up, 0.75 on the 1.54". The same both ways, so the pictures are scaled
  once and serve both orientations. Speeds and glyph gaps follow it.
- **Pictures**: `"pixel": true` (pixel art) finds the art's cell, the
  largest step every frame edge and colour change falls on
  (`scenes.pixel_cell`), and scales it to a whole number of screen pixels per
  cell, never below one, nearest-neighbour: 7 px cells become 5 on the 1.54"
  (0.75), not a blur. Anything else is scaled smoothly (Lanczos). The limits
  (`sprite_frame_px`, `bundle_bytes`, a picture at most the page) apply to the
  scaled result, and the refusal says what the frames became at which scale.
- **Backgrounds**: a colour costs nothing; an image or text background is
  drawn (cover or contain) for each page shape the bundle carries, so a
  turning board downloads two.

The `push_scene` answer's `rendering.scene` gives `design`, `scale`, and
`placed` (each object's place on the page compiled for), and `layouts`
(each page shape's size and places): an agent can
check a layout with `dry_run`, without a screen.

## Colour variants: one sheet, many colours

An asset may be another one with exact colours swapped:

```json
"assets": {"clawd": {"image_base64": "…", "image_sha256": "…", "frame": [78, 66], "pixel": true},
           "clawd2": {"from": "clawd", "map": {"#d97757": "#5896de", "#a85238": "#34628f"}}}
```

- Same frames and size as its source, scaled with it (`pixel: true` is the
  source's); each colour of `map` is replaced exactly, everything else
  (eyes, outline, alpha) is the source's. Colours compare as RGB565, what
  the board holds: two keys that are one RGB565 colour are refused, and so
  is a key no opaque pixel of the compiled source has (the refusal lists the
  source's most used colours). At most 12 entries; the source exists and is
  not itself derived; a variant is used by an object like any asset, and a
  source used only through its variants is kept. Not a page background
  (the server draws those for the page).
- One pass, as on the board: a pixel replaced is not replaced again, so
  `{"#aa0000": "#0000aa", "#0000aa": "#aa0000"}` swaps two colours.
- **A file of its own.** The server recolours the source
  (`scenes.recolor_asset`) and sends the variant like any asset. Deflated
  (`z=1`) a flat-coloured variant costs a few KB to download; held raw, it
  counts toward `bundle_bytes`. Until 2026-10-21 a `scene_recolor1` player
  made variants itself from the source, the only way the Clawd field fitted
  the then 1.5 MB download limit.
- **Size.** `bundle_bytes` (3 MB) bounds the sum of every asset held raw,
  variants included; the `push_scene` answer's `rendering.scene` gives it.
- **Why 3 MB.** The boards have 8 MB of PSRAM, and a new scene is loaded
  beside the one playing before it takes over, so two bundles must fit.
  Measured 2026-10-21 on SideWig2 with the Clawd field below holding
  2,718,144 B: 5,281,999 B of PSRAM still free, room for a second scene at
  the limit.

Measured 2026-10-21 on SideWig2 (`02:00:00:00:00:07`) on the bench platform:
the Clawd field, one orange sheet of 22 frames of 78x66 (2,291 B of PNG,
339,768 B, 3,216 B deflated) and seven two-colour variants of it. The board
downloaded the manifest and `clawd.bin` only, made each variant in 62 to
65 ms (0.44 s for the seven), and played about 2 s after fetching the
manifest. Each variant's sha256, logged by the board, was the one of the
file the server now sends (`scenes.recolor_asset`). As files the same
field holds 2,718,144 B, which the 1.5 MB download limit refused then and
the 3 MB held limit takes now (`test_the_clawd_field_of_eight_fits_the_2_inch_as_files`;
the field `~/.claude/wignet-clawd` builds, 2,841,696 B on the 2" both ways up). The 2" build is 1,322,877 B of flash
(20.2%), static RAM 105,452 B; the 1.54" build 1,466,057 B (22.4%), 104,972
B. Seen on the 1.54": not yet.

## Sprites that avoid each other

`do.avoid` on a sprite (beside `wander`): `true`, its whole frame, or
`[x, y, w, h]`, the part of the frame that meets others (a body, not the air
around it), in the sheet's pixels (design units in a design scene, scaled
with the sheet: the manifest's `avoid` is in screen pixels).

```json
"do": {"wander": {"anchor": "center", "w": 320, "h": 240}, "speed": 24, "avoid": [9, 30, 36, 33]}
```

- Every 40 ms (`tickAvoid` in `scene_player.cpp`)
  a sprite that is wandering, shown, on the page shown, whose box meets
  another shown sprite's box with `avoid` while its move heads towards it,
  stops and goes to a random point of its area away from it (the first of
  up to 8 picks that goes away). One already heading away is left alone, so
  two that overlap part rather than shake. A sprite whose wandering is off
  (`wander <id> off`) stays put and is met like a wall.
- Only boxes, only between sprites with `avoid`; the server never moves
  anything (it does not know where they are).

Measured 2026-10-21 on SideWig2 (`02:00:00:00:00:07`) on the bench platform,
held portrait (240x320): the Clawd field, eight 78x66 sprites all
wandering with a 36x33 body box, 48 `on` entries loaded. Each turn is
logged (`[Scene] 3 met 5 (1 in all) at …`). Over 2 minutes: 436 turns,
48 to 69 a sprite, no crash. 17% were a second turn of the same sprite
at the same place 40 ms later (crowded: a third of the page is sprites),
in runs of at most 7. A first version, which took any point not
towards the other and one sprite met at a time, made 553 turns, 22% of
them repeated, and once held a sprite caught between two for 12 turns.
The 2" build is 1,324,021 B of flash (20.2%); the 1.54" 1,467,193 B
(22.4%). Seen on the 1.54": not yet.

## Glyphs: values the screen keeps itself

A `glyphs` object shows a value the board provides, picked frame by frame from
a sheet the server drew: the board still renders no text. One source so far:

- `"source": "time"`: the board's local time, `serverEpoch` (every `/schedule`)
  carried forward with its own millisecond counter, plus `utcOffsetSec`, the
  Wignet's current offset from its time zone (`set_local_time`,
  `scenes.utc_offset`). A daylight-saving change reaches it at the next sync.

```json
{"page": 1, "id": 1, "obj": "glyphs", "sheet": "digits", "source": "time",
 "format": "HH:MM", "blink": true, "gap": 7, "x": 18, "y": 95}
```

The sheet's frames are `0`-`9` then `:` (frame 10); `format` is `HH:MM`,
`H:MM` or `HH:MM:SS`; `blink` turns the colon off on odd seconds; the whole
string must fit the page. The first page (`/pic`, previews) shows the time at
compile. On the board: a container of one `lv_image` per character, updated
by an LVGL timer every 250 ms (`tickGlyphs`); nothing shows until the first
`/schedule` gave the time. A scene that only shows the time (a clock) never
needs a `send_scene_event`.

Measured 2026-10-19 on SideWig: a 5x7-cell pixel font scaled 7x, 11 frames of
35x49, 410 B of PNG, 56,595 B on the board.

## Hands: pictures the screen turns with its time

A `hand` turns a picture with a value the board provides; the server still
draws it. The picture is a square, the hand pointing up from its centre; the
board turns it around that centre, which sits at the object's `x`, `y`
(`lv_image_set_rotation`, LVGL's software transform).

```json
{"page": 1, "id": 2, "obj": "hand", "asset": "minute", "source": "time",
 "unit": "minute", "x": 120, "y": 120}
```

`time` units: `hour` (30 degrees an hour, moving with the minutes), `minute`
(6 degrees a minute, moving with the seconds unless `"smooth": false`),
`second` (6 degrees a second). Objects draw in their order, so a centre cap
listed after the hands sits on top. The first page shows the hands at compile
(`scenes.hand_angle`); on the board they are hidden until the first
`/schedule` gave the time, then updated by the same 250 ms timer as glyphs.

Measured 2026-10-19 on SideWig: a dial background and three hands (140, 210
and 230 px squares) plus a cap, 3.4 KB of PNG, 465,768 B on the board; the
second hand turns every second.

## Sounds: short tunes a scene plays

A scene on a Wignet with a speaker (`audio_out`, `get_capabilities`
scene.sound) names its sounds, and an action plays one:

```json
"sounds": {"ding": {"tune_name": "doorbell"}, "boop": {"tune": "A t120 o5 c8 e8 g4", "volume": 40}},
"on": {"a.short": "sound ding; report ding"}
```

- A sound is MML (`docs/mml.md`): `tune`, or `tune_name` from `get_tunes`,
  copied into `tune` at publish (`scenes.expand_sounds`, as an event does).
  8 sounds at most, each at most 10 s; every sound declared must be played by
  an action, and `sound <name>` must name a declared one.
- It plays once, from its first note at full volume: one loop of the tune, cut
  at 10 s (`AudioHAL::startCue`, in the audio task that rings alarms). A
  `volume` left out is the Wignet's (`set_sound_volume`) when the scene is
  pushed: the bundle carries the number.
- A `sound` while another of the scene's plays stops it and starts the new
  one. The buttons stay the scene's: a cue does not take presses as an alarm
  does. The scene ending stops its sound.
- Alarms win: a scene's sound is not played while an alarm or a pushed sound
  rings, and a due alarm stops a playing cue (`AlarmClockHAL::ringIfDue`).

## Instrument: a tone and colours that follow the tilt

On a Wignet declaring `motion1` (and with a speaker for the tone), the board
measures its own tilt, and a scene binds values to it rather than events:

```json
{"pages": [{"page": 1, "bg": {"sweep": "ranges", "source": "tilt.y"}}],
 "ranges": [["#ff3b30", "#ffcc00"], ["#00c7be", "#5856d6"], ["#1b1b3a", "#f5f5f5"]],
 "tone": {"source": "tilt.x", "octaves": [3, 4, 5], "span": 12, "wave": "triangle", "volume": 50},
 "on": {"a.short": "octave next", "a.long": "octave prev", "b.short": "range next",
        "b.long": "range prev", "pwr.short": "tone toggle"}}
```

- `tilt.x` and `tilt.y` go from -1 to 1 in the upright page's frame: `tilt.x`
  is the page's right edge going down, `tilt.y` its top edge going down; full
  at 45 degrees (`ScenePlayer readTilt`, from `OrientationHAL::gravity`).
- `ranges`: up to 8 colour ranges of 2 to 6 stops. A page background
  `{"sweep": "ranges", "source": …}` slides along the current range (-1 the
  first stop, 1 the last); `range next|prev` changes range. The server draws
  the middle of the first range on the first page.
- `tone`: a continuous wave (`sine`, `triangle`, `square`, `saw`) whose pitch
  goes from C of the current octave at -1 to `span` semitones (1-36, 12) higher
  at 1, gliding over `glide_ms` (60); `octaves` (1-7, 4 is middle C) is the
  list `octave next|prev` steps through; `tone on|off|toggle`; `"on": false`
  starts silent. A volume left out is the Wignet's when the scene is pushed.
- The player reads the tilt every 20 ms (`tickTilt`) and redraws the
  background only when its RGB565 colour changes. While a scene follows the
  tilt the screen's turn holds still (`OrientationHAL::follow`), so tilting
  never turns the page under it.
- The tone runs in the audio task like a cue (`AudioHAL::startTone`): buttons
  stay the scene's, a `sound` pauses it and it comes back after, an alarm or
  a pushed sound stops it and it comes back when they end.
- With `motion1` the events `tilt.left|right|up|down` fire past 0.6 and
  re-arm under 0.3, `face_down` with the screen facing down, `shake` on a jolt
  of more than 0.9 g.

## Scenes as slides

A scene is a slide like a text or an image (issue #244), on a Wignet that
plays scenes: `set_slides` and `add_slide` take `{"scene": {...}}` (or a scene's
`publication_id`), refused elsewhere. The slideshow sets the pace:

- `/schedule` names the scene only while its slide shows, with
  `"slide": true` (`scenes.showing_scene`); a pushed scene or an event's has
  `false`. The player takes it down when the rotation moves on and loads it
  again, from its first page, when it comes back.
- In a slide, `end` does nothing, and a 5 s hold starts the scene again
  rather than leaving it (`restartWanted` in the player).
- `send_scene_event` to a scene slide that is not on the screen is refused
  (409), saying when it shows again: events are not kept for later.
- A standing scene (`push_scene` with `standing`) is a slide list of one.

## The bundle the screen downloads

`push_scene` publishes a publication of kind `scene`, as a push (3 h by
default, from download) or standing, exactly like `push_event`; an alarm still
takes over. Its folder (`generated/publications/<id>/`) holds:

- `scene.json`, the manifest;
- `asset_<name>.bin`, one raw file per asset;
- `src_<name>.png`, the pictures as received (what a re-render compiles from);
- the usual `source.png` and `panel.*`: the first page as it first shows, so
  `/pic`, previews and the owner's page work as for any publication;
- `asset_<name>.bin.z`, the asset deflated, made on its first `z=1` request.

### Compressed assets

`GET /scene/<id>/<asset>.bin?mac=…&z=1` answers a zlib stream of the same
bytes (`application/zlib`, its own ETag), kept beside the asset once made
(`platform_device._deflated`). The server's drawings are flat colours, so
they shrink about a hundredfold. Measured 2026-10-19: the robot's four assets
go from 973,140 B to 2,137 B, the analogue clock's from 465,768 B to 4,229 B.
Before this, SideWig took about 38 s to download the robot over the internet,
showing barely 22 s of a 60 s slide.

The player always asks with `z=1`: a body of exactly the asset's `bytes` is
raw (a server that does not compress), a shorter one is inflated with the
ESP32-S3 ROM's `tinfl` into the asset's buffer. Either way the `sha256` of
the manifest checks the result.

### /schedule

A `scene2` client gets three more fields (`utcOffsetSec`, the Wignet's offset
from UTC now, is for `time` glyphs):

```json
"scene": {"id": "5f0c…", "url": "/scene/5f0c….json", "etag": "\"9a1e…\""},
"sceneEvents": [{"seq": 12, "name": "mail"}]
```

`scene` is `null` when no scene is selected (show `/pic` as before). The
`etag` is the manifest's: while it does not change, keep playing. Events are
those sent to this scene and not yet acknowledged, oldest first, 16 at most,
dropped after an hour; acknowledge with `sev=<last seq>` on the next
`/schedule`.

### GET /scene/<id>.json?mac=…

The manifest (compact JSON). Fetching it counts as the delivery
(`record_delivery`: a push's time starts). ETag + `If-None-Match` → 304.

```json
{"v": 1, "w": 320, "h": 240, "turn": 0, "start": 1,
 "assets": {"bg1": {"file": "bg1.bin", "format": "rgb565", "w": 320, "h": 240, "frames": 1,
                    "bytes": 153600, "sha256": "…"},
            "cat": {"file": "cat.bin", "format": "rgb565a8", "w": 48, "h": 48, "frames": 10,
                    "bytes": 69120, "sha256": "…"}},
 "pages": [{"page": 1, "bg": "bg1"}, {"page": 2, "color": 1319997}],
 "objects": [{"page": 1, "id": 1, "obj": "sprite", "asset": "cat", "x": 20, "y": 150,
              "states": {"idle": {"frames": [0, 1], "ms": 400},
                         "happy": {"frames": [2, 3], "ms": 150, "for": 3000, "then": "idle"}},
              "state": "idle", "wander": [0, 100, 320, 240], "speed": 30}],
 "on": [{"page": null, "event": "a.short", "do": [["state", 1, "happy"], ["report", "petted"]]},
        {"page": 2, "event": "a.short", "do": [["page", 1]]},
        {"page": null, "event": "idle:600", "do": [["end"]]}]}
```

- `w`, `h`: the page, upright. `turn`: the clockwise quarter turn from the
  page into the panel buffer (the `orientation.rotation_for` of the
  publication), applied by the screen.
- `layouts`: where everything sits on each page shape the board can take
  (below). The top level stays the page it was compiled for.
- A page has `bg` (an asset name) or `color` (`0xRRGGBB`).
- `on` is a list: `page` `null` acts on every page. Actions are already
  parsed: `["page", n|"next"|"prev"]`, `["state", id, name]`,
  `["move", id, x, y]`, `["wander", id, true|false]`, `["show", id]`,
  `["hide", id]`, `["report", value]`, `["end"]`.

### GET /scene/<id>/<asset>.bin?mac=…

Every asset, colour variants included. Raw, little-endian, no header (sizes are in the manifest), sRGB: the screen
applies its own colour calibration (`waveshare_esp32s3_lcd.cpp`).

- `rgb565`: `w*h*2` bytes, a page background.
- `rgb565a8`: per frame, the colour plane (`w*h*2`) then the alpha plane
  (`w*h`), frames one after the other: LVGL's `LV_COLOR_FORMAT_RGB565A8`.
  The spike's `esp32/spike/make_assets.py` wrote the same layout.

Check `bytes` and `sha256` before using a file. The bundle is immutable: a
new scene, or the same one compiled again, has a new id.

### POST /input?mac=…

What a scene reports (`report` actions), JSON, 4 KB at most:

```json
{"publication_id": "5f0c…", "events": [{"value": "pasta", "event": "p2:a.short", "page": 2}]}
```

1 to 32 events, `value` 1 to 64 characters; `event` (48 characters at most)
and `page` are optional context. 60 reports a minute per screen at most (429
beyond). Stored in `scene_reports` for 7 days; the agent reads them with
`get_inputs`. An unknown screen or scene is a 404: nothing is created.

## The agent's tools

| Tool | What |
|---|---|
| `push_scene` | `scene`, `duration_seconds` / `standing`, `dry_run`, `request_id`; the answer's `rendering.scene` gives pages, objects, events, bundle size and each asset |
| `send_scene_event` | `name`: queued in `scene_events` for the scene on the screen, refused if it has no `event:<name>`; 16 waiting at most |
| `get_inputs` | `since`, `limit`: the reports, oldest first, with `next_since` (`GET /api/owner/<id>/inputs`) |

`get_capabilities` gives the `scene` block (inputs, gestures, motion, touch,
limits) and `skills.scenes`; the per-device guide says which buttons the
screen has.

## Turning the screen

The server lays out and draws; the board picks the layout.

The scene is compiled once for the board's real size with every page shape
it can take: on the 2"
(`orientation_report1`, a 320x240 frame) landscape 320x240 and portrait
240x320; on the square 1.54" (`self_orient1`) the one 240x240 page, which the
board turns as a whole. The manifest then carries:

```json
"layouts": {"landscape": {"w": 320, "h": 240, "objects": {"1": [97, 60]}, "wander": {"1": [10, 180, 310, 240]},
                          "moves": [[183, 118]], "bg": {"2": "bg2"}},
            "portrait":  {"w": 240, "h": 320, "objects": {"1": [57, 140]}, "wander": {"1": [0, 200, 240, 320]},
                          "moves": [[103, 198]], "bg": {"2": "bg2_portrait"}}}
```

- `objects`: each object's place (top-left; a hand's centre), by id.
- `wander`: each wandering sprite's area.
- `moves`: the target of every `move` action, in the order of the `on`
  entries and their actions: the player numbers its moves the same way.
- `bg`: each page's background asset for that shape (`bg<n>` for the page
  compiled for, `bg<n>_<shape>` for the other). Sprites and pictures are
  shared, drawn upright.

A scene without `design` gets `layouts` too: its own coordinates on its
page, kept inside the other one (objects moved in so they fit).

When the board turns, the player (`relayout` in `scene_player.cpp`) picks
the layout whose `w` x `h` is the page held now, swaps the LVGL page size and
the page's turn into the frame, moves every object and wander area there (an
object moved by a `move` goes to that move's target in the new layout) and
redraws. States, timers, the page shown and pending events are untouched:
nothing is downloaded. A scene that follows the tilt still holds the turn.

On the server a new `ori=` still renders every other publication again
(`rerender_later`), but `content_runtime._prepare_again` keeps a scene whose
manifest already has a layout for the new page (`publications.scene_keeps_turn`):
same id, same pending `sceneEvents`. Its first page is drawn at publish for
every turn (`turn90/panel.*`, …), and `/pic` serves the one for the turn held
now, with that `X-Wignet-Turn`, so the board's relayout poll is satisfied at
once.

A bundle compiled before the board declared `scene2` (runtime 1) is
compiled again from its `src_*.png`: a design scene laid out again, an
older one with coordinates clamped (`scenes.validate(clamp=True)`). That
bundle has a new id, starts from its first page, and events sent to the
old id are no longer delivered.

## The player (firmware)

`esp32/src/scene_player.cpp`, on both LCD boards (`WIGNET_SCENES`
in `waveshare_esp32s3_lcd_154` and, since 2026-10-20,
`waveshare_esp32s3_lcd_2`; LVGL 9.6 with `spike/lv_conf.h`). Since
2026-10-21 it is the host of `scene_runtime.cpp` and plays runtime 2 bundles
only (`docs/scene_runtime.md` "The player"); what follows is the runtime 1
player it replaced, kept for its measurements. It declared
`scene1` on `/schedule` with the buttons it has: `in=a,b` (BOOT, PLUS) on the
1.54", `in=a` (BOOT) on the 2", which has no power-hold pin either (no
`pwr_short1`). Both declare `motion1` (the QMI8658). It:

- plays a page of the manifest's `w` x `h`, which is the frame or, on a
  board that reports how it is held (`orientation_report1`, the 2"), the
  frame turned a quarter: LVGL draws the page upright and `flush` hands
  `DisplayHAL::pushRegion` the manifest `turn`, which turns it into the
  frame. Any other size is refused.

- reads `scene` and `sceneEvents` from each `/schedule` answer; a new `id`
  downloads the manifest and every asset into PSRAM, each checked against its
  `bytes` and `sha256`, before anything changes on screen. A failed download
  is retried after 30 s; `scene: null` (or another id) takes the scene down.
- builds one LVGL container per page (an RGB565 background or a colour),
  `img` and `sprite` objects (`lv_animimg` per state, `for`/`then` with an
  `lv_timer`), wandering with `lv_anim`, the `after:`/`idle:` timers, and runs
  the parsed actions; the page's own entry for an event wins over the scene's.
- while a scene plays, the panel is LVGL's (`DisplayHAL::lendPanel`): the loop
  keeps fetching `/pic` into its frame, shown again when the scene ends.
- buttons are polled from an LVGL timer in the LVGL task. PWR is never read
  (power_hold_hal.cpp: 2 s, off). A press while a sound plays, or within 1 s
  after, is the sound's. A 5 s hold, or an `end` action, takes the scene down
  locally; it is not played again while `/schedule` names the same id.
- events apply in order, and the last `seq` goes back as `sev=`; `report`
  values are posted to `/input` from the loop.

- with `scene_turn1`, moves the scene to the page for how the board is held
  when it turns (`ScenePlayer::onTurn`, above "Turning the screen").

Not yet: touch, drawing
`/pic` pages through LVGL too (one compositor). A scene compiled again on the
server (a new size or profile, or a turn on a player without `scene_turn1`)
gets a new id and starts again from its first page.

### Measured on the bench (2026-10-17)

SideWig (`02:00:00:00:00:08`) against the platform on the Mac (below), the
robot scene: three 240x240 pages, a 4-frame 64x64 gears sprite and a 2-frame
96x96 bubble, 450,048 B of assets. Downloaded and checked in under a second
on the LAN; `event:question` sent with `send_scene_event` showed page 2 at
the next poll (7 s) and was acknowledged with `sev=1`; BOOT on that page ran
`report yes; page 1`, posted at once, and `get_inputs` returned it. The 1.54"
build is 1,435,801 B of flash (21.9%), static RAM 103,804 B.

### Measured on the 2" (2026-10-20)

SideWig2 (`02:00:00:00:00:07`) against production, held landscape: a test
scene of two colour pages, a 64x64 sprite with two animated states and a
40x40 picture, 53,952 B of assets. Played 10 s after `push_scene` (the next
poll); `send_scene_event ping` switched to page 2 at the next poll (9 s),
acknowledged with `sev=`; at the end of its 3600 s the scene was taken down
when `/schedule` stopped naming it. The 2" build is 1,315,005 B of flash
(20.1%), static RAM 105,444 B. Not yet seen on the 2": BOOT presses and
reports, the 5 s hold, a tilt event, a scene held portrait.

### Turning on the 2" (2026-10-20)

SideWig2 (`02:00:00:00:00:07`) on the bench platform, firmware declaring
`scene_turn1`, the Clawd scene drawn for `"design": [320, 240]` with
`portrait` overrides and pixel-art sheets: scale 1, both layouts, 1,447,728 B
of assets downloaded once. Turned four times while Clawd animated: each turn
logged `Turned to 0|90: … laid out in 3 ms`, the layout followed at once,
his state (`working`, then `done`) carried through, `/schedule` named the
same id throughout, no asset was fetched again and `/pic` answered the held
turn at once (no "waiting for the new layout"). The 2" build is 1,320,465 B
of flash (20.1%), static RAM 105,452 B; the 1.54" build 1,463,665 B (22.3%),
104,972 B. Seen on the 1.54": not yet. An event queued while a turn happens
is checked by `tests/test_scenes.py`, not yet seen on the board.

### On the bench

Build with `PLATFORMIO_BUILD_FLAGS='-DWIGNET_BENCH_HOST=\"<the Mac's LAN
address>\" -DWIGNET_BENCH_PORT=5050'` to talk to a platform on this machine
(`deploy/application.py` under werkzeug's `run_simple` on `0.0.0.0:5050`, its
own state directory). Do not call `/schedule` by hand as the board without
its `w`/`h`: the server takes it as a new screen, compiles the scene again
and the board restarts it. Flash the normal build afterwards.

## Still to do

- A push channel if the powered
  cadence is too slow for server events.
