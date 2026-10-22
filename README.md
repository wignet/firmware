# Wignet firmware

Open firmware for [Wignet](https://wignet.app) screens: e-paper, colour
e-paper, small LCDs and HDMI TVs that show what your AI agent publishes.

The device has one job: ask `api.wignet.app` for its picture, show it, sleep,
and ask again. It says what it is (its size, its colours, its speaker) and the
platform renders for that. Everything that runs on the screen is here, under
the GPL-3.0.

## Get a working Wignet

**Waveshare ESP32 Driver Board + 7.5-inch V2:** use the
[ready-to-flash download](https://github.com/wignet/firmware/releases/latest)
and follow [the USB installation guide](docs/flashing.md). No PlatformIO or
compilation is needed. Downloads for other boards will follow hardware
validation.

To build from source:

1. Pick a supported board (below) and install [PlatformIO](https://platformio.org).
2. Flash it:

   ```bash
   cd esp32
   cp src/config.h.example src/config.h
   pio run -e <environment> -t upload
   ```

3. The screen opens a `Wignet-XXXX` Wi-Fi network. Join it with your phone,
   choose your Wi-Fi and enter its password.
4. The screen shows a QR code. Scan it: that page is your Wignet. Connect your
   AI agent from there (MCP or REST), or send it a photo.

For a TV, flash the Raspberry Pi image instead: [raspberry-tv/README.md](raspberry-tv/README.md).

## Supported hardware

| Board | Screen | Environment |
|---|---|---|
| Waveshare ESP32-S3 PhotoPainter | 7.3" Spectra 6 colour e-paper, 800×480 | `waveshare_photopainter_73e6` |
| Waveshare e-Paper ESP32 Driver Board | 7.5" V2 black and white, 800×480 | `waveshare_driver_75v2` |
| Waveshare e-Paper ESP32 Driver Board | 2.7" / 3.5" | `waveshare_driver_27`, `waveshare_driver_35` |
| DFRobot FireBeetle ESP32 (battery) | 7.5" V2 | `firebeetle_75v2` |
| Wemos/Lolin D1 mini ESP8266 | 7.5" V2 / 3.52" | `d1_mini_75v2`, `d1_mini_35` |
| Waveshare ESP32-S3-LCD-1.54 | 1.54" LCD, turns itself | `waveshare_esp32s3_lcd_154` |
| Waveshare ESP32-S3-LCD-2 | 2" LCD, scenes | `waveshare_esp32s3_lcd_2` |
| Raspberry Pi Zero W | any HDMI TV | [raspberry-tv/](raspberry-tv/) |

Wiring, switches and per-board notes: [esp32/README.md](esp32/README.md).
Bring your own board: [HARDWARE.md](HARDWARE.md) says what a firmware must do
and what the server needs to know about a new screen.

## Write your own client

[DEVICE_API.md](DEVICE_API.md) is the device ↔ server contract: two HTTP
requests, an image and a schedule. You may implement it in any language and
under any licence, open or proprietary.

## Repository map

- `esp32/`: PlatformIO firmware for every ESP32 and ESP8266 board, the Wi-Fi
  setup portal (`esp32/portal/`) and bench tools (`show_image.py`,
  `play_mml.py`, `testcard.py`).
- `raspberry-tv/`: the HDMI TV client and its Buildroot image.
- `raspberry/`: the Raspberry Pi e-paper client.
- `docs/`: screens and features the firmware implements (colour e-paper,
  LCDs, Wi-Fi, power, alarm tunes, scenes).

Some comments and documents name files of the hosted platform (`backoffice/…`,
server `docs/…`). The platform is not part of this repository; those names
say where the other half of a behaviour lives.

## Licence

[GPL-3.0-or-later](LICENSE). The e-paper driver library this firmware is built
on, [GxEPD2](https://github.com/ZinggJM/GxEPD2), is GPL-3.0 too.

“Wignet” and its logo are trademarks and are not covered by the licence: see
[TRADEMARKS.md](TRADEMARKS.md). Contributions are welcome under the terms in
[CONTRIBUTING.md](CONTRIBUTING.md).

Une Wignet en français : [wignet.app/fr/construire](https://wignet.app/fr/construire.html).
