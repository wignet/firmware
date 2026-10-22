# Install the ready-to-flash firmware

The first downloadable image supports **Waveshare e-Paper ESP32 Driver Board
+ 7.5-inch V2 black-and-white panel (800 × 480)**. It is not for a FireBeetle,
an ESP8266, a PhotoPainter or an ESP32-S3 LCD.

Get the image and `SHA256SUMS` from the
[latest release](https://github.com/wignet/firmware/releases/latest).
`wignet-waveshare_driver_75v2-factory.bin` contains the bootloader, partition
table and application. Flash the whole file at **0x0**. The application-only
`firmware.bin` from a PlatformIO build is not a first-install image.

## Flash over USB

Use a USB data cable. Install Python and esptool:

```sh
python -m pip install esptool==4.11.0
```

Check the downloaded file's checksum (`sha256sum -c SHA256SUMS` on Linux,
`shasum -a 256 -c SHA256SUMS` on macOS). On Windows, compare
`Get-FileHash .\wignet-waveshare_driver_75v2-factory.bin -Algorithm SHA256`
with the value in `SHA256SUMS`.

Replace `PORT` with the board's port, such as `/dev/ttyACM0`,
`/dev/cu.usbserial-…` or `COM3`:

```sh
python -m esptool --chip esp32 --port PORT --baud 115200 write_flash 0x0 wignet-waveshare_driver_75v2-factory.bin
```

This **first-install image resets stored Wi-Fi and local settings**: the
merged file fills the space between the boot images with erased bytes. It
keeps the board's hardware identity and its Wignet owner page. For an update
that preserves Wi-Fi, build from source and use PlatformIO upload instead.
The image is built from source, never from a backup of a configured device;
it contains no Wi-Fi credentials or device identifier.

If connecting fails, hold BOOT, tap RESET, release BOOT when flashing starts.

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

## Build and publish downloads

On a clean checkout, copy `esp32/src/config.h.example` to `config.h`, then:

```sh
cd esp32
pio run -e waveshare_driver_75v2 -t release
```

The image, a checksum, a source-commit record (`BUILD.json`) and a manifest
are generated under `esp32/.pio/release/`. The release target refuses a local
configuration or uncommitted firmware inputs. It uses PlatformIO's flash
offsets and esptool's `merge_bin`.

The GitHub workflow builds pull requests and `v*` tags. A tag creates a
**draft** release. Before publishing it, flash the exact attached image onto
the supported board and check the full portal → Wi-Fi → owner page → first
publication path, then a reboot that remembers Wi-Fi. Record what was
checked in the release notes. Downloadable binaries are GPL-3.0-or-later;
their exact source is available at the release tag.
