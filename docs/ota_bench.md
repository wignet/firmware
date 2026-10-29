# The OTA bench

The firmware side of the OTA contract (wignet `docs/ota.md`) has a bench
gate: before `ota1` is declared in a production build, the PhotoPainter must
be seen refusing, reporting or rolling back in each case the contract lists.
The production server only sends valid offers, signed with the production
key, so the faulty ones come from a local server: `esp32/ota_bench_server.py`.
A bench build talks to it with a test key and its own CA.

## What is where

- `esp32/ota_bench_server.py`: `init` generates `esp32/bench/`, `serve`
  offers one image with one fault.
- `esp32/bench/` (git-ignored, never committed): the test signing key, the
  bench CA and the server's certificate, an untrusted CA for `bad_tls`,
  `ota_bench.h` for the firmware and `bench.json` (addresses, last counter).
- `[env:waveshare_photopainter_73e6_ota1_bench]` (`esp32/platformio.ini`): the
  ota1 build, plus `ota_bench_flags.py`, which reads `bench/`.

The bench build differs from the ota1 build in four ways, all from
`ota_bench.h` and `ota_bench_flags.py`:

- it trusts the bench's test key, not the production key;
- it trusts the bench CA, not ISRG Root X1, and accepts firmware URLs under
  `https://<bench ip>:<tls port>/firmware/`;
- it polls `/schedule` on the bench server (`WIGNET_BENCH_HOST`, as in
  `docs/scenes.md`);
- its attempt record lives in the NVS namespace `ota_bench`, not `ota`: bench
  counters never block the production server's offers afterwards.

It says so at boot: `[OTA] BENCH BUILD …; never outside the bench`. It cannot
install anything the production server offers (unknown key), and the bench
server cannot offer anything to a production build (unknown key, URL outside
`api.wignet.app`). After the bench, flash the ota1 build back over USB.

## Running it

Kouleur, or any PhotoPainter, on USB, on the same network as this computer:

```sh
cd esp32
python ota_bench_server.py init --ip 192.168.1.68      # this computer's LAN address
OTA_BENCH_TAG=a pio run -e waveshare_photopainter_73e6_ota1_bench -t upload
cp .pio/build/waveshare_photopainter_73e6_ota1_bench/firmware.bin /tmp/bench_a.bin
OTA_BENCH_TAG=b pio run -e waveshare_photopainter_73e6_ota1_bench
cp .pio/build/waveshare_photopainter_73e6_ota1_bench/firmware.bin /tmp/bench_b.bin
python ota_bench_server.py serve --image /tmp/bench_b.bin --fault expired
```

Keep a serial console open: the board writes why it refuses an offer
(`[OTA] Offer refused: …`), and the server writes each poll, each download
and the board's report (`RESULT counter N: …`). Stop the server with Ctrl-C
and start it again with another fault: each run uses a new counter (one above
the last in `bench.json`), except `wrong_counter`.

`OTA_BENCH_TAG` changes nothing but a string in the image: two tags of one
commit are two different images, which is also the "same commit, different
image" case. Offer an image the board is not running.

Allow the two ports (5050 and 5443 by default) through this computer's
firewall. The bench server answers `/pic/…` with 304, so the board keeps the
picture it shows, and its `/schedule` carries no sounds.

## The cases

One fault per run (`--fault`); what the board must do:

| Fault | Expected |
| --- | --- |
| `none` | installs, reboots pending, then `ota=confirmed:<id>` |
| `expired` | refused: `expired` |
| `wrong_hw` | refused: `hw is not this board` |
| `stale_nonce` | refused: `signature` (signed for the previous poll's nonce) |
| `bad_sig` | refused: `signature` (unknown key under the bench key's id) |
| `wrong_counter` | refused: `counter not above the last attempt` |
| `wrong_hash` | `ota=failed:<id>:hash` |
| `wrong_size` | `ota=failed:<id>:size` |
| `truncated` | `ota=failed:<id>:download` |
| `bad_tls` | `ota=failed:<id>:download` (certificate from an untrusted CA) |
| `no_confirm` | pending, then rollback at the 10-minute deadline: `ota=rolledback:<id>` |
| `forged_confirm` | confirmation refused, then rollback: `ota=rolledback:<id>` |
| `replayed_confirm` | confirmation refused, then rollback: `ota=rolledback:<id>` |

Faults built into an image instead (`OTA_BENCH_FAULT` when building it, then
`--fault none`). `crash_pending` goes in the image the server offers; the two
resets go in the image the board runs, since it is the running image that
installs:

| `OTA_BENCH_FAULT` | Built into | Expected |
| --- | --- | --- |
| `crash_pending` | the offered image | it aborts 20 s after boot; the bootloader returns to the previous one: `ota=rolledback:<id>` |
| `reset_before_select` | the running image | reset after the attempt record, before the slot is selected: the old image boots, the attempt counts as interrupted, nothing is reported |
| `reset_after_select` | the running image | reset after the slot is selected, before the record says so: the new image boots pending and is confirmed as usual |

The unsafe-power case runs against production: on battery, the board
reports `ota=failed:<id>:power` (passed on Kouleur, 2026-10-29).

Still missing from the firmware: a watchdog that recovers a pending image
whose startup hangs. Its case will join this list with it.
