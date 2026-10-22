# LVGL spike on the 2" LCD (not merged)

Branch `spike/lcd2-lvgl`, off `feat/lcd-2`, 2026-10-17. Can a Wignet animate a
small character on the board, described by an agent without compiling
firmware, while the server keeps every pixel? This spike puts LVGL 9.6.0 into
the Waveshare ESP32-S3-LCD-2 firmware (`docs/lcd_2.md`) to find out.

**Status: compiles; never run.** No board was attached when it was built
(`/dev/cu.*` had only Bluetooth and the debug console), so every on-device
figure below is either computed from the build or marked as an estimate. The
firmware prints the real ones itself (below, "Measuring on the bench").

## What it does

- Env `waveshare_esp32s3_lcd_2_lvgl` (`esp32/platformio.ini`) extends
  `waveshare_esp32s3_lcd_2` with `lvgl/lvgl@9.6.0`, ArduinoJson and
  `-DWIGNET_LVGL_SPIKE`. The plain env is untouched: its build is the same
  size as before (1,120,541 B; +16 B from the `-dirty` version string).
- `src/lvgl_spike.cpp`: LVGL draws into two 320x40 partial buffers in PSRAM
  (2 x 25,600 B). The flush calls `DisplayHAL::pushRegion()` in
  `src/hal/waveshare_esp32s3_lcd.cpp`, which sends only the panel window the
  rectangle lands on, turned by `MOUNT_TURN` and through the same calibration
  tables as `showFrame()`, in 18 bits, with the panel's INVON left as it is.
- While LVGL runs, `DisplayHAL::lendPanel(true)` makes `showFrame()` send
  nothing: the loop still polls `/schedule` and decodes pages into its frame,
  invisibly.
- BOOT (GPIO0) is an LVGL keypad sending `LV_KEY_ENTER`; the sprite is the
  focused object of its group and gets `LV_EVENT_PRESSED`.
- The scene is a JSON string (`SCENE_JSON`): a 320x240 RGB565 background, a
  48x48 RGB565A8 sheet (10 frames: walk right, happy, walk left), named states
  with frames, frame time, an optional duration and hop, a wander area and
  speed, and `"on": {"button": "happy"}`. The sprite walks to random points in
  the area (`lv_anim` on x and y, eased, walking-left frames when it goes
  left), cycles frames with `lv_animimg`, and on BOOT stops, hops, shows the
  happy frames for 3 s, then wanders again.
- `esp32/spike/make_assets.py` stands in for the server: it draws the
  background and the sheet and writes them in LVGL's native formats
  (`background.bin`, `walker.bin`), embedded as they are
  (`board_build.embed_files`). Nothing is fetched over HTTP yet.
- `lv_timer_handler` runs in a task of its own on core 1 (the loop's core),
  priority 2, one above the loop.

## Measurements

| What | Figure | How |
| --- | --- | --- |
| Flash, plain LCD-2 build | 1,120,541 B (17.1% of 6.25 MB) | `pio run` |
| Flash, spike build | 1,505,429 B (23.0%): +384,888 B | `pio run` |
| of which pictures | 222,720 B: background 153,600, sheet 69,120 (6,912 a frame) | file sizes |
| of which code and constants | about 162 KB: LVGL (image, animimg, label, default theme, Montserrat 14), ArduinoJson, the spike | the rest |
| Static RAM (.data + .bss) | 104,116 → 104,852 B: +736 B | `pio run` |
| PSRAM at run time | 51,200 B of draw buffers, plus LVGL's larger allocations | by construction |
| Internal RAM at run time | 8 KB task stack, plus LVGL's objects (allocations of 4 KB or less stay in internal RAM) | by construction; not measured |
| Frame rate | not measured | no board |
| CPU load | not measured; **estimated** 5-15% at 80 MHz | below |
| `lv_timer_handler` beside the loop | not measured | below |
| Colours against the calibration | the same tables, by construction; not checked on the panel | below |

The estimate: a sprite walking 40 px/s at up to 30 fps dirties about one
50x50 rectangle a frame (old and new position, merged). That is ~2,500 pixels:
7.5 KB at 18 bits, ~1.5 ms on the 40 MHz SPI, plus blending the background and
an RGB565A8 sprite into a PSRAM buffer and the per-pixel turn and lookup in
`pushRegion()`, at 80 MHz (`setup()` lowers the clock). The animimg frame
change redraws the same rectangle. Taken together: a few ms of work every
33 ms. A full-screen redraw (the first frame) is 76,800 pixels, ~46 ms of SPI
alone.

### Measured on the 1.54" board (2026-10-17)

The 2" board was not at hand; the same spike runs on the 1.54"
(`waveshare_esp32s3_lcd_154_lvgl`: 240x240, `spike/background_240.bin`, the
wander area from the screen size), bench board `02:00:00:00:00:08`, Wi-Fi
on, polling `api.wignet.app` every 10 s. Serial monitor, 5 s windows, about
2 minutes:

| What | Figure |
| --- | --- |
| Flash, spike build | 1,600,189 B (24.4%) |
| Static RAM | 103,492 B (31.6%) |
| LVGL at start | 2 x 19,200 B buffers in PSRAM; 5,988 B internal, 38,432 B PSRAM |
| Frames drawn | 16-25 a second (only when something moves: the walker pauses 0.2-1.5 s) |
| Dirty area | about 2,370 px a frame (one ~49x49 rectangle) |
| CPU in `lv_timer_handler` | 13-20% at 80 MHz, about half of it the flush |
| Longest handler call | 8-9 ms (107 ms once: the first full-screen draw) |
| Longest gap between calls | 26-35 ms, **33 ms in the window with a full PNG download and decode** (`/pic`, 3.7 s) |
| Free heap | ~204 KB internal, ~8.1 MB PSRAM; internal drifted down ~5 KB over 2 min (not yet called a leak) |
| LVGL task stack | 4,020 B of 8,192 never used |

So, at 80 MHz: animation does not stall while the loop downloads and decodes
a page, and there is CPU to spare. On the panel (the owner, by eye): colours
right, through the board's calibration, with clean sprite edges; BOOT makes
the walker happy (`[LVGL] BOOT pressed`, figures unchanged). Free internal
heap settled at ~203.9 KB after the first two minutes.
`Wire.cpp requestFrom ... Error -1` appears at each `/schedule`; the spike
uses no I2C, so it comes from the regular firmware's power or IMU read.

### Measuring on the bench

Flash `pio run -e waveshare_esp32s3_lcd_2_lvgl -t upload`, open the serial
monitor, and read:

- once: `[LVGL] Started: buffers 2 x 25600 B in PSRAM; LVGL took N B internal, M B PSRAM`;
- every 5 s: `[LVGL] fps, CPU % (flush %), px/frame, handler max ms, gap max
  ms, MHz, heap internal / PSRAM, stack free`. CPU is the share of wall time
  spent in `lv_timer_handler`, flush included (the flush is synchronous).
  `gap max` is the longest time between two handler calls: if the loop's
  HTTP or PNG decoding starves LVGL, it shows there.

Two switches, to compare: `-DWIGNET_LVGL_CPU_MHZ=240` (setup() runs at 80)
and `-DWIGNET_LVGL_BUF_INTERNAL` (draw buffers in internal RAM rather than
PSRAM).

### Beside the /schedule loop

The loop is not rewritten: LVGL has its own task on the same core, one
priority above, and sleeps until its next timer (at most 33 ms). The loop's
blocking work (HTTP, PNG decode into PSRAM) is pre-empted for a few ms each
frame, so the animation should not stutter while a page downloads; the loop
gets slower by LVGL's share. Wi-Fi runs on core 0. To be confirmed by
`gap max` during a fetch.

What the spike does not settle: today the loop's page and the scene are two
pictures fighting for one panel, so the spike simply hides the page. In a real
version the page would become the scene's background (one compositor, one
writer to the panel), and a page arriving would mean `lv_image_set_src` from
the LVGL task, never a `showFrame()`.

### Colours

LVGL's flush goes through the same `calibratedRed/Green/Blue` tables, the same
18-bit mode and the same INVON as every page, so a colour shows the same
whether it came as a PNG or as an LVGL image. Two caveats: the sprite's edges
are blended in RGB565 before calibration (as the server would composite before
calibrating, so consistent), and the calibration itself was measured on the
1.54" panel and is still to be checked on the 2" (`docs/lcd_2.md`).

## LVGL's XML loader (lv_xml)

**It is not in 9.6.0, nor 9.5.0.** The 9.5.0 changelog lists "XML Engine
Removed" under breaking changes ("feat(xml): remove the XML parser and loader",
lvgl/lvgl PR 9565): development continues outside the main repository, as part
of LVGL Pro (the editor, CLI and viewer, now documented at lvgl.io/docs/pro).
`docs/src/xml.mdx` in 9.6 is only a redirect page. I did not check the
licence of the Pro runtime loader.

The last open release that has it is **9.4.0** (`src/others/xml`, 2025-10).
Could it describe this scene? Partly:

- yes: a screen, an `lv_image` with a registered source (the pictures still
  have to be registered from C), a timeline moving the sprite between fixed
  points, and a `play_timeline` event on `pressed` or `key`;
- no: there is no `lv_animimg` parser, so no walk cycle; timeline animations
  take fixed start and end values with no repeat or random target, so no
  wander; no timed state that reverts after 3 s.

Maturity: it changed every release up to 9.4 (slots, selectors, image
buttons, colour animations were all added in 9.5's cycle) and was then taken
out. Building on it would pin the firmware to 9.4 or to LVGL Pro. A JSON
scene of our own, interpreted by a few hundred lines of C++ as here, fits the
need better: the server writes it, the vocabulary is ours (states, wander,
events), and it stays readable to an agent.

## Open questions

1. Run it, and read the figures above off the serial monitor.
2. The turn costs a per-pixel coordinate switch and lookup in `pushRegion()`.
   The ST7789 can turn by itself (MADCTL MV/MX/MY): with `MOUNT_TURN` done by
   the controller, the flush would be a straight copy through the tables.
3. The flush is synchronous on Arduino `SPI`; DMA (`esp_lcd` panel IO, or
   `SPI.writeBytes` from a second buffer) would let LVGL render the next band
   while one is sent. Only worth it if the CPU figure says so.
4. Tearing: partial updates are not synchronised with the panel's scan (the
   board does not wire TE as far as the pinout shows).
5. How scenes and sheets reach the board: a scene in `/schedule` with asset
   URLs, cached by ETag in PSRAM or LittleFS; the server converting to
   RGB565A8 (the format is 3 bytes a pixel, uncompressed; LVGL's RLE/LZ4
   options are off).
6. Battery: an animated screen cannot deep-sleep. On a battery board the scene
   would play only while awake (a button press, an event).
7. 9.6 is the last v9 release; v10 removes everything deprecated. With
   `ARGB8888` off, 9.6 fails to link unless `LV_DRAW_SW_SUPPORT_A8` is off too
   (`lv_draw_sw_blend_to_a8.c` needs `argb8888_image_blend`): see
   `esp32/spike/lv_conf.h`.
