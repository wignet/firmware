# Wignet ESP32/ESP8266 E-Paper Clients

This project supports multiple ESP32 and ESP8266 hardware configurations with
e-paper displays for the Wignet image polling system.

Before changing server URLs or preparing a firmware release, read the
[device API](../DEVICE_API.md). The device host is `api.wignet.app`.

Experimental application updates and rollback on the Waveshare ESP32 7.5 V2:
[OTA bench](../docs/ota-bench.md). This opt-in target needs one initial USB
installation of its larger partition layout; the ordinary target is unchanged.

## Prerequisites

### Software
- **PlatformIO** (VS Code extension or CLI)
- **Arduino framework** for ESP32/ESP8266 (installed automatically by PlatformIO)

For a fresh checkout, create the ignored local configuration header once:

```bash
cp src/config.h.example src/config.h
```

### Hardware
- ESP32 or ESP8266 development board (Waveshare Driver Board, FireBeetle, D1 mini, etc.)
- Waveshare e-paper display (7.5" V2 800x480)
- Connecting wires for e-paper

---

## Supported Hardware Targets

### Waveshare ESP32-S3 PhotoPainter Rev2.0 + 7.3" Spectra 6

**Environment:** `waveshare_photopainter_73e6`

This is the integrated PhotoPainter board using an ESP32-S3-WROOM-1-N16R8
(16 MB flash, 8 MB octal PSRAM) and the 800x480 `GDEP073E01` / `HT073A01`
six-color panel. The firmware is dual-format: it displays the historical 1-bit
BMP immediately, advertises WGN6 support, and switches automatically to black,
white, green, blue, red and yellow after the server marks the device as
`spectra6` and has a current color artifact. A rejected WGN6 frame triggers an
explicit BMP recovery request before the panel is touched.

The HAL handles the board-specific requirements:

- SPI: `DC=8`, `CS=9`, `SCK=10`, `MOSI=11`, `RST=12`, `BUSY=13`
- AXP2101 PMIC: I2C `SDA=47`, `SCL=48`, address `0x34`
- E-paper power: ALDO4 at 3.3 V while awake, disabled before deep sleep
- Audio codec (ES8311, I2C `0x18`) power: ALDO3, left on. Switching it off
  took the shared I2C bus down (RTC and PMIC included) until a power cycle
  (hardware, 2026-09-26)
- A 24 KB paged display buffer, a 48 KB 1-bit BMP buffer, and a 192 KB WGN6
  validation buffer in PSRAM
- Power (`power_hal.cpp`, since 2026-09-30): the AXP2101 tells mains from
  battery and gives the battery voltage and fuel gauge, sent on every
  `/schedule` (`pwr`, `bat`, `mv`, `chg`, with `power_report1`). On mains the
  frame never deep-sleeps and syncs every minute; plugging in or unplugging
  triggers a sync. The bench `power` command shows the registers. Details and
  what was validated on hardware: `../HARDWARE.md` "Power"

Build and upload over the ESP32-S3 native USB port:

```bash
cd esp32
pio run -e waveshare_photopainter_73e6
pio run -e waveshare_photopainter_73e6 --target upload \
  --upload-port /dev/cu.usbmodem1101
pio device monitor -p /dev/cu.usbmodem1101 -b 115200
```

Before the first upload, preserve the recoverable 16 MB factory image:

```bash
mkdir -p backups
esptool --chip esp32s3 --port /dev/cu.usbmodem1101 \
  read-flash 0 0x1000000 backups/photopainter_factory_<mac>.bin
shasum -a 256 backups/photopainter_factory_<mac>.bin
```

The USB port name can change after flashing or reconnecting. If automatic reset
fails, hold `BOOT`, tap `RESET`, then release `BOOT` when upload begins.
On a fresh install, join the open `Wignet-XXXX` access point and complete its
captive portal; this board's observed provisioning SSID is `Wignet-0006`.
Once Wi-Fi is connected, the PhotoPainter shows the same temporary MAC/IP
startup screen as other targets, then fetches its image from Wignet. A new
device is assigned the server-rendered `welcome` image containing the owner
URL and QR code. If that render is still pending, `/schedule` asks the frame
to retry in 60 seconds rather than sleeping through the welcome service's
normal 24-hour interval. QR generation and the owner URL remain server-owned.

#### Bench: play an MML tune over USB

With the frame connected and awake, its USB port carries a small bench
console (`src/usb_console.cpp`) that plays an alarm tune directly, with no
platform, Wi-Fi or alarm involved. From the repo root:

```bash
python3 esp32/play_mml.py tests/fixtures/mml/wake_jingle.mml --volume 40 --seconds 20
```

The script checks the tune with the platform's parser (`backoffice/mml.py`,
errors with line numbers), finds the `/dev/cu.usbmodem*` port, sends the
normalized tune and plays it through the alarm's audio path at full
`--volume` from the start (no fade-in). Ctrl+C or any frame button stops it.
It needs `pyserial`. `--check` only parses; `--port` picks the port.

In a serial monitor the console answers `help`, `mml` (then the tune's lines,
then `end`), `play [volume] [seconds]`, `stop`, `ringdraw [draws] [volume]`
and `i2c [reset]` (bus levels and scan; expect `0x18` codec, `0x34` PMIC,
`0x51` RTC). Replies start with `[bench] `.

`ringdraw` checks that a sound survives panel refreshes. It plays the last
tune (or the tone) in the audio task, redraws the panel `draws` times (2 by
default), and reports the worst gap between audio buffers and the dropouts;
see `HARDWARE.md`, "Audio runs in its own task".

#### Bench: test an alarm over USB

`esp32/test_alarm.py` sets a one-off test alarm through the same
console and follows it until it has rung, again with no platform involved:

```bash
python3 esp32/test_alarm.py --status                       # clock, RTC, alarms
python3 esp32/test_alarm.py wake.mml --in 60               # frame stays awake
python3 esp32/test_alarm.py wake.mml --in 90 --sleep       # deep sleep, alarm wakes it
python3 esp32/test_alarm.py wake.mml --in 90 --sleep-rtc   # only the RTC can wake it in time
```

The test alarm rings through the real alarm path (fade-in, volume,
duration, buttons) with the tune given, or the frame's sound without one.
It is kept apart from the platform's cached alarm, in RTC memory: a
`/schedule` poll does not touch it, and it survives deep sleep but not a
power cycle. `--sleep` forces deep sleep even though USB is connected;
`--sleep-rtc` also sets the timer backup two minutes late, so a wake on
time proves the RTC interrupt. USB drops while the frame sleeps; the script
waits for it and prints the frame's report (`alarm` in the console): how
late it rang and what woke it. A ringing alarm ignores serial input, like a
real one: stop it with a button, or keep `--duration` short (30 s minimum).

Console commands behind it: `alarm in <s> [volume] [fade-in] [duration]
[sound]`, `alarm`, `alarm clear`, `sleep [rtc]`.

#### Bench: show an image over USB

```bash
python3 esp32/show_image.py poster.png                    # as push_content, in the frame's colours
python3 esp32/show_image.py photo.jpg --fit cover
python3 esp32/show_image.py logo.png --profile mono1 --mode threshold
python3 esp32/show_image.py frame.wgn6                    # a device-ready file, as is
python3 esp32/testcard.py testcard.png                    # Spectra 6 harmonies card
python3 esp32/testcard.py --card mixing mixing.png        # every ink pair mixed
python3 esp32/testcard.py --card calibrate calibrate.png  # inks, text grounds, patterns
```

A picture is rendered by `imagePush.render`, the function behind
`push_content`, so the bench shows what a push would. `<name>.panel.png` is
the exact converter output; `<name>.real.png` paints each ink as it looks on
the panel (`backoffice/wgn6.py` `INK_APPEARANCE`). The file goes over USB in
acknowledged 4 KB chunks and is drawn by the decoder that draws downloads,
with all its checks: a corrupt file (bad CRC) leaves the panel alone. A shown
image pauses the platform's refresh for 30 minutes so it stays up; `resume`
in the console lifts that.

Measured 2026-09-26: a 192 KB WGN6 transfers in 2.2 s and the Spectra 6
panel then takes about 30 s; a 48 KB BMP transfers in 0.5 s, 36 s of panel.

The black-and-white ESP32 frames have the same `image` and `resume`, and no
tune, alarm or I2C commands. They talk through their USB-serial bridge
(on the Waveshare driver board a CH343, `/dev/cu.usbmodem*` on macOS) at
115200 baud. Their `help` starts `commands (images: bmp):`, so without
`--profile` the script sends them the 1-bit BMP; a WGN6 is refused and the
panel left alone. Measured 2026-09-28 on `waveshare_driver_75v2`: the 48 KB
BMP transfers in 4.2 s, 4.5 s of decode and panel. While the image is held the
frame does not fetch or deep sleep, so it keeps listening. A frame that is
already asleep answers only after its next wake or a `RESET` press.

The converter matches pixels against pure primaries (`RGB_PALETTE`), not
the inks' real look: a colour drawn as the panel shows it (green `#608050`)
is dithered from several inks, while `#00FF00` prints as solid green ink.
The test card (testcard.py) draws only exact ink values, with no
anti-aliasing, so the panel shows its patterns pixel for pixel: the six inks
solid, every pair mixed 50/50 and 75/25, and five-step tint ramps (oranges,
pinks, light blues, blue-yellow, greys).

The `calibrate` card checks the measured palette the contrast rules and the
photo converter rely on (`wgn6.PHOTO_INKS`, another panel's measurement) on
ours: the six inks as large solid patches (to measure or photograph under
the room's light), black and white text on every ink and every ink as text
on white at 40, 24 and 16 px, and six 50/50 mixes (orange, cream, pink,
light blue, teal, grey) laid out five ways. Checkerboard, rows and columns
hold the same pixels in other arrangements, which on a measured Spectra 6
can differ by about 18 ΔE00; diffused is what Floyd-Steinberg makes of a
flat colour an agent draws; photo is the mix's measured colour through
`quantize_photo`.

Things learned on the bench (2026-09-26):
- Leave DTR/RTS raised when opening the port. Lowering them (as a
  "don't reset" setting would) passes through RTS high / DTR low, which is
  esptool's reset: the frame rebooted on every open.
- The frame answers late while it redraws the e-paper (tens of seconds); the
  script waits and retries.
- The synth needs about 1.5–3 ms per 10 ms of audio at the firmware's 80 MHz.

---

### 1. Waveshare ePaper ESP32 Driver Board

**Environment:** `waveshare_driver_75v2`, `waveshare_driver_27`, or
`waveshare_driver_35`

**Board specification:**
- [Waveshare Universal e-Paper Driver Board](https://www.waveshare.com/wiki/E-Paper_ESP32_Driver_Board)
- Integrated ESP32-WROOM-32
- Dedicated e-paper connector

**Display Support:**
- 7.5" V2 (800x480)
- 2.7" (264x176)
- 3.52" black/white, UC8253c (240x360 native, 360x240 landscape)

**Pin mapping (fixed on-board):**

| E-Paper Pin | ESP32 GPIO |
|-------------|------------|
| BUSY        | GPIO 25    |
| RST         | GPIO 26    |
| DC          | GPIO 27    |
| CS          | GPIO 15    |
| CLK (SCLK)  | GPIO 13    |
| DIN (MOSI)  | GPIO 14    |
| VCC         | 3.3V       |
| GND         | GND        |

**Build & Upload:**
```bash
cd esp32
pio run -e waveshare_driver_75v2 -t upload
pio device monitor --baud 115200
```

**Test first-install WiFi provisioning:**
```bash
cd esp32
pio run -e waveshare_driver_75v2_no_default_wifi -t upload
pio device monitor --baud 115200
```

This variant clears saved WiFi credentials once for the flashed firmware build,
before starting WiFiManager. It should show the `Wignet-XXXX` access point and
the captive portal flow. After you enter WiFi credentials, the next boots keep
them. Rebuild and flash this variant again to test a fresh first-install flow,
or flash `waveshare_driver_75v2` for the normal production firmware.

**Bench notes, 2026-06-18:**
- Target for the Waveshare ESP32 Driver Board + 7.5" V2 black/white panel: `waveshare_driver_75v2`.
- Observed USB serial port during flash: `/dev/cu.usbserial-1410` (CP2102N bridge).
- Observed ESP32 MAC: `02:00:00:00:00:01`; first provisioning AP name: `Wignet-0001`.
- Expected boot log includes `Display: 800x480` and `E-Paper initialized via HAL`.

**Bench notes, 2026-06-21:**
- First-install test target: `waveshare_driver_75v2_no_default_wifi`.
- Validated upload port: `/dev/tty.wchusbserial5ABA0259681` (WCH USB serial bridge).
- Observed ESP32 MAC: `02:00:00:00:00:02`; provisioning AP name: `Wignet-0002`.
- Successful upload command:
  `pio run -e waveshare_driver_75v2_no_default_wifi --target upload --upload-port /dev/tty.wchusbserial5ABA0259681`

**Validated 3.52" setup, 2026-07-24:**
- Environment: `waveshare_driver_35`.
- Physical panel: Waveshare 3.52" black/white, 240x360 native.
- Wignet output: landscape rotation `1`, reported as `360x240`.
- Observed USB port: `/dev/cu.wchusbserial5ABA0253081`
  (WCH `1A86:55D3`; the port name is session-specific).
- Observed ESP32 MAC: `02:00:00:00:00:03`.
- The custom driver intentionally retains its software status check using
  command `0x71`. It may log `Busy Timeout!`, but the validated hardware still
  completes the refresh. Replacing it with GPIO BUSY polling caused the panel
  to remain black during this test and was rolled back.
- The server must associate the MAC with the intended service and generate a
  monochrome BMP matching the reported `360x240` dimensions.

```bash
cd esp32
pio run -e waveshare_driver_35
pio run -e waveshare_driver_35 --target upload \
  --upload-port /dev/cu.wchusbserial5ABA0253081
pio device monitor -p /dev/cu.wchusbserial5ABA0253081 -b 115200
```

---

### 2. DFRobot FireBeetle ESP32 V3 (DFR0478) + e-Paper Driver HAT

**Environment:** `firebeetle_75v2`

**Board specification:**
- [DFRobot FireBeetle ESP32 Wiki](https://wiki.dfrobot.com/FireBeetle_ESP32_IOT_Microcontroller(V3.0)__Supports_Wi-Fi___Bluetooth__SKU__DFR0478)
- Low-power consumption design
- On-board status LED: `LED_BUILTIN` / `D9`; firmware turns it on while awake
  and off before deep sleep.
- Uses default VSPI pins for SPI

**Wiring (FireBeetle → Waveshare e-Paper Driver HAT):**

| E-Paper HAT | FireBeetle Label | ESP32 GPIO | Note |
|-------------|------------------|------------|------|
| VCC         | 3V3              | 3.3V       | |
| GND         | GND              | GND        | |
| DIN         | MOSI             | GPIO 23    | |
| CLK         | SCK              | GPIO 18    | |
| CS          | D5               | GPIO 9     | **Requires DIO Flash Mode** |
| DC          | SCL              | GPIO 22    | |
| RST         | IO21             | GPIO 21    | (was MISO in V1 docs) |
| BUSY        | D4               | GPIO 27    | |

**Driver HAT switches:**
- Display Config: **B** (for 7.5" display)
- Interface Config: **0** (4-line SPI)

**Build & Upload:**
> **Note:** The `firebeetle_75v2` environment is pre-configured with `board_build.flash_mode = dio` to resolve the conflict between the CS pin (GPIO 9) and the internal SPI flash.

```bash
cd esp32
pio run -e firebeetle_75v2 -t upload
pio device monitor --baud 115200
```

**Development status, 2026-06-24:**
- Work is intentionally kept on branch `dev/firebeetle-low-power` until the
  FireBeetle + battery behavior is validated on hardware.
- Uploaded build target: `firebeetle_75v2`.
- Observed upload/log port for this session: `/dev/cu.wchusbserial1430`
  (WCH USB serial, session-specific and may change).
- Observed ESP32 MAC: `02:00:00:00:00:04`.
- Boot logs after reset showed:
  - `[LED] FireBeetle status LED on`
  - `Display: 800x480`
  - WiFi connected to saved AP `home-wifi`, IP `192.168.1.x`, RSSI `-50 dBm`
  - HTTP server listening on port 80
  - startup screen displayed
  - schedule response `nextRefreshSec: 21600` (6 hours)
  - `[LED] FireBeetle status LED off for deep sleep`
- The serial capture did not conclusively show the final
  `[DeepSleep] Entering deep sleep...` line after LED shutdown, so consider the
  full sleep/wake cycle unvalidated until checked on the device.

**Battery profile for FireBeetle DFR0478 + 2000 mAh Li-ion:**
- Firmware low-power path: CPU frequency is set to 80 MHz, WiFi is disconnected
  and disabled before sleep, the e-paper driver is put in `hibernate()`, and the
  FireBeetle LED is switched off before entering deep sleep.
- Vendor/reference figures: DFRobot lists `LED_BUILTIN` as `D9`, an average
  operating current of 80 mA, and 10 uA low-power support; Espressif lists ESP32
  deep sleep with RTC timer + RTC memory at 10 uA.
- With 2 wakeups/day and 30 s awake per wakeup: active use is
  `80 mA * 60 s / 3600 = 1.33 mAh/day`; deep sleep use is about
  `0.01 mA * 23.98 h = 0.24 mAh/day`; total is about `1.57 mAh/day`, or
  `2000 / 1.57 = 1270 days` before losses.
- Treat that as an optimistic upper bound. Real autonomy can be much lower if
  the e-paper HAT leaks current, the Li-ion protection/regulator quiescent
  current is higher than expected, WiFi retries extend wake time, or battery
  capacity/self-discharge dominate. Measure current at the battery input for the
  real number.
- Remaining firmware tradeoff: `enterDeepSleep()` still keeps a 10 s stability
  delay before sleeping. Once the display update path is validated on hardware,
  reducing that delay is the next simple battery win.

**Build & Upload (2.7"):**
```bash
cd esp32
pio run -e waveshare_driver_27 -t upload
pio device monitor --baud 115200
```

---

### 3. Wemos/Lolin D1 mini ESP8266 + e-Paper Driver HAT

**Environment:** `d1_mini_75v2`

**Board specification:**
- D1 mini / ESP8266
- Uses the Waveshare e-Paper Driver HAT over SPI
- USB serial bridge observed on the bench board: CH340/CH341
  (`/dev/cu.wchusbserial1440`, VID:PID `1A86:7523`)
- On macOS/MacBook, install the WCH CH340/CH341 serial driver if the board does
  not appear as `/dev/cu.wchusbserial*` after using a data-capable USB cable.

**Validated device, 2026-06-20:**
- Environment: `d1_mini_75v2`
- Observed ESP8266 MAC during flash: `02:00:00:00:00:05`
- Expected first provisioning AP name: `Wignet-0005`
- Successful upload command:
  `pio run -e d1_mini_75v2 -t upload --upload-port /dev/cu.wchusbserial1440`

**Wiring (D1 mini -> Waveshare e-Paper Driver HAT):**

| E-Paper HAT | D1 mini label | ESP8266 GPIO |
|-------------|---------------|--------------|
| VCC         | 3V3           | 3.3V         |
| GND         | G             | GND          |
| DIN         | D7            | GPIO 13      |
| CLK         | D5            | GPIO 14      |
| CS          | D8            | GPIO 15      |
| DC          | D3            | GPIO 0       |
| RST         | D4            | GPIO 2       |
| BUSY        | D2            | GPIO 4       |

**Driver HAT switches:**
- Display Config: **B** (for 7.5" display)
- Interface Config: **0** (4-line SPI)

**Build & Upload:**
```bash
cd esp32
pio run -e d1_mini_75v2
pio run -e d1_mini_75v2 -t upload --upload-port /dev/cu.wchusbserial1440
pio device monitor -p /dev/cu.wchusbserial1440 -b 115200
```

If upload does not auto-enter the ESP8266 bootloader, hold `FLASH`/`BOOT`,
press `RST`, then release `FLASH`/`BOOT` once PlatformIO starts connecting.
If the board has no `FLASH` button, temporarily connect `D3`/`GPIO0` to `GND`,
reset the board, flash it, then remove the jumper.

**Deep sleep note:**
For ESP8266 timed wakeup, connect **D0/GPIO16 to RST**. Without this jumper,
the device can enter deep sleep but will not wake itself automatically.

More details: [docs/D1_MINI_ESP8266_75V2.md](docs/D1_MINI_ESP8266_75V2.md)

---

## Remote Protocol (Multi-resolution)

The clients automatically inform the server of their screen dimensions during image polling:
- Added query parameters `w` (width) and `h` (height) to the Wignet URL.
- These dimensions are read from the `DisplayHAL` class.

**Example Request**:
`GET /pic/4displayB5.bmp?mac=XX:XX:XX&w=264&h=176`

The PhotoPainter target adds:

```http
Accept: application/vnd.wignet.spectra6, image/bmp;q=0.5
```

The URL stays `.bmp` for compatibility. The server returns WGN6 only when the
stored device profile is also `spectra6`; old servers and all legacy devices
continue to exchange BMP. The binary format and rollout invariants are frozen
in [`../../docs/wgn6.md`](../docs/wgn6.md).

### Firmware version

Every build is stamped with the git commit it comes from:
`firmware_version.py` runs before the build and writes
`src/firmware_version_gen.h` (gitignored). A build with uncommitted changes to
the firmware inputs reads `<commit>-dirty.<hash>`, the hash covering those
changes. The version is printed at boot (`[Boot] Firmware …`) and sent on
every `/schedule` poll as `&fw=`; the backoffice shows it in the device page
and in the owner `get_state` (`firmware.up_to_date`, `firmware.commits_behind`).
Build from a clean, pushed commit on `main` for a device that should report
`up_to_date: true`.

---

## Configuration

### WiFi Provisioning (Captive Portal)

**No hardcoded WiFi credentials needed!** The device uses WiFiManager for easy WiFi configuration.

**First boot (no WiFi configured):**
1. The device creates a WiFi access point named `Wignet-XXXX` (XXXX = last 4 chars of MAC address)
2. The e-paper displays: *"Connectez vous au wifi Wignet-XXXX pour configurer ce Wignet"*
3. Connect to this network from your phone or computer
4. A captive portal opens automatically at `192.168.4.1`
5. The portal opens on **Choose a network** (**Choisir un réseau** when the phone's language is French; the menu at the top right switches language).
6. After a short scan, nearby networks are listed with their signal and whether they are secured. **Refresh** scans again; **Another network** lets you type a name.
7. Select the WiFi network, enter its password, then tap **Save and connect** (**Enregistrer et connecter**).
8. The page shows **Settings saved** (**Configuration enregistrée**) above the network name, and says the Wignet is joining it.
9. The `Wignet-XXXX` network then disappears while the device switches networks.
10. The device saves the credentials, restarts, connects to the selected WiFi network, then fetches its first image from the Wignet server.

The same portal is started when saved credentials exist but that Wi-Fi cannot
be reached, such as after moving the Wignet. The portal remains available until
new working credentials are saved. On iPhone, joining `Wignet-XXXX` should open
the captive portal automatically; `http://192.168.4.1` remains the manual
fallback if iOS has cached an earlier connectivity result for that SSID.

**Captive portal pages** are served by `wifi_provisioning.cpp` from
`portal/` (embedded into `src/portal_assets.h` by `embed_portal.py` before each
build). WiFiManager still runs the access point, DNS and `POST /wifisave`
(fields `s` and `p`). Preview and edit them locally with
`portal-preview/server.py`; see `portal-preview/README.md`.

**Subsequent boots:**
- Device auto-connects to the saved WiFi network
- No manual configuration needed

**Reset WiFi credentials:**
- Access `http://<device-ip>/wifi/reset` in your browser
- Device will restart in AP mode for reconfiguration

### Server Settings

The device host is set in `src/config.cpp`:
```cpp
const char *WIGNET_HOST = "api.wignet.app";
const int WIGNET_PORT = 80;
```

Keep `/pic/4displayB5.bmp` and `/schedule` unchanged.
Both fetch functions currently build `http://` URLs with `WiFiClient`; do not
set port 443 without implementing and validating TLS for both requests.
See [DEVICE_API.md](../DEVICE_API.md) for ETag/304, wake scheduling, resolution
and release checks.

---

## Adding a New Hardware Target

See [../HARDWARE.md](../HARDWARE.md): what a target is, what the firmware
must do, board bring-up, USB bench testing, and what changes on the server
for a new resolution, screen type or capability.

---

## Troubleshooting

### Brownout detector triggered
Power supply issues during WiFi startup. Solutions:
- Use a quality USB cable
- Add a capacitor (10-100µF) between 3V3 and GND
- The firmware already uses reduced CPU (80MHz) and limits WiFi TX power after WiFi startup so the setting is actually applied.
- If brownouts continue during provisioning/AP startup, use a stronger USB power source or power the board separately; repeated brownouts can make the USB serial port disappear until the board is replugged.

### E-Paper not updating
- Check wiring connections
- Verify the display model matches your HAL configuration
- Check serial monitor for error messages

### Upload fails
- Make sure no serial monitor is running
- Try pressing the BOOT button during upload
- Verify the correct port is detected
- On D1 mini / ESP8266, e-paper wires on `D3`/`GPIO0`, `D4`/`GPIO2`, or
  `D8`/`GPIO15` can interfere with boot mode. If flashing fails, disconnect
  the display wires temporarily and force bootloader mode with `D3`/`GPIO0`
  held low during reset.

### D1 mini USB port not visible on macOS
- Use a data-capable USB cable. Charge-only cables power the board but do not
  create a serial port.
- A CH340/CH341 board should appear as `/dev/cu.wchusbserial*`.
- On MacBook/macOS, install the WCH CH340/CH341 driver if the USB device is
  detected but no serial port appears:
  https://www.wch-ic.com/downloads/CH341SER_MAC_ZIP.html
- Confirm detection with:
```bash
ls /dev/cu.*
system_profiler SPUSBDataType | egrep -i "wch|ch34|ch91|cp210|silicon|serial|uart|usb2"
```

---

## File Structure

```
esp32/
├── platformio.ini          # Multi-environment configuration
├── README.md               # This file
├── embed_portal.py         # Pre-build: embeds portal/ into src/portal_assets.h
├── portal/                 # Wi-Fi setup pages served by the device
├── portal-preview/         # Local server for those pages
├── src/
│   ├── main.cpp            # Hardware-agnostic Wignet logic
│   ├── config.h.example    # Copy to ignored local config.h before building
│   ├── config.cpp
│   ├── wifi_provisioning.h # WiFi captive portal
│   ├── wifi_provisioning.cpp
│   ├── portal_assets.h     # Generated from portal/ by embed_portal.py
│   └── hal/
│       ├── display_hal.h              # Abstract interface
│       ├── waveshare_photopainter_73e6.cpp # PhotoPainter/Spectra 6
│       ├── waveshare_driver_75v2.cpp  # Waveshare implementation
│       ├── d1_mini_75v2.cpp           # D1 mini / ESP8266 implementation
│       └── firebeetle_75v2.cpp        # FireBeetle implementation
```

---

## References

- [GxEPD2 Library](https://github.com/ZinggJM/GxEPD2) - E-paper driver library
- [WiFiManager Library](https://github.com/tzapu/WiFiManager) - WiFi configuration via captive portal
- [Waveshare E-Paper Wiki](https://www.waveshare.com/wiki/7.5inch_e-Paper_HAT)
- [DFRobot FireBeetle Wiki](https://wiki.dfrobot.com/FireBeetle_ESP32_IOT_Microcontroller(V3.0)__Supports_Wi-Fi___Bluetooth__SKU__DFR0478)
