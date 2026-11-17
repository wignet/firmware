# Build your own firmware and install it over the air

You can change this firmware, build it, and install your build on your own
Wignets over the air, the way an official release installs. Three steps:
build, upload, install. The server's side of it is `docs/ota.md`, "Who
certifies, who installs", in the wignet repository.

## Before you start

- **One USB flash first.** The board must already run an official image that
  updates over the air (it declares `ota1`): flash it once over USB, as
  [flashing.md](flashing.md) says. That flash installs the bootloader, the
  two app slots and the OTA client that will take your build. A board
  running something else takes nothing over the air.
- **A Wignet account.** You sign in with the same account as on
  app.wignet.app, and you must administer the Wignet (hold its admin link
  or be its admin through your account). Someone a Wignet was shared with
  cannot install firmware on it.

## What to keep as it is

Your build is installed by the OTA client it replaces, and must keep being
able to take the next one. Leave these untouched:

- the OTA client (`src/ota_client.*`, `WIGNET_OTA_CLIENT` and
  `WIGNET_OTA_CAPABLE` in your board's environment), which declares `ota1`;
- the `ota1` token the firmware sends in `caps=` (`src/main.cpp`): the server
  looks for that literal in the image;
- `src/ota_keys.h`, the server's public key. With another key the board
  refuses every offer, the next one included;
- the `&fw=` version stamp (`firmware_version.py`).

Build your board's own environment (`waveshare_driver_75v2`,
`waveshare_photopainter_73e6`, …), never an `_ota1_bench` one: those trust
the bench's test key. Keep `api.wignet.app` as the host in `src/config.cpp`:
a board that talks to another server never confirms the update.

## 1. Commit, then build

```bash
cd esp32
git commit -am "What I changed"
pio run -e <environment>
```

Commit **before** you build. The build stamps the commit into the image
(`Firmware version:` in the build log, `src/firmware_version_gen.h`), and the
board reports it on every sync. The server confirms a new firmware only when
the board reports the commit it was stored with. A build from uncommitted
changes reports `<commit>-dirty.<hash>` and would never be confirmed: the
board would go back to its previous firmware by itself after ten minutes. You
do not need to push the commit.

## 2. Upload

```bash
python3 wignet_push.py -e <environment> --changelog "What changed"
```

`wignet_push.py` uploads `.pio/build/<environment>/firmware.bin` to your
account (`POST https://app.wignet.app/api/account/firmware`) and prints the
build's id. Before uploading it checks what the server checks, so you learn
of a refusal at once:

- the image reports a clean commit: not `-dirty`, not `unknown`;
- the image declares `ota1`;
- the environment is not a bench one.

The first run opens your browser to sign in to Wignet, as the account
connector does; the sign-in is kept in `~/.config/wignet/push.json`, readable
by you only. `python3 wignet_push.py --logout` forgets it. `--check` runs the
checks and uploads nothing.

No Python, or prefer the page? Open one of your Wignets on app.wignet.app,
Settings › Firmware › Your firmware, and upload `firmware.bin` there. An
agent with the account connector can also submit an image with
`submit_firmware`.

The server refuses a build without `ota1` (installed, it could never update
over the air again, only over USB) and a build from uncommitted changes (it
would roll back by itself), whichever way you upload it. Uploading the same
bytes twice gives the same build.

## 3. Install

From your Wignet's page: Settings › Firmware › Your firmware, then Install.
Or ask your agent, on the account connector: `update_firmware` with the id
the upload printed. No other approval follows: your request is the decision.

The board takes the update at its next sync, restarts on it, and must
confirm within ten minutes. If it does not (the build does not start, cannot
reach the server, or reports another commit), the board goes back to the
firmware it had; the page says so. An image that rolled back twice on a
Wignet is not offered to it again.

## Who can install it

Only you. Your builds are listed for you alone (`yours` in `get_firmware`),
and only on Wignets you administer. If someone uploaded the very same bytes
first, the build is theirs, and only they can install it.

To go back to an official release, pick it in Settings › Firmware: any
official release for your board can be installed, an older one included.
