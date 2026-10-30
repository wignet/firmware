# Wignet firmware: notes for coding agents

Read `HARDWARE.md` before adding or changing a hardware target (board, panel,
resolution, colour, capability): the firmware's required behaviour, USB
bench testing, and what the server needs to know about each kind of hardware.

Power (mains or battery, battery level) and `hw=`: `HARDWARE.md` "Power",
before touching wake scheduling or a board that can sense its power.

Read `DEVICE_API.md` before changing endpoint configuration, HTTP/TLS,
image fetching or wake scheduling. It is the client/server contract.

- Device host: `api.wignet.app`. `app.wignet.app` is for owners and AI
  agents, never for device polling.
- The ESP32/ESP8266 and Raspberry Pi clients build `http://` URLs. Port 443
  alone does not make them speak HTTPS; TLS needs its own implementation and
  a hardware test.
- Keep `/pic/4displayB5.bmp`, `/schedule`, the `mac` identity, `w`/`h`,
  ETag/304 handling and `nextRefreshSec` semantics. Do not depend on
  redirects.
- A live `/schedule?mac=...` call registers the device and changes its wake
  tracking. Test full cycles on a device of your own; never invent a MAC.
- Never commit `esp32/src/config.h`, a Wi-Fi name, a password or the
  identifier of a real device.

- Wi-Fi: every firmware remembers up to 5 networks and joins the strongest
  known one in range; the setup portal opens only when none is
  (`wifi_provisioning.cpp`, `docs/wifi.md`). It reports them as `wifi=` and
  forgets what `wifiForget` lists, never the network in use. Keep that path on
  a new board.
- Scenes (`scene1`, `scene_player.cpp`) only on a screen that redraws in
  under a second (LCD, OLED, HDMI), never on e-paper. Each scene feature has
  its own token, declared only when the board has the hardware
  (`scene_sound1`, `scene_tone1`, `motion1`, `pwr_short1`): `HARDWARE.md`
  "Firmware rules", `docs/scenes.md`.
- A board that never sleeps (an LCD) declares `stays_awake1` and reports its
  real `pwr=`: the server keeps its cadence (`HARDWARE.md` "Power").
- Wi-Fi setup portal: pages in `esp32/portal/`, served by
  `wifi_provisioning.cpp` and previewed with `esp32/portal-preview/server.py`
  (see its README). `esp32/src/portal_assets.h` is generated from them by
  `esp32/embed_portal.py` (run by every PlatformIO build; commit it with the
  change). `esp32/portal/wignet-portal.css` holds the Wignet design-system
  tokens and is generated outside this repository. Never edit either by hand.
- Alarm tunes: `raspberry-tv/mml.py` is a copy of the platform's MML parser;
  `esp32/src/mml_tune.cpp` walks the same grammar (`docs/mml.md`). A grammar
  change goes to the platform first.
- Names such as `backoffice/…` in comments point to the hosted platform,
  which is not in this repository.
- Screens a board draws itself (power-on, Wi-Fi setup, startup, Welcome) and
  the Wignet mark on them: `HARDWARE.md` "Start screens and the Wignet mark".
