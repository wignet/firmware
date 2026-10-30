# Install the ready-to-flash firmware

Download the image for your board (both screens are 800 × 480):

| Board and panel | File | esptool chip |
| --- | --- | --- |
| Waveshare ESP32 Driver Board + 7.5-inch V2 black-and-white | `wignet-waveshare_driver_75v2-factory.bin` | `esp32` |
| Waveshare ESP32-S3 PhotoPainter Rev2.0 + 7.3-inch Spectra 6 | `wignet-waveshare_photopainter_73e6-factory.bin` | `esp32s3` |

These images are not for a FireBeetle, an ESP8266 or an ESP32-S3 LCD.

Get the image and its matching `wignet-<environment>-SHA256SUMS` from the
[latest release](https://github.com/wignet/firmware/releases/latest).
Each image contains the bootloader, partition table and application. The
PhotoPainter image also initializes an empty LittleFS partition for sounds
and tunes; it includes no sounds or settings from a configured frame.
Flash the whole file at **0x0**. The application-only
`firmware.bin` from a PlatformIO build is not a first-install image.

## Flash over USB

Use a USB data cable. Install Python and esptool:

```sh
python -m pip install esptool==4.11.0
```

Check the downloaded file's checksum (`sha256sum -c wignet-<environment>-SHA256SUMS`
on Linux, `shasum -a 256 -c wignet-<environment>-SHA256SUMS` on macOS).
Replace `<environment>` with your board's target name, such as
`waveshare_photopainter_73e6`. On Windows, compare
`Get-FileHash .\wignet-waveshare_driver_75v2-factory.bin -Algorithm SHA256`
with the value in the matching checksum file (use the PhotoPainter filename
for that board). The release also includes `SHA256SUMS` covering every image.

`wignet-<environment>-app.bin` is the application alone: the Wignet server
installs it over the air on a frame that already runs this firmware (on the
7.5-inch, with the new partition table below). Never flash it at 0x0.

Replace `PORT` with the board's port, such as `/dev/ttyACM0`,
`/dev/cu.usbserial-…` or `COM3`:

```sh
python -m esptool --chip esp32 --port PORT --baud 115200 write_flash 0x0 wignet-waveshare_driver_75v2-factory.bin
```

For the **PhotoPainter Spectra 6**:

```sh
python -m esptool --chip esp32s3 --port PORT --baud 460800 write_flash 0x0 wignet-waveshare_photopainter_73e6-factory.bin
```

This **first-install image resets stored Wi-Fi and local settings**: the
merged file fills the space between the boot images with erased bytes. It
keeps the board's hardware identity and its Wignet owner page. On the
PhotoPainter, it also clears locally cached sounds and tunes. For an update
that preserves Wi-Fi, build from source and use PlatformIO upload instead.
The image is built from source, never from a backup of a configured device;
it contains no Wi-Fi credentials or device identifier.

If connecting fails, hold BOOT, tap RESET, release BOOT when flashing starts.

### From the old layout (7.5-inch)

Firmware from this version on gives the 7.5-inch board a new partition table:
two 1.75 MiB application slots, so the server can later update it over the
air (wignet/firmware#14). A partition table can't be changed over the air:
every 7.5-inch frame takes it over USB once, with either the factory image
above (which resets Wi-Fi) or, to keep Wi-Fi, PlatformIO from source:

```sh
cd esp32
pio run -e waveshare_driver_75v2 -t upload
```

`-t upload` writes the bootloader, partition table and application; the
Wi-Fi networks live in NVS, at the same place in both tables. Never flash only
the application at 0x10000 onto a frame that has not taken the new table: it
runs, but the frame stays without over-the-air updates.

## Connect the screen

1. After flashing, the screen opens an unprotected **Wignet-XXXX** Wi-Fi
   network. Join it on your phone. Keep the connection if the phone says it
   has no internet.
2. Open its configuration portal. If it does not open automatically, visit
   **http://192.168.4.1**. Select your 2.4 GHz Wi-Fi and enter its password.
3. The screen joins your network and downloads its content from Wignet.
   For a new device, scan the welcome QR code to open its owner page and add
   it to your account. A previously used device keeps its content and owner
   page: resetting Wi-Fi does not create a new Wignet.
4. Send a photo or a message from the owner page. It appears when the frame
   next syncs; e-paper refresh takes several seconds. Later, connect your AI
   agent from the owner's settings.

On the PhotoPainter, a colour refresh takes roughly 30 seconds. It reports
its Spectra 6 profile and power itself. On USB it stays awake at the cadence
chosen on the owner page; the battery cadence applies when unplugged.

## Build and publish downloads

On a clean checkout, copy `esp32/src/config.h.example` to `config.h`, then:

```sh
cd esp32
pio run -e waveshare_driver_75v2 -t release
pio run -e waveshare_photopainter_73e6 -t release
```

The image, checksum, source-commit record (`wignet-<environment>-BUILD.json`)
and installer manifest (`wignet-<environment>-manifest.json`) are generated
under `esp32/.pio/release/<environment>/`. The release target refuses a local
configuration or uncommitted firmware inputs. It uses PlatformIO's flash
offsets and esptool's `merge_bin`.

The GitHub workflow builds pull requests and `v*` tags. A tag creates a
**draft** release, whose notes are `docs/changelog/<tag>.md` plus a link to
this guide; a tag without that file fails. Write it for owners: the server
shows it on the frame's page when it offers the release, so a few lines on
what changes for them, no install steps. Before publishing the release, flash the exact attached image onto
the supported board and check the full portal → Wi-Fi → owner page → first
publication path, then a reboot that remembers Wi-Fi. Record what was
checked in the release notes. Downloadable binaries are GPL-3.0-or-later;
their exact source is available at the release tag.
