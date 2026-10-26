# Waveshare ESP32 7.5 V2: experimental OTA bench

First firmware milestone for [platform issue #282](https://github.com/fablamenas/wignet/issues/282).
This is an opt-in bench target, not an official release or an automatic update
service. The existing `waveshare_driver_75v2` target is unchanged. No new OTA
capability is advertised and HTTP schedule responses cannot request updates.

## Memory and compatibility

`waveshare_driver_75v2_ota` uses `esp32/partitions/ota_4m.csv`:

| Region | Offset | Size |
| --- | --- | --- |
| NVS | 0x9000 | 20 KiB, original location |
| OTA metadata | 0xe000 | 8 KiB, original location |
| app0 | 0x10000 | 1.75 MiB |
| app1 | 0x1d0000 | 1.75 MiB |
| Filesystem | 0x390000 | 384 KiB |
| Core dump | 0x3f0000 | 64 KiB |

Changing this layout needs a USB installation. Never update the partition table
or use a merged factory image through the OTA application writer. The existing
filesystem region changes location; this board currently has no LittleFS usage.
Do not erase flash/NVS: Wi-Fi credentials and the hardware MAC must survive.

Each candidate must carry the `WIGNET_OTA_V1` descriptor for
`waveshare_driver_75v2` and layout `ota-4m-v1`. It occupies the first 80 bytes of
`.rodata_custom_desc`, after the standard ESP application descriptor. Both the
build packager and device verify its fixed position. A same-chip firmware for
another panel, an old firmware without this contract, or an incompatible layout
is rejected. This descriptor expresses compatibility, not a cryptographic
signature or a guarantee about arbitrary user-written code.

## Build and first installation

From `esp32/`:

```sh
cp src/config.h.example src/config.h
pio run -e waveshare_driver_75v2_ota
```

The build checks the descriptor and creates
`.pio/ota/waveshare_driver_75v2_ota/{firmware-ota.bin,ota.json}`. These are
application-only artifacts; `ota.json` records their exact length, SHA-256,
source version (including dirty hash), target, layout and experimental status.
No secrets or read-back device data belong in these artifacts.

Before the first hardware test, identify the board/flash, preserve a private
recovery backup outside Git, and ensure stable USB power. Then install the
normal experimental target by USB, with an explicit port:

```sh
pio run -e waveshare_driver_75v2_ota -t upload --upload-port /dev/ttyACM0
```

Do not use `erase` or flash the `_reject` target as the initial USB image.
The normal application must first complete an image fetch and schedule cycle.

## Bench console and application transfer

Only physical serial commands authorize installation. There is no unauthenticated
LAN upload endpoint. Keep USB power connected throughout testing: this board
cannot measure whether a battery is safe for an update.

- `ota status` / `ota hold`: report slot, trial state, layout, free heap and
  largest allocation; hold normal fetch/sleep for 30 minutes.
- `ota resume`: release that bench hold.
- `ota-image <bytes> <sha256>`: stream an application in acknowledged 4096-byte
  chunks, like the existing `image` console protocol.
- `ota <bytes> <sha256> <https-url>`: authorize the exact artifact over USB,
  then download it over Wi-Fi. This explicit bench command may name a direct
  external HTTPS test host; normal device traffic still uses api.wignet.app.
- `ota reject`: while a new application is pending, reject it and reboot.

`ota_bench.py` checks the adjacent application against the manifest before
opening the explicitly selected serial port. It has no backend imports:

```sh
python ota_bench.py .pio/ota/waveshare_driver_75v2_ota/ota.json --port /dev/ttyACM0 --check
python ota_bench.py .pio/ota/waveshare_driver_75v2_ota/ota.json --port /dev/ttyACM0
# Same exact artifact, hosted over HTTPS with a public-CA certificate:
python ota_bench.py .pio/ota/waveshare_driver_75v2_ota/ota.json --port /dev/ttyACM0 --url https://your-test-host.example/firmware-ota.bin
```

The USB transfer isolates flash/rollback tests from hosting/TLS tests. The Wi-Fi
path uses IDF's embedded CA bundle with hostname/certificate verification and
clock synchronization. It refuses HTTP, redirects, chunked responses and an
incorrect Content-Length. It logs free heap/largest allocation before and after
TLS so runtime memory can be measured. Never replace verification with
`setInsecure()`. Maintain the CA bundle through SDK/firmware updates.

The receiver checks the active partition layout and candidate prefix before
writing the inactive slot, displays an update/power notice, then streams with
a 4 KiB heap buffer and incremental SHA-256. Only after the expected hash and
ESP image validation succeed does it change the boot partition. A failed or
interrupted transfer must retain the original boot selection. The notice
invalidates the image cache so the next normal fetch restores the display.

## Trial boot and rollback

The SDK must enable `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`; the firmware
supplies a strong **C-linkage** `verifyRollbackLater()` override. A linked-symbol
check prevents silently using Arduino's weak auto-confirm implementation.

At the beginning of setup, a pending application arms an independent 180-second
ESP timer before display initialization and blocking Wi-Fi provisioning. It
cannot deep-sleep until confirmed. Confirmation requires both an image fetched
successfully (or a valid cached 304) and a parsed schedule with a positive integer
`nextRefreshSec`. The deadline rejects/reboots an unconfirmed application even
when the main loop is blocked. A crash/reset before confirmation also lets the
rollback-enabled bootloader choose the previous valid app.

Build `waveshare_driver_75v2_ota_reject` for fault injection. It has the same board
and layout contract but intentionally never confirms. Install it **through the
working experimental application's updater**, never as an initial USB image:

```sh
pio run -e waveshare_driver_75v2_ota_reject
python ota_bench.py .pio/ota/waveshare_driver_75v2_ota_reject/ota.json --port /dev/ttyACM0
```

After 180 seconds it must reboot into the previous working slot. This does not
restore NVS/filesystem data; future persistent format migrations must preserve
backward compatibility.

## Validation record and remaining work

Host checks cover truncated/mismatched descriptors, wrong chip, malformed hashes,
manifest/artifact integrity, partition boundaries and actual linked rollback/TLS
symbols. CI builds both experimental images independently of release packaging.

Hardware validation is still required before declaring this target supported:

1. Initial USB installation preserves Wi-Fi, MAC and owner identity.
2. Normal 800x480 1-bit BMP, 304 without redraw, valid cadence, sleep/wake and
   failed request preserving the previous image.
3. Application transfer switches slots and confirms after image + schedule.
4. Interrupted transfer, wrong SHA-256, wrong target/layout and oversize input
   keep the previous application bootable.
5. Fault-injection image and reset before confirmation return to the original
   slot, including recovery of ordinary polling and display.
6. HTTPS download succeeds with measured heap margin; invalid certificate,
   redirect, missing/wrong length and truncated body are rejected.
7. Power loss during download and around activation recovers correctly.

Authenticated server offers, server/device status, official CI releases,
submission authorization, owner UI and MCP tools are later milestones. Do not
connect this writer to the existing plain-HTTP schedule without authenticating
the update command. No physical test result is implied by a successful build.
