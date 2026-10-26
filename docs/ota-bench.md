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

USB OTA acknowledgements are deliberately deferred until the writer requests
the next block, after flash erase/write. The first complete 4 KiB block is
received before erase begins: ESP32 flash operations can suspend UART
interrupts, so acknowledging early loses bytes even with a 4 KiB RX buffer.
The ordinary image-transfer protocol keeps its original acknowledgement timing.
The host retries its initial hold command because opening a UART bridge may
reset the board and discard a command sent during startup.

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

A pending trial prevents sleep but must not enable the bench hold that pauses
normal polling; otherwise it could never fetch the image needed to confirm.

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

Hardware session, 2026-10-26, final application commit `f0c02986eefc`:

| Measurement | Result |
| --- | --- |
| Board identified over USB | ESP32-D0WD-V3 revision 3.1, 4 MiB flash |
| Normal application-only binary | 1,251,248 bytes |
| Each application slot | 1,835,008 bytes (1.75 MiB) |
| Spare flash per normal application slot | 583,760 bytes (about 570 KiB) |
| Free heap before real HTTPS update | 183,148 bytes; largest block 110,580 bytes |
| Free heap after TLS setup | 121,880 bytes; largest block 73,716 bytes |

The TLS figures are from the working update connection before allocation of the
4 KiB streaming buffer. Both the normal and fault-injection applications were
built from the recorded source commit; full application and partition/link
checks passed. No private backup, credentials, device identifiers, signed URLs
or serial logs are stored in Git.

Passed on the connected board:

- Complete private 4 MiB recovery backup, with copied checksum verified;
  initial USB installation and the new partition layout recognized at boot.
- Same hardware MAC before and after installation. The initial installation
  opened the setup portal; after configuration, Wi-Fi survived subsequent
  resets and USB/OTA installs. Usability of the *original* saved networks was
  not established before reconfiguration.
- Normal 800x480 BMP processing, a cached `304` without redraw, schedule
  cadence and timed deep sleep/wake observed in the device log.
- Full USB application transfer, slot switch, fresh image processing and
  confirmation after image plus valid schedule.
- Full Wi-Fi update over verified HTTPS, SHA-256 validation, slot switch and
  confirmation after image plus valid schedule.
- Oversize request, wrong board descriptor, interrupted USB transfer and wrong
  SHA-256 rejected, each followed by a status check of the unchanged active
  slot. An interrupted transfer requires allowing the abandoned frame's
  ten-second drain timeout to finish before issuing a new serial command.
- Plain HTTP rejected. A verified HTTPS non-firmware file was rejected before
  flash writing. An expired-certificate endpoint failed certificate
  verification; the active slot remained unchanged.
- Fault-injection application downloaded over HTTPS into app0, booted with
  `trial=1`, then rejected itself on the 180-second deadline. The bootloader
  returned to the previous app1 with `trial=0`.
- A second HTTPS fault-image installation was reset through the USB bridge's
  hardware reset line immediately after `trial=1`, before confirmation. The
  bootloader returned to the previous app1 with `trial=0`, without waiting
  for the application's rejection timer.

The HTTPS test binaries were held in an **unpublished draft release**, not an
official release. A host resolved the temporary asset redirect first; the
ESP32 received only the final direct HTTPS URL, size and SHA-256 over USB,
without a GitHub account token or on-device redirect handling.

Host checks cover truncated/mismatched descriptors, wrong chip, malformed
hashes, manifest integrity, partition geometry, actual linked rollback/TLS
symbols and ACK timing using the real USB stream class with a controlled
sender. Both ordinary and experimental build jobs pass in CI.

Still to validate before a supported production release:

1. Owner/operator visual confirmation of the expected panel contents.
2. Deliberate failed image request preserving the previous panel content.
3. Dedicated HTTP redirect, missing/wrong Content-Length, chunked response
   and truncated HTTPS-body fixtures (the policy is implemented, but these
   individual network fixtures have not all been exercised on hardware).
4. Actual power removal during download and around activation. The timer
   rejection test does not substitute for cutting power.

Authenticated server offers, server/device status, official CI releases,
submission authorization, owner UI and MCP tools are later milestones. Do not
connect this writer to the existing plain-HTTP schedule without authenticating
the update command. No physical test result is implied by a successful build.
