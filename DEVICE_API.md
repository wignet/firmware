# Device API

> **Implementing this contract is free.** You may write a client for it, in any
> language and under any licence, open or proprietary. The GPL-3.0 of this
> repository covers its code and documents, not the protocol they describe.

A Wignet device makes two HTTP requests, then sleeps: it fetches its image,
then asks `/schedule` when to come back. Everything else (what is shown, when,
in which colours) is decided by the platform. No API key, login or
authentication header is involved.

## Addresses

| Purpose | Host / URL |
| --- | --- |
| Device image and schedule requests | `api.wignet.app` |
| Owner page, shown in the welcome QR code | `https://app.wignet.app/w/<id>` |
| Owner REST / OpenAPI / MCP, for AI agents | `https://app.wignet.app/api/owner/<id>/…` |

Use the ASCII hostname, without `www`. Devices only call `api.wignet.app`;
they never call the owner host. Both HTTP (80) and HTTPS (443) are served on
it, with no redirect between them.

## The poll

```text
GET http://api.wignet.app/pic/4displayB5.bmp?mac=<device-mac>&w=800&h=480
GET http://api.wignet.app/schedule?mac=<device-mac>&w=800&h=480
```

- `mac` is the device's hardware MAC, URL-encoded. It is the device's
  identity: keep it stable across firmware updates, or the device becomes a
  new one.
- `w` and `h` are the panel buffer's dimensions in its intended orientation.
  The response is a **1-bit BMP**; `.xbm` is also served. Request dimensions
  select and resize the returned image; they do not change the device's
  stored resolution.
- On image `200`, decode and display the image and keep its `ETag`. Send it
  back as `If-None-Match` while the cache is valid. A `304` has no body: keep
  the displayed image, skip the redraw, and still fetch the schedule.
- On a cold boot, do not trust uninitialised RTC or cache state.
- `/schedule` returns JSON with a positive `nextRefreshSec`, in **seconds**.
  Sleep for that long. On a network error or an invalid answer, fall back to a
  logged default interval; do not hard-code your own cadence otherwise.
- On any failure, keep the previous image. Never treat an HTML error page or a
  redirect as image bytes.

Publishing to a device does not wake it: what an agent publishes is shown at
the device's next poll. A device on external power polls every 10 s by
default (the owner's setting); a device on battery polls less often, and also
wakes for display events and alarms.

### Current clients and transport

`esp32/src/config.cpp`:

```cpp
const char *WIGNET_HOST = "api.wignet.app";
const int WIGNET_PORT = 80;
const char *WIGNET_IMAGE_PATH = "/pic/4displayB5.bmp";
const char *WIGNET_SCHEDULE_PATH = "/schedule";
```

`esp32/src/main.cpp` uses `WiFiClient` and builds `http://` URLs in both
`fetchWignetImage()` and `fetchSchedule()`; the Pi client
`raspberry/wignet_client.py` does the same. Setting port 443 alone does not
make them speak HTTPS.

An HTTPS client must use TLS for both requests, verify the server certificate
against a maintained CA bundle (do not pin the leaf certificate: it renews
automatically), and be checked on each board for clock setup, memory,
timeouts and sleep/wake.

An opt-in Waveshare 7.5 V2 OTA bench target is described in
[docs/ota-bench.md](docs/ota-bench.md). Normal polling remains unchanged;
only a physical USB command can authorize a firmware download, with its exact
size and SHA-256. That bench command may name a direct external HTTPS test
host with a public-CA certificate. It consumes no update offers from HTTP
schedule responses and declares no OTA capability yet.

## What a device declares on `/schedule`

These parameters go on **`/schedule` only**, never on the image request.
Unknown tokens are ignored; a malformed or newer declaration is logged and
ignored without making `/schedule` fail.

- **Capabilities, `caps=v1:<token>,<token>…`.** They say what the firmware
  can do; the server sends the matching fields. [HARDWARE.md](HARDWARE.md)
  lists every token and when to declare it.
- **Firmware version, `fw=<commit>`** (or `<commit>-dirty.<hash>` for a build
  with uncommitted changes), stamped by `esp32/firmware_version.py`. The Pi
  HDMI client sends the same value from a `VERSION` file written at install
  time by `raspberry-tv/tv_version.py`. The server shows the owner whether the
  firmware is up to date. An absent value clears the stored one.
- **Hardware, `hw=<build target>`**: the PlatformIO environment, e.g.
  `waveshare_photopainter_73e6`.
- **Power** (with `power_report1`): `pwr=ext` (external power) or `pwr=bat`,
  and when known `bat=<0-100>`, `mv=<battery millivolts>`, `chg=1|0`. A device
  on `ext` gets the owner's powered cadence. A device that never sleeps (the
  LCD boards) also declares `stays_awake1` and keeps the powered cadence on
  battery; its `pwr=` then only informs the owner. An absent `pwr` clears the
  previous report; an invalid value is ignored. See [docs/power.md](docs/power.md).
- **Wi-Fi, `wifi=<URL-encoded JSON>`**, sent by every build: the network in
  use, its signal and the known networks' names, never passwords
  (`{"cur", "rssi", "known"}`). The answer may carry `wifiForget` (`[names]`):
  forget those networks, never the one in use. See [docs/wifi.md](docs/wifi.md).

## What the server answers, by capability

- **Colour e-paper (`spectra6`).** The server renders colour for the device
  and serves WGN6 to an image request that accepts it
  ([docs/wgn6.md](docs/wgn6.md)). Dropping the token switches back to black
  and white.
- **Alarms and sounds (`wake_alarm`, `audio_out`, `sound_list1`,
  `tune_mml1`).** `/schedule` carries an `alarm` object and, with
  `sound_list1`, `soundsRevision` and `sounds`: the next sounds soonest first,
  at most 16 and at most 8 days ahead, each `{occurrenceId, eventId, epoch,
  pushed, sound, volume, fadeInSec, maxDurationSec[, tuneId]}`. A pushed sound
  comes first with `pushed: true` and `epoch` = `serverEpoch`. With
  `tune_mml1` it also carries `tunes`, `{tuneId: MML text}`, each tune sent
  once ([docs/mml.md](docs/mml.md)).
- **Welcome drawn by the device (`welcome_local1`, colour LCDs).** While the
  page is Welcome, `/schedule` carries `welcome`, `{"address", "qrSize",
  "qr"}` (else null), `qr` being the code's `qrSize`² modules as hex, row by
  row, most significant bit first, 1 dark. The device draws Welcome itself and
  does not fetch the image meanwhile ([docs/lcd_2.md](docs/lcd_2.md)).
- **Scenes (`scene1`, only on a screen that redraws in under a second).**
  The device adds its buttons as `&in=a,b` (never the power button). With a
  speaker that plays MML it adds `scene_sound1`, with an accelerometer
  `motion1`, with a speaker `scene_tone1` (a continuous tone), with a power
  button `pwr_short1` (a short press reaches the scene). `/schedule` then
  carries `scene` (`{id, url, etag, slide}` or null), `sceneEvents`
  (`[{seq, name}]`, acknowledged with `&sev=<last seq>`) and `utcOffsetSec`.
  The bundle is `GET /scene/<id>.json` and `GET /scene/<id>/<asset>.bin`
  (`&z=1` asks for a zlib stream); `report` values go to `POST /input`, all
  with `?mac=`. See [docs/scenes.md](docs/scenes.md).

## RGB / HDMI readers

A full-colour reader (an HDMI TV first) declares itself; nothing is set up on
the server:

```text
GET https://api.wignet.app/schedule?mac=<device-mac>&w=1920&h=1080&caps=v1:rgb,screen_size1,power_report1&pwr=ext&hw=raspberry_pi_hdmi
GET https://api.wignet.app/pic/4displayB5.png?mac=<device-mac>&w=1920&h=1080
```

- `rgb` gives the device the RGB profile; `screen_size1` makes this poll's
  `w`/`h` its buffer size.
- The PNG is an opaque full sRGB page. `ETag`/`304`, the failure rule and the
  cadence mean the same as for BMP.
- The PNG route answers `406` until the profile is `rgb`. The first page after
  a declaration may still be the previous rendering, scaled; the colour one
  follows at the next poll.
- A reader with speakers adds `wake_alarm,audio_out,sound_list1,tune_mml1`
  and rings the `sounds` list (`raspberry-tv/sound_player.py`).

During a planned display event, image responses (`200` and `304`) carry
`X-Wignet-Event-Occurrence` (an opaque per-occurrence token),
`X-Wignet-Event-Wake-After` (seconds until the event's time) and
`X-Wignet-Event-Window-Remaining` (seconds until the display window ends).
They are absent for slides, pushes and the Welcome page. The HDMI client uses
them to wake the TV over CEC at the event time with the image already
prepared, and to return it to standby 90 s after the window if it found it in
standby. An immediate publication (a push, a slide) is not a planned event and
does not wake a TV in standby. See [docs/hdmi_tv.md](docs/hdmi_tv.md).

## Testing a client

Use a device of your own. Before a release, check on the hardware:

1. One complete image download at the right resolution.
2. An unchanged image answered `304`: no redraw, but a schedule request.
3. A valid `nextRefreshSec`, a real sleep, a wake, and polling again.
4. Cold boot, and a failed request that keeps the previous image.
5. The same identity (and so the same owner page) before and after a reflash.

A route-only check needs no device (the expected answer is **400**,
`missing mac`):

```sh
curl -i http://api.wignet.app/pic/4displayB5.bmp
curl -i http://api.wignet.app/schedule
```

Do not call the API with a made-up MAC: the server registers unknown devices.
