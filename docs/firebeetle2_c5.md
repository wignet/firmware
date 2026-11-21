# FireBeetle 2 ESP32-C5 (DFR1222) + 7.5" V2: bring-up

Environment `firebeetle2_c5_75v2`, HAL `esp32/src/hal/firebeetle2_c5_75v2.cpp`.
Status: **in bring-up, not validated**. Firmware boots on the board; the
display has not yet been wired (2026-11-21).

## Not the FireBeetle we had

`firebeetle_75v2` is the DFR0478: a classic ESP32 (dual-core Xtensa) behind a
CH340, `ttyUSB*`. This one shares only the name. Measured on the bench board,
2026-11-20, with `esptool.py flash_id` (read only):

- ESP32-C5 revision v1.0, single-core RISC-V, Wi-Fi 6 2.4 + 5 GHz, 48 MHz crystal
- 4 MB flash, no PSRAM
- native USB Serial/JTAG: `/dev/ttyACM0`,
  `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00`

None of the classic FireBeetle wiring (`esp32/docs/TROUBLESHOOTING.md`, CS on
GPIO 9 in DIO mode) applies.

## Wiring (proposed, Waveshare e-Paper Driver HAT)

It follows the board's own GDI e-paper connector (Arduino-ESP32 variant
`dfrobot_firebeetle2_esp32c5`), plus a BUSY pin the GDI connector lacks.

| HAT  | Board label | GPIO | Note |
|------|-------------|------|------|
| VCC  | 3V3_C       | –    | switched rail, GPIO0 HIGH = on (off at reset) |
| GND  | GND         | –    | |
| DIN  | MOSI        | 24   | |
| CLK  | SCK         | 23   | |
| CS   | D6          | 27   | strapping (boot mode, ROM log); harmless while GPIO28 is high |
| DC   | D2          | 8    | |
| RST  | D3          | 26   | strapping (boot mode); same |
| BUSY | D12         | 6    | our choice |
| PWR  | 3V3_C       | –    | HAT Rev 2.3+ only: tie to VCC |

HAT Display Config switch on **B** for the 7.5-inch panel.

Avoid: GPIO2, 3, 7, 25 (strapping), 13/14 (USB), 16–22 (flash), 28 (BOOT
button), 1 (battery voltage), 15 (LED and GDI backlight).

Boot mode (ESP32-C5 datasheet v1.5, table 3-3): GPIO28 = 1 gives SPI boot
whatever GPIO26/27 read, and GPIO28 has an internal pull-up and the BOOT
button. So CS and RST can sit on 27 and 26.

Power: the firmware raises GPIO0 in `DisplayHAL::init()`. `hibernate()`, the
last display call before deep sleep, puts the panel to sleep, floats the
signal lines (so the unpowered HAT is not fed through them) and drops GPIO0.

## Build and flash

The C5 needs Arduino-ESP32 3.3 (pioarduino 55.03.313, core 3.3.13);
`espressif32@7.1.3` has no C5. Build it in its own PlatformIO home:

```sh
cd clients/esp32
PLATFORMIO_CORE_DIR=~/.platformio-pioarduino pio run -e firebeetle2_c5_75v2 \
  -t upload --upload-port /dev/ttyACM0
```

Why the separate home: both platforms install `framework-arduinoespressif32`
(2.0.17 and 3.3.13) into the same slot of `~/.platformio/packages`. Building
one target swapped the core under the others (seen 2026-11-21: the C5 build
lost its core after a `waveshare_driver_75v2` build, then reinstalling it
replaced the 2.0.17 core). Restore the shared home with
`pio pkg install -e waveshare_driver_75v2`.

Other differences from the shared `[env]`:
- `lib_ldf_mode = chain`: `deep+` misses Arduino 3.x's `Network` library.
- `board = esp32-c5-devkitc1-n4` with `board_build.variant =
  dfrobot_firebeetle2_esp32c5`: pioarduino 55.03.313 has no FireBeetle C5
  board file.
- `partitions/ota_4m.csv`, so OTA can be enabled later without a USB
  repartition. The OTA client is **not** enabled: untested on the C5. Image
  1.49 MB of a 1.75 MB slot (2026-11-21).
- `main.cpp` includes `soc/rtc_cntl_reg.h` only where it exists (not on C5).

## Log so far

- 2026-11-20: board identified, environment and HAL written, build passes.
- 2026-11-21: flashed over USB (hash verified). Serial: boots, GxEPD2 reports
  `Busy Timeout!` on power on / full update / power off (no HAT wired yet),
  then `*wm:Starting Web Portal`.

## Still to do

- Solder the HAT, check the first full refresh and the BUSY level.
- Measure deep-sleep current with 3V3_C cut.
- Battery voltage on GPIO1 (divider ratio not published: read the schematic,
  `DFR1222_firebeetle-2-esp32-c5_schematics_V1.0.pdf` on the DFRobot wiki).
- After a bench pass: `docs/hardware_catalogue.json`, `DEVICES.md`, power
  table in `HARDWARE.md`, then OTA.
