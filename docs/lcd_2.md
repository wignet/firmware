# Waveshare ESP32-S3-LCD-2: a 2" colour screen

A 240x320 colour LCD, the 1.54" board's bigger sibling (`docs/lcd_154.md`):
same chip, same ST7789 controller, same firmware code. It shows the
platform's full-colour PNGs (the `rgb` profile). Started 2026-10-16 on the
bench board `02:00:00:00:00:07` (access point `Wignet-0007`).

Env: `waveshare_esp32s3_lcd_2` (`esp32/platformio.ini`). Factory
flash backed up on 2026-10-16 to
`esp32/backups/lcd2_factory_020000000007.bin` (16 MB, SHA-256
`18522444…09dbb77`). Waveshare publishes no GitHub repository for it: the
pins come from `ESP32-S3-LCD-2-Demo.zip` (files.waveshare.com, ESP-IDF
`06_lvgl_example`).

## The board

| Part | Detail | Pins |
| --- | --- | --- |
| SoC | ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM, native USB | |
| Screen | 2" IPS 240x320 (portrait native), ST7789T3, colours inverted | MOSI 38, SCLK 39, CS 45, DC 42, no reset pin |
| Backlight | active high | 1 |
| I2C | QMI8658 accelerometer | SDA 48, SCL 47 |
| microSD | on the screen's SPI bus | MISO 40, CS 41 (held high) |
| Battery | connector, voltage on GPIO5; none fitted | |
| Button | BOOT | 0 |
| Camera | OV2640/OV5640 connector | not used |

No speaker, no microphone, no power-hold pin.

## What it declares

`/schedule?…&w=320&h=240&caps=v1:rgb,screen_size1,welcome_local1,orientation_report1,power_report1,stays_awake1,scene2,motion1&in=a&pwr=ext&ori=<turn>`

- `welcome_local1` (since 2026-10-20, both LCD boards): the Welcome page is
  drawn by the board itself, like its Wi-Fi screens, from `/schedule`
  `welcome` (the owner page's address and its QR modules,
  `backoffice/welcome_local.py`); `/pic` is not fetched meanwhile. The
  server-drawn page used hard-edged text for the e-paper inks, which looked
  pixelated at 2". The code is drawn in whole-pixel modules, as large as
  fits between the header and the two bottom lines, and redrawn on a turn.

- `rgb`, `screen_size1`: full-colour PNG at 320x240, the landscape buffer
  (`HARDWARE.md`), the page turned into it by the server.
- `orientation_report1` (`backoffice/device_capabilities.py`): the QMI8658
  says which edge is up, and `ori=` (the same clockwise turns as the
  platform's orientations) is the Wignet's orientation: the owner's is
  hidden and a changed one refused. Turning a 320x240 page to 240x320 means
  laying it out again, which only the server can do, so a new `ori=` renders
  everything again (`rerender_later`) and the answer asks the board back in
  3 s. `/pic` says which turn its page was laid out for (`X-Wignet-Turn`).
- `scene2`: plays scenes as the 1.54" does, with the scene runtime 2
  (`docs/scene_runtime.md`). SideWig2 still ran the runtime 1 firmware
  (`scene1`, 2026-10-20) when runtime 1 was retired on 2026-10-21: until it
  is flashed with `scene2`, it shows no scenes
  (`scene_player.cpp`, `docs/scenes.md`), with `in=a`: BOOT is its one
  button (no PLUS, no power-hold pin, so no `pwr_short1`). A scene is
  compiled for how the board is held, like any page: held portrait, the
  manifest is 240x320 with `turn` 90 or 270, and the player turns it into the
  landscape frame (`DisplayHAL::pushRegion`'s `pageTurn`).
- Turning (`scene_turn1` before `scene2`): the manifest carries both layouts (landscape 320x240,
  portrait 240x320) and the player moves a playing scene to the one held now
  when the board turns, at once, keeping its state and pending events: no
  new id, no download (`docs/scenes.md` "Turning the screen"): 3 ms a turn,
  measured 2026-10-20 with the Clawd scene. Before it, a turn compiled the
  scene again under a new id, from its first page.
- Colour variants: `scene_recolor1` (2026-10-21) made them on the board
  (the Clawd field's seven in 0.44 s, measured 2026-10-21); with `scene2`
  each arrives as a deflated file instead.
- Wandering sprites with `do.avoid` turn away from each other
  (`docs/scenes.md` "Sprites that avoid each other"; `scene_avoid1` before
  `scene2`).
- `motion1`: tilt values and tilt/shake/face_down events from the QMI8658
  (`OrientationHAL::gravity`), in the held page's frame.
- No sound: the board has no speaker.
- `pwr=ext`: always awake, at the powered cadence (`WIGNET_ALWAYS_POWERED`).

## Turning it

1. The orientation task (`orientation_hal.cpp`) finds a steady new turn.
2. The board's own screens (power-on, Wi-Fi setup, connected) are drawn
   again at once, upright in portrait or landscape, Wi-Fi portal included.
3. A platform page is shown shrunk to fit the new way up, with black bars,
   never sideways (`lcdShowInterim`), and the loop calls `/schedule` at once.
4. The board fetches every 2 s, without its cached ETag, and shows only a
   page whose `X-Wignet-Turn` matches how it is held; it gives up after 30 s.

## Firmware

The 1.54" board's driver, `src/hal/waveshare_esp32s3_lcd.cpp`, takes the
panel size, the frame size and a fixed mounting turn per board: the 2" frame
is 320x240, sent to the 240x320 panel turned 90 degrees (`MOUNT_TURN`). The
same power-on picture, setup screens, brand colours and colour calibration
as the 1.54" board; the calibration was measured on the 1.54" panel and is
still to be checked on this one with a test card.

## Still to do

- Check which way up landscape reads (`MOUNT_TURN` 90 or 270), the sensor's
  axes (`board.h`, a first guess copied from the 1.54") and the colour
  calibration, with test cards over the bench console.
- Battery, if one is fitted: GPIO5 voltage; there is no power-hold pin.
