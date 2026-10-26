# Adding or changing Wignet hardware

For work on a new board, panel, resolution, colour profile or capability. The
wire contract is [DEVICE_API.md](DEVICE_API.md). Files named `backoffice/…`
below belong to the hosted platform, not to this repository: a new kind of
hardware may need a change there too, so open an issue. Checked 2026-09-29; Wi-Fi, scenes,
power and audio rules brought up to date 2026-10-20.

## A firmware works when it has been seen working on hardware

The experimental Waveshare ESP32 7.5 V2 OTA target, its 4 MiB partition
layout, application compatibility descriptor, USB migration and rollback
bench procedure are documented in [docs/ota-bench.md](docs/ota-bench.md).
It remains opt-in until the physical checks there have passed; it is not
an automatic update service or a validated release.

A build that compiles proves nothing about a panel. Before calling a target
done:

1. Test it on the device over USB, with the bench console (below).
2. Run a full cycle on a dedicated test device: DEVICE_API.md's release
   checks (right-size image, `304` without redraw, real sleep and wake on
   `nextRefreshSec`, cold boot, failures, same MAC and owner link).

Never probe production with an invented MAC: `/schedule` creates or updates
a device. If a step could not be run, say so in the PR and list what remains.
Document the board like the existing ones (`esp32/README.md`, `esp32/docs/`),
including what was and was not validated on hardware.

## Firmware rules the code does not state

- **`w`/`h` is the landscape buffer the firmware draws into**, after the HAL's
  rotation. The 3.52" panel is 240x360 natively and reports 360x240. Server
  layouts assume landscape; owners get portrait through the orientation
  setting, not the panel size.
- **Target-specific code outside the HAL** goes only in the existing hooks in
  `esp32/src/main.cpp`: `getDefaultDisplayRotation()`, `StatusLED`,
  `preferColor` in `fetchWignetImage()`, `caps=` in `fetchSchedule()`, and
  the PhotoPainter blocks in `setup()`, `loop()`, `enterDeepSleep()`. When a
  second board shares a feature (alarm, USB-awake), replace the target check
  with a feature flag (e.g. `WIGNET_HAS_ALARM`) rather than listing targets.
  Optional hardware keeps no-op implementations for targets without it.
- **Colour:** declare `spectra6` in `caps=`; that is what switches the device
  to colour on the server. Validate the whole frame before touching the panel;
  if it is rejected, request again with `Accept: image/bmp` and no cached ETag
  (`docs/wgn6.md`).
- **Wi-Fi:** every firmware remembers up to 5 networks and joins the
  strongest known one in range, opening its setup portal only when none is
  (`wifi_provisioning.cpp`, `docs/wifi.md`). A new board keeps that path; it
  does not call WiFiManager's `autoConnect()` on its own. It reports the
  networks it knows on every `/schedule` (`wifi=`: the one in use, its RSSI,
  the known names, never a password) and forgets those `/schedule` lists in
  `wifiForget`, never the one in use (DEVICE_API.md).
- **Scenes (`scene2`) only on a screen that redraws in under a second:**
  colour LCDs, OLEDs, HDMI. A scene animates sprites and reacts to a press at
  once; e-paper (seconds to tens of seconds a refresh, ghosting) never
  declares a scene token, and keeps slides and events. The server also keeps
  scenes to full-colour (`rgb`) screens. A board that plays them uses
  `scene_player.cpp` (LVGL, `WIGNET_SCENES`) around `scene_runtime.cpp`,
  declares `scene2` (the scene runtime 2, `docs/scene_runtime.md`) and its
  buttons in `in=` (never the power button), and keeps the protected
  gestures: holding PWR (switching off), a ringing alarm or pushed sound, a
  5 s hold on any button (`docs/scenes.md` "Protected interactions").
  `scene2` stands for everything a scene player does: turning a scene
  without a download (`layouts`), sprites that avoid each other, and, with
  `audio_out`, `sound` actions and the tone. Besides it, a scene player
  declares hardware, each only when the board really has it:
  - `motion1` (an accelerometer): `tilt.x`/`tilt.y`/`tilt.z` values and the
    tilt, `shake`, `face_down` events (`OrientationHAL::gravity`); the
    screen's turn holds still while a scene follows the tilt;
  - `pwr_short1` (a power button whose hold the firmware handles): a short
    press reaches the scene as `pwr.short`, the hold stays the board's;
  - `touch1` (a touch screen): touches on objects.

  It also says what it can hold and draw: `scene_3mb` (a bundle of up to
  3 MB in PSRAM; without it the server sends at most 1.5 MB) and `scene_i4`
  (I4 pictures, `docs/scene_runtime.md` "Indexed pictures").

  **A new kind of scene is not a new token.** It is expressed with the
  runtime (variables, bindings, guards, `every:`, `when:`) or becomes a
  shorthand the server compiles down to it (`docs/scene_runtime.md`
  "Shorthands"). A token is for new hardware, or a runtime 3 if the core
  itself ever has to change. Runtime 1's feature tokens (`scene1`,
  `scene_sound1`, `scene_tone1`, `scene_turn1`, `scene_recolor1`,
  `scene_avoid1`) were retired on 2026-10-21: a board declaring only them
  gets no scenes until it runs a `scene2` firmware.

  The player asks for assets with `z=1` and inflates them with the ROM's
  `tinfl` (docs/scenes.md "Compressed assets"), and follows `scene.slide`
  from `/schedule`: in a slide `end` does nothing and a 5 s hold restarts
  the scene.
- **A colour LCD draws its own screens** (boot, Wi-Fi setup, Wi-Fi OK, and
  with `welcome_local1` the Welcome page) with its fonts, flush left, #F7F2E8
  on black; the server sends what a screen says, not a picture of it, where
  its own drawing would look worse at the panel's size.
- **Capabilities:** a new `caps=` token must be added to `REPORTED_TOKENS` in
  `backoffice/device_capabilities.py` before any firmware sends it; the
  server silently ignores unknown tokens. A value that is not yes/no (battery
  voltage) needs its own `/schedule` parameter, documented in DEVICE_API.md.

## Several sounds: read `sounds` when you can hold them

A board with alarm audio should declare `sound_list1` and keep `/schedule`'s
`sounds` list (DEVICE_API.md) rather than the single `alarm` object:

- **Store the whole list** (LittleFS, not NVS: 16 entries plus tunes of up to
  2 KB each do not fit NVS comfortably), and write it only when
  `soundsRevision` or the list changes, to spare the flash.
- **Arm the RTC and the timer for the earliest sound not yet rung**, and after
  each ring re-arm for the next one. Keep a small set of rung `occurrenceId`s
  (the same ids as `alarm`) so a reboot never replays one.
- **Ring within the grace window** as today; drop an entry found more than
  120 s late.

The PhotoPainter does this since 2026-10-01: `src/sound_list.{h,cpp}` holds
the rules (what is due, what comes next, how a new list replaces the held
one, the rung set) in plain C++, checked on the host by
`tests/test_sound_list_firmware.py`; `alarm_clock_hal.cpp` parses `sounds` and
`tunes` with ArduinoJson, stores them in `/sounds.json` on LittleFS (written
only when they change), keeps the 32 last rung occurrence ids in NVS, and
arms the RTC for the next sound. A pushed sound is handed over once, so one
not rung yet is kept when the next answer no longer lists it. LittleFS stays
mounted from boot (the audio task streams the WAV from it meanwhile). A
platform that sends no `sounds` falls back to the single `alarm`. `alarm` on
the bench console lists what the frame holds, and the last sound rung (kept
in RTC memory: after a deep-sleep wake it rings before USB is back).

Bench, 2026-10-01, on mains, against app.wignet.app: three one-off sounds
plus two weekly alarms held at once (5 sounds, 1 tune shared by both
alarms). The first two rang awake 0 s after their time, 0 dropouts, and
each poll after a ring held one sound fewer. A fourth, with the frame in
deep sleep and the timer backup two minutes late (`sleep rtc`), rang 0 s
after its time, woken by the RTC interrupt, before Wi-Fi: from the stored
list. Keep the sound due sooner than the sync interval for that test: on
mains the 60 s timer otherwise wakes the frame first.
The Raspberry Pi TV client does the same since 2026-10-12, in Python:
`raspberry-tv/sound_list.py` ports `sound_list.cpp`'s rules, keeps the list
and the rung ids in its state directory, and a thread wakes the TV over CEC,
then plays through HDMI with `aplay`. Its synthesizer (`tune_render.py`, numpy)
matches the reference sample for sample (`raspberry-tv/test_sound.py`).
- **Parse it with a real JSON parser.** The current firmware reads `/schedule`
  with `indexOf` and cuts the alarm object at the first `}`: fine for one flat
  object, not for a list.
- Keep reading `alarm` when `sounds` is absent (an older server).

## Power: report it if the board can sense it

A frame on mains syncs every `powered_sync_interval_seconds` (10 s unless
the owner chose up to 3600 s); on battery, every
`battery_sync_interval_seconds` the owner chose. The server learns which from
the firmware, or else from the owner. Contract: DEVICE_API.md; model:
`docs/power.md` (a full checkout). Server side since #83; the PhotoPainter
firmware sends it since 2026-09-30 (`esp32/src/power_hal.*`).

- **`hw=<PlatformIO environment>` on every `/schedule`, on every board**, e.g.
  `&hw=waveshare_photopainter_73e6`. It is how the platform tells boards
  apart. Lowercase letters, digits and `_`, at most 48 characters.
  `firmware_version.py` defines it for every build (`WIGNET_HW_TARGET`, from
  PlatformIO's environment name): a new target gets it with no code.
- **If the board can sense its power**, add to `/schedule`: `pwr=ext` (external
  power present) or `pwr=bat`, and when known `bat=<0-100>`, `mv=<battery
  millivolts>`, `chg=1|0`. Declare `power_report1` in `caps=`. Send `mv` even
  when you have a percentage: a fuel gauge is only trusted once calibrated.
  A board that can't sense it sends nothing; the owner states it on the page.
- **Sync when the power source changes:** plugging in and unplugging each
  trigger a sync that reports the new state. A charger counts as mains, not
  only a computer.
  - **Unplugging** happens while the frame is awake, since it never sleeps on
    mains: the main loop reads the power chip every 2 s
    (`POWER_CHECK_MS`) and makes the next poll due at once. No interrupt is
    needed for it.
  - **Plugging in** can happen during deep sleep: the AXP2101's VBUS insert
    interrupt wakes the PhotoPainter on GPIO21 (`usb_awake_hal.cpp`), and
    the normal wake cycle reports `pwr=ext`.
- **On mains, stay awake** and poll every `nextRefreshSec`, never faster than
  every 10 s: never deep-sleep while `pwr=ext`. The server answers a powered
  frame its powered cadence (10 s by default). Firmware before 2026-09-30
  polled every 60 s on mains whatever the answer: reflash a PhotoPainter for
  any other cadence.
- **On battery, deep-sleep for `nextRefreshSec`** as today, unless the
  board never sleeps (an LCD, `WIGNET_ALWAYS_POWERED`): it then declares
  `stays_awake1`, polls at the powered cadence whatever its power, and its
  `pwr=` is only shown to the owner.

**PhotoPainter implementation** (`esp32/src/power_hal.cpp`, AXP2101 at I2C
`0x34`). `PowerHAL::begin()` sets only the bits it needs, after the RTC has
started I2C: `0x30` bit 0 (battery voltage ADC) and `0x18` bit 3 (fuel
gauge). Each report reads:

| Register | Bits | Means |
| --- | --- | --- |
| `0x00` | bit 5 | VBUS good: `pwr=ext`, else `pwr=bat` |
| `0x00` | bit 3 | Battery present. Without it, `bat`/`mv`/`chg` are left out |
| `0x01` | bits 6:5 | Battery current: `01` charging (`chg=1`), else `chg=0` |
| `0x34`, `0x35` | 14 bits | Battery voltage, mV (`mv=`), sent only if 2000–5500 |
| `0xA4` | byte | Fuel gauge, 0–100 (`bat=`) |

Read on `02:00:00:00:00:06` on 2026-09-30 with the bench `power` command:

| Situation | Registers | Sent |
| --- | --- | --- |
| USB, no battery | `00=20 01=15 34/35=0x00B0` (176 mV on empty pins) | `pwr=ext` |
| USB, full battery | `00=28 01=14 34/35=0x1045 A4=64` | `pwr=ext&bat=100&mv=4165&chg=0` (`01` bits 2:0 = 4, charge done) |

**Validated on that frame (2026-09-30):**
- `pwr=ext` and the battery readings above;
- `api.wignet.app` recognised `power_report1` and answered 60 s;
- on mains it stayed awake;
- after unplugging it deep-slept: it had rebooted about 95 s after the replug, where a frame that never slept would have been up for hours;
- plugging in woke it, and it reported `pwr=ext` again.

Since 2026-10-02 its `device_power_log` also holds the `pwr=bat` reports,
at least one an hour, read with `get_power_history`.

**Battery drain, measured 2026-10-09** on that frame from its
`device_power_log`, times UTC. There was no meter: the gauge and the voltage
are all there is. The battery's capacity is not recorded; the last column
assumes 1500 mAh (Waveshare's optional battery), scale it for another.

| Stretch on battery | Sync | Drop | Lasts | Mean current at 1500 mAh |
| --- | --- | --- | --- | --- |
| 10-03 00:05 → 10-05 00:33 | about hourly | 94 → 89 %, 4078 → 4034 mV: ≈ 2.5 %/day | ≈ 40 days | ≈ 1.5 mA |
| 10-06 15:46 → 10-07 15:19 | 60 s | 98 → 76 %: ≈ 22 %/day | ≈ 4.5 days | ≈ 14 mA |
| 10-08 14:29 → 10-09 05:52 | 60 s | 28 → 16 %: ≈ 19 %/day | ≈ 5 days | ≈ 12 mA |
| 10-08 08:14 → 14:19 | 60 s, and something else | 58 → 29 % in 6 h: ≈ 115 %/day | < 1 day | ≈ 70 mA |

- **Sync rates are inferred.** The log keeps at most one unchanged row an
  hour. Up to 10-04 those rows came every 61.5 min like clockwork, which is one
  sync an hour plus the wake. From 10-05 they came between 60:00 and 61:20,
  which is one sync a minute. `battery_sync_interval_seconds` read 60 on 10-09.
- **The 10-08 spike** looks like a frame awake most of the time or redrawing
  often: tests, or failed fetches, since a failed `/pic` skips the sleep. The
  log cannot tell which.
- **The gauge is not a measure.** On 10-04 it held 90 % for a day. On 10-09
  it rose from 54 to 62 % after unplugging while the voltage fell
  3905 → 3816 mV, and `latest_battery_run.percent_per_day` read -48.2. Follow
  `mv` for trends.
- **The deep-sleep floor is unknown.** Hourly syncs bound everything at
  ≈ 1.5 mA, far above the ESP32-S3's ~10 µA, but this log cannot split sleep
  from wakes. Measuring it takes a meter between the battery and the board,
  since on USB the frame never sleeps.

Where a wake's time goes, from the firmware on 2026-10-09:

1. boot, then the WiFiManager connect;
2. NTP on every wake, up to 5 s (`AlarmClockHAL::syncFromNtp`);
3. `/pic`, plus about 30 s of Spectra refresh if the image changed, then `/schedule`;
4. 10 s in `enterDeepSleep()` with Wi-Fi still connected.

Cutting this down is issue #182.

**Not yet:**
- how the gauge percentage tracks the voltage over a full discharge;
- with a meter: the deep-sleep floor, the cost of one wake without a redraw,
  and the cost of one Spectra refresh (#182).

Which boards can sense power, checked in the firmware on 2026-09-29:

| Target | Mains? | Battery level? |
| --- | --- | --- |
| `waveshare_photopainter_73e6` | Yes: AXP2101 VBUS status | Yes: AXP2101 voltage and fuel gauge (uncalibrated) |
| `waveshare_esp32s3_lcd_154` | Partly, since 2026-10-19: charging (GPIO3 low) or a computer on its USB (`Serial.isPlugged()`); a wall charger with a full cell reads as battery. `stays_awake1` | Yes: voltage through a x3 divider on GPIO1, a percentage from a LiPo curve |
| `firebeetle_75v2` (DFR0478) | Probably not | Maybe: a divider to an analog pin; check the board before relying on it |
| `waveshare_driver_*`, `d1_mini_*` | No | No (a D1 mini could read A0 if the battery is wired to it) |

## Board bring-up traps

- **Back up the factory flash** before the first upload
  (`esptool read-flash 0 <flash size> backups/<board>_factory_<mac>.bin`).
  It cannot be recovered otherwise.
- **BUSY timeout:** Spectra 6 refreshes take ~30 s, beyond GxEPD2's 20 s
  default. A timeout that is too short lets sleep cut a refresh; the
  PhotoPainter HAL raises it to 60 s.
- **Mounting rotation** is set twice on a colour board:
  `getDefaultDisplayRotation()` for 1-bit drawing, `renderPackedColor()` in
  the HAL for WGN6.

## Audio runs in its own task

`AudioHAL::startAlarm` (PhotoPainter and 1.54" LCD, `esp32/src/audio_hal.cpp`)
plays in a FreeRTOS task on the app core, above the loop's priority, and
returns once the sound has started. A scene's cue (`startCue`, a tune once)
and tone (`startTone`, until stopped) run in the same task; buttons do not
stop them, as they belong to the scene. The loop meanwhile joins Wi-Fi, fetches and draws: an
event's sound and image arrive together. `playAlarm` and `playTest` are the
blocking forms, for the bench.

What a board with audio must keep:
- **One sound at a time, alarms first.** `ringIfDue()` does nothing while
  an alarm or pushed sound rings (`isRinging()`); a due alarm waits within its
  120 s grace. A scene's cue or tone gives way: a due alarm stops it, and the
  scene never starts one while something rings.
- **No sleep over a sound.** `enterDeepSleep()` waits for it to end
  (`waitForSound`), before and after the last `ringIfDue()`. The buttons still
  stop it.
- **One owner of the I2C bus.** The codec shares it with the PMIC and RTC:
  the loop skips its power check while a sound plays.
- **The DMA covers jitter, not a refresh:** 8 × 512 frames, about 85 ms at
  48 kHz. The task refills it; a panel refresh takes tens of seconds.

Every ring logs its buffer count, the worst gap between two I2S writes and
the dropouts (gaps longer than the DMA holds), then the task's spare stack.
`ringdraw` on the bench console measures a sound under panel refreshes.

## PWR pulses while the amplifier plays (PhotoPainter)

Measured 2026-10-10 on the bedroom Wignet with the bench's `buttons 600`
(below): in 8.5 minutes at rest, PWR (GPIO5) never moved; real presses held
it 685-945 ms (KEY 685 ms). From the first buffer of a ring, PWR pulsed high
for 5-10 ms every 80-300 ms with nobody touching it: 95 pulses in a 2-minute
ring. The old stop check (high, then high again 25 ms later) eventually
caught two pulses and ended the ring, 3 to 30 s in: an alarm with a fade-in
was stopped before it was audible, while short chimes, loud at once, were
heard. The firmware now counts PWR as a press only once held 300 ms
(`audio_hal.cpp` `PWR_HOLD_MS`) and logs the pulses it ignored in `Alarm
ended`. Before trusting a button for anything during playback, watch it with
`buttons` while a tune plays.

## USB bench console

`esp32/src/usb_console.*`, on every ESP32 target. It draws a file through the
same decoder as downloads, with no Wi-Fi or server involved. Scripts in
`esp32/`: `show_image.py` (any picture, `.bmp` or `.wgn6`), `testcard.py`
(colour test cards), `play_mml.py` and `test_alarm.py` (audio and alarm
boards). See `esp32/README.md` for usage.

`ringdraw [draws] [volume]`, on the PhotoPainter, plays the last tune (the
tone without one) in the audio task while the loop redraws the panel, then
answers
`[bench] done <ms>, <buffers> buffers, worst gap <us> us, <n> dropouts`.
Zero dropouts over two refreshes is the bar for showing an event's image
while its sound plays.

Measured on the PhotoPainter, 2026-09-30, at 80 MHz, firmware 97756d9:

| Test | Worst gap | Dropouts | Stack spare |
| --- | --- | --- | --- |
| `ringdraw 2 40`, 4-channel tune, two ~30 s refreshes | 10.7 ms | 0 | 4688 B |
| Test alarm, WAV from LittleFS, awake | 10.7 ms | 0 | 4256 B |
| Test alarm from deep sleep, tune | 10.7 ms | 0 | 4904 B |
| 90 s alarm across a fetch (304) and a `/schedule` | 10.8 ms | 0 | 4908 B |
| 150 s alarm across a published message: download, WGN6 decode and a full refresh | 15.2 ms | 0 | 4712 B |

Up to a download and a colour refresh, the worst gap is one 10 ms buffer.
With one, a synth buffer took 14.2 ms to render (of 10 ms real time, at
80 MHz): the DMA absorbed it, but that is where the margin goes first. A ring uses
about 8 KB of stack, so keep `AUDIO_STACK` at 12 KB.

`buttons [seconds]`, on the PhotoPainter, logs every change of BOOT, KEY
and PWR for that long (300 s by default), sampled every 5 ms in a background
task while the frame runs normally, with how long each level lasted:
`[bench] buttons: +18162 ms PWR PRESSED (was up for 18162 ms)`.

`cue [volume]`, on boards with audio, plays the last tune once through a
scene's path (`AudioHAL::startCue`, 10 s at most) and logs every BOOT or KEY
press while it plays, which must not stop it:
`[bench] cue ended after 2625 ms, 0 presses, 244 buffers, 0 dropouts`.

`power`, on every board, prints what `/schedule` would report and, on the
PhotoPainter, the raw AXP2101 registers behind it:
`[bench] axp2101 00=28 01=14 18=0A 30=03 34=10 35=45 A4=64`, then
`[bench] power valid=1 external=1 battery=100% 4165 mV charging=0 -> &pwr=ext&bat=100&mv=4165&chg=0`.
A board that can't sense it answers `[bench] power: this board can't sense it`.

If you extend it: replies start with `[bench] `, and the first line of `help`
names the formats the frame draws (`commands (images: bmp wgn6):`), which is
how `show_image.py` picks what to send.

Known gaps:
- `show_image.py` renders at a fixed 800x480. Another size needs the frame
  to report it (e.g. in the `help` line) and the script to read it.
- The scripts find `/dev/cu.usbmodem*` and `/dev/ttyACM*` only; CH340 boards
  need `--port`.
- ESP8266 targets have no bench console: test them through a full cycle.

## What changes on the server

The server learns the board only from `hw=`, once the firmware sends it
(`devices.reported_hw`, shown as `power.hardware`); nothing depends on it yet.
A device's hardware is what the superadmin sets in `devices.params` (`width`,
`height`, `display_profile`, `capabilities`), plus the `caps`, `fw` and power
fields the firmware reports. A declared `caps=` token wins over the stored
setting it stands for (`spectra6` or `rgb` over `display_profile`,
`audio_out` and `wake_alarm` over `capabilities`). With `screen_size1`, the
poll's `w`/`h` become the size used when the admin has set none. A new device
starts as mono1, 800x480, no capabilities, and counted as on battery.

- **New board, same panel type and size:** nothing, unless the firmware lives
  outside `esp32/`. Then add it to `FIRMWARE_PATHS` in
  `backoffice/firmware_version.py` and `BUILD_INPUTS` in
  `esp32/firmware_version.py`, or the device's version can't be counted.
- **New resolution:** serving already resizes to the requested `w`/`h`. But
  most renderers are designed for 800x480 (`BASE_W, BASE_H` or `W, H` in
  the root scripts; surf, windsurf and bus use a fixed grid) and only scale.
  Check each service at the new size. `grep -rn "800"` in `backoffice/` and
  `frontend/` finds the 800x480 defaults and preview ratios.
- **New screen type:** keep `docs/wgn6.md`'s compatibility rule: every
  renderer still writes the 1-bit BMP, and nothing changes for a device
  unless it declares the new profile in `caps=` (a token in
  `REPORTED_TOKENS`, read by `effective_display_profile()`) and its request
  explicitly accepts the new format. `grep -rn "spectra6\|SPECTRA6"` lists every place
  the existing profile touches (registry, validation, negotiation, producers,
  scheduler, admin UI, three copies of the palette, bench script, tests).
  That is work for a new display *technology* only. A new *client* of an
  existing profile needs nothing on the server: it declares the profile
  (`caps=v1:rgb`, `spectra6`) and, with `screen_size1`, its buffer size in
  `w`/`h`; the admin values are only a fallback and an override. The HDMI
  `rgb` client (docs/hdmi_tv.md) is the model: an immutable opaque sRGB PNG
  alongside the compatibility BMP, fetched at `/pic/4displayB5.png`.
- **Power sensing on a new board:** nothing on the server: it already reads
  `pwr`/`bat`/`mv`/`chg` (`backoffice/power.py`). Add the board to the table
  above, and to `docs/power.md`.
- **New capability:** start from `backoffice/device_capabilities.py`.
  `grep -rln "device_capabilities\|effective_capabilities\|supports_alarm"
  backoffice` finds the owner API and MCP gating, `grep -rln capabilities
  frontend/src` the admin UI, and `grep -rln wake_alarm tests` the tests.
- **Public hardware lists:** `grep -rn "800×480\|800x480\|264×176\|Spectra"
  website* shop agent_skill *.md`. After editing `agent_skill/`, regenerate
  `website*/skills/` (`agent_skill/README.md`).
