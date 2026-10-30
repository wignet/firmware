# Wignet TV — Raspberry Pi HDMI client

This is a small, full-screen RGB Wignet reader for a TV. The TV itself does not
run a browser or a local web application. If the Pi has no saved Wi-Fi
connection, it opens a temporary `Wignet-XXXX` access point and shows its name
and portal address on the TV. Connect a phone to that network; the same
bilingual captive portal used by the ESP32 clients opens to scan networks,
enter the password and connect. A USB keyboard is not needed. If a saved
network stops working, the same setup portal returns on the next boot.

NetworkManager stores the chosen connection on the Pi. The setup portal never
logs the password. After connecting, the Pi installs the TV runtime and the
Wignet display client takes over automatically.

The setup AP is open (no security setting). Setup failures are retried every
five seconds before the reader starts, and are retained in the bounded log
`/var/log/wignet-tv/setup.log`, including when the SD is inspected offline.
The setup AP itself never counts as a connection to the host Wi-Fi.

Once connected, it declares itself on `/schedule`
(`caps=v1:rgb,screen_size1,power_report1`, plus the sound tokens below, and
its screen size), fetches the
server-prepared PNG, retains the last page on a failure, and obtains each
next poll time from `/schedule`. The platform selects the active Welcome page,
event, push or slide when `/pic` is requested; the Pi neither interprets
events nor renders content. The server identifies an active planned display
event in response headers so the Pi can coordinate the TV's CEC wake.

After Wi-Fi onboarding, the graphical client shows a local Wignet splash
while DNS, TLS and the first download complete. It then sends HDMI-CEC
`Image View On` followed by `Active Source`, so a compatible television wakes
from standby and selects the Wignet input.  CEC is enabled by default and can
be disabled with `WIGNET_CEC=off` in `/etc/wignet-tv.conf`; it is always a
best-effort action, so a non-CEC monitor still displays normally.
For a planned display event, the Pi keeps the event image ready and sends the
CEC wake at the event's scheduled time, even when its display window opens
earlier for `lead_seconds`. If the Pi first polls after the scheduled time but
the display window is still open, it wakes immediately. Slide changes and
text/image pushes do not wake the TV; a sound does, planned or pushed (see
below). The Pi itself must remain powered and connected.

`tv_power.py` decides who owns the TV, so a bedroom television is neither woken
for nothing nor switched off under someone watching it:

- Before waking, the Pi reads the TV's power state over CEC
  (`cec.py status`). A TV already on is left alone, now and afterwards. A state
  it cannot read is woken as before but never put back to sleep.
- A TV the Pi woke goes to standby once the event's display window or the
  sound has ended, plus 90 s, and never sooner than 2 min after the wake. A
  second event or sound in the meantime extends the stay instead of cycling.
  If someone switched the TV off meanwhile, the Pi does not touch it.
- The same manager tracks the TV's 8-second startup delay for both image
  and sound events. A sound waits for that delay even when an image caused
  the wake; receiving both does not send a second wake or restart the delay.
- Loop guard: at most 4 wake-ups per hour. Beyond that the Pi stops waking
  the TV (it still displays and rings) and logs `TV wake refused`.
- Not detected: someone changing the TV's input while the Pi's wake is
  in force. The Pi would still put the TV to sleep at the end.

Each decision is logged, so support can tell what happened to the TV. On the
Buildroot image `/var/log/messages` lives in RAM and is lost at each reboot,
so the client also copies everything it logs, with a UTC timestamp, into
`support.log` in its state directory (`/data/wignet-tv/` on Buildroot), which
is kept across reboots. That file is bounded at 64 KB, plus one previous
`support.log.1`. It gets a few lines a day: the client logs sounds, CEC,
errors and a crash's traceback, never each sync. An event that woke the TV reads
`TV in standby; waking it for 930 s`, then `CEC Image View On and Active Source
sent`, then `sound: ringing …`. A TV left alone reads `TV already on; leaving it
alone`. Going back to sleep reads `CEC Standby sent`. These lines are new
since 2026-10-17: before that only failures were logged, so a successful wake
left no trace.

The splash follows the Wignet site's warm paper/ink/coral palette and shows an
indeterminate progress bar until the first server-rendered frame arrives; it
does not imply a percentage while network startup can take an unknown time.

The client can ask a compatible TV to enter HDMI-CEC standby without suspending
or powering off the Pi. For a manual check, run:

```sh
sudo /usr/local/lib/wignet-tv/cec.py standby
sudo /usr/local/lib/wignet-tv/cec.py wake
```

Wake also selects the Wignet HDMI input. Both commands are best-effort and
require CEC to be enabled on the TV.

## Sound through the TV

The TV's speakers ring the Wignet's sounds: alarms, one-off events and sounds
pushed by an agent (`push_event` with a `sound`), as the PhotoPainter does.
With `WIGNET_SOUND=on` (the default) the client adds
`wake_alarm,audio_out,sound_list1,tune_mml1` to `caps=`, so the platform sends
`/schedule`'s `sounds` list and its MML `tunes` (DEVICE_API.md), and offers
sound to the owner and agents for this Wignet. Nothing changes on the server.

- `sound_list.py` holds the list with the PhotoPainter's rules
  (`esp32/src/sound_list.cpp`): a sound rings within 120 s of its time or not
  at all, a pushed sound is kept until rung, and the 32 last rung occurrences
  are kept in `/var/lib/wignet-tv/rung.json`, written before playback, so a
  restart never rings one twice. The list itself is in `sounds.json`, so a
  sound still rings if the network is down at its time.
- `tune_render.py` synthesizes each tune with numpy, parsed by the platform's
  own `backoffice/mml.py` (copied next to it by the installers). Its samples
  are identical to the reference synthesizer's (`test_sound.py`), and each
  tune is rendered once, when the list arrives, into `/var/lib/wignet-tv/tunes/`.
- `sound_player.py` rings in a thread of its own, so the screen keeps updating:
  it wakes the TV over CEC 8 s before a sound (a pushed sound waits 8 s after
  the wake), then plays it through `aplay` at 48 kHz, looped for
  `fadeInSec + maxDurationSec`, rising from silence to `volume` over the
  fade-in like `audio_hal.cpp`. Without CEC it plays at once. A sound without a
  tune plays `/usr/local/lib/wignet-tv/sounds/<sound>.wav` (48 kHz mono 16-bit)
  if present, else the firmware's 880 Hz beep.

The Wignet's volume (`set_sound_volume`) scales the samples; the TV's own
volume applies on top. A sound ends at its duration: the remote cannot stop it
yet. `WIGNET_AUDIO_DEVICE` picks the ALSA output (`aplay -L` lists them) when
the default is not HDMI. HDMI audio needs `dtparam=audio=on` in
`/boot/firmware/config.txt` (Pi OS's default); on a monitor reported as DVI,
add `hdmi_drive=2`. Set `WIGNET_SOUND=off` for a screen without speakers.

```sh
journalctl -u wignet-tv -f | grep sound:   # renders and rings
```

It is intended for Raspberry Pi OS Lite (64-bit or 32-bit; use 32-bit for an
original Pi Zero). The Pi renders nothing: the server prepares each page at
the size the Pi declares, which is the HDMI mode's unless `/etc/wignet-tv.conf`
sets one. A size set by the fleet administrator, if any, wins.

## Install a fresh card

1. In Raspberry Pi Imager, choose **Raspberry Pi OS Lite (32-bit)** for a Pi
   Zero. Connect the Pi to the TV through HDMI and boot it. Preconfiguring
   Wi-Fi in Imager skips the setup portal; without it, use a phone to join the
   temporary `Wignet-XXXX` network shown on the TV. The portal should open by
   itself; if it does not, visit `http://192.168.4.1`.
2. Clone the Wignet repository to the Pi and run `sudo ./install.sh` from
   `raspberry-tv/`. The installer copies the shared ESP32 portal
   assets, and the setup assistant installs the TV runtime only after Wi-Fi is
   available.

If the card has just been flashed and SSH deliberately has no sudo password,
power the Pi off, put the card in a Linux reader and install the bootstrap
from a Wignet repository checkout without booting it:

```sh
sudo ./install-offline.sh /run/media/$USER/rootfs
```

On the next Pi boot, the phone-based portal configures Wi-Fi before the Pi
downloads the display runtime. The installer also configures NetworkManager's
shared-connection DNS to direct captive-portal requests to the local setup
page.
3. The first poll declares the TV; the server prepares the Welcome page in
   colour at its size, and the following poll shows it (the first may show
   it in black and white while that runs). Publish with the ordinary
   owner/MCP actions (`push_image`, `set_custom_image`, messages and slides).

The service uses HTTPS and the Pi's regular CA store. After Wi-Fi onboarding,
it starts the splash and CEC without waiting for time synchronisation;
HTTPS retries until the clock and API are available. To override the detected Wi-Fi/Ethernet MAC, or the
declared size, edit `/etc/wignet-tv.conf` and restart the service:

```sh
sudo systemctl restart wignet-tv
journalctl -u wignet-tv -f
```

## Boot: splash and start-up time

The TV shows no Linux text from power-on. `wignet-tv-boot apply` (run by
`install.sh` and at every boot by `wignet-tv-boot.service`, after
onboarding) keeps it that way:

- `cmdline.txt`: kernel console on `tty3` (never on screen), `quiet`, no
  Raspberry logo, no cursor; `getty@tty1` off, so no login prompt is drawn.
- `config.txt`: `disable_splash=1` (no rainbow square) and
  `auto_initramfs=0`: MMC and ext4 are built into the Pi kernel, and Pi OS's
  14 MB initramfs cost about 7 s.
- `wignet_tv.py` caches its splash as raw RGB565
  (`/var/lib/wignet-tv/splash-WxH-stride.rgb565`). udev runs
  `boot-splash.sh` the moment vc4 registers `fb0` and copies it there. The
  client later draws the same frame, so the handover is invisible.
  `vc4` and `i2c_bcm2835` (the HDMI DDC bus) are loaded by
  `/etc/modules-load.d/wignet-tv.conf` instead of waiting for udev.
- Off once cloud-init has finished the Imager's first boot: cloud-init
  (about 35 s at every boot), the rpi-swap file (`mkswap` at every boot,
  about 20 s; `/etc/rpi/swap.conf.d/90-wignet-tv.conf`), Bluetooth, udisks2,
  `NetworkManager-wait-online`.
- A netplan Wi-Fi profile (Imager) is moved to a NetworkManager keyfile, or
  dropped when Wignet saved the same SSID: NetworkManager rewrote it at every
  start, with a netplan generate and a systemd reload each time (3 × 13-18 s).
- `wifi_setup.py` checks for Pillow and numpy without importing them (4 s).

Measured on the Pi Zero W on 2026-10-13, Wi-Fi already configured:

| | before | after |
|---|---|---|
| kernel | 12.5 s | 4.1 s |
| HDMI framebuffer (`vc4drmfb`) | 64.5 s | 15.5 s |
| `wignet-tv.service` started | 2 min 28 s | 58 s |
| boot finished | 2 min 46 s | 1 min 9 s |

Still on the critical path: NetworkManager (about 21 s), which runs
`netplan generate` and a systemd reload at each start even without any
netplan profile; avoiding that means replacing a system package.

For diagnosis, put the Linux messages back on the TV (next boot):

```sh
sudo wignet-tv-boot log on     # off for the product configuration
```

The choice is the `wignet.bootlog=on` token in `cmdline.txt`, kept across
redeploys. `cmdline.txt.wignet-orig` and `config.txt.wignet-orig` keep
Pi OS's originals. If a card does not boot after these changes, put
`auto_initramfs=1` back in `config.txt` from a PC.

## Power and timed display

The original Pi Zero W stays powered while this client follows
`nextRefreshSec`; it has no onboard RTC that can restart it from a complete
shutdown at a chosen time. A planned display event can wake the TV over CEC,
but not a powered-off or sleeping Pi. Timed Pi wake needs an external RTC/power
board or a later Pi model with a wake-capable RTC.

`/schedule` is a real device contact and updates the Wignet's known presence.
`--once` is available for a local setup diagnostic; it still makes that
contact, so use the actual device identity only.

## Physical validation and product roadmap

On 2026-10-12, the owner confirmed the full onboarding cycle on the Pi Zero W
with the Wi-Fi bootstrap fix (`9fb0f81`): the AP configuration screen appeared,
the Wi-Fi password was submitted, and the server image reached the TV.
This validates onboarding and the first image, not the remaining checks below.

On 2026-10-13, the owner watched a reboot of the same Pi (`471c2b8`): a black
screen, then the Wignet splash ("Connexion à votre Wignet"), then the last
server image, with no Linux text at any point.

Product work to schedule after this working intermediate version:

- [x] Show a Wignet splash from early boot, covering Linux boot messages in
  the product configuration while retaining a development diagnostics mode
  (2026-10-13, see "Boot: splash and start-up time").
- [ ] Bring the HDMI Wi-Fi configuration screen into line with the site's
  visual style and guidebook.
- [ ] Keep a clean visual handover from password submission to the client
  splash and server image. Linux console output currently appears through
  the configuration image during this transition.

## Buildroot image (Pi Zero W, experimental)

The `buildroot/` external tree builds a standalone image, with a read-only
root and persistent Wi-Fi settings, logs and client state on `/data`.
Buildroot 2026.02.3 produced the first image on 2026-10-13. Its first physical
boot on 2026-10-15 reported `Invalid ELF header magic`: the kernel modules
were XZ-compressed, but BusyBox modprobe lacked XZ support. The requested
kmod tools had silently been disabled by their missing
`BR2_PACKAGE_BUSYBOX_SHOW_OTHERS` dependency. The configuration now enables
that dependency and XZ support for kmod. It also enables the BusyBox NTP
client needed by `S45ntpd` for HTTPS, and retains setup logs in
`/data/wignet-tv/setup.log` for offline diagnosis.

On 2026-10-15, the corrected image booted on the Pi Zero W and the owner
completed the AP portal and joined the home Wi-Fi. This exposed another
first-boot issue: NTP started before Wi-Fi/DNS and the clock stayed in 1970,
blocking HTTPS. `S45ntpd` now defers startup until a default route exists;
the runtime restarts it after onboarding. NTP errors are retained in
`/var/log/ntpd.log`, and stopping the service cannot create a bogus 1970
clock seed.

A subsequent software reboot with saved Wi-Fi and the clock seed removed
validated automatic reconnection, an NTP step from 1970 to the current date,
and a fresh server image without manual intervention. Measurements on the
same Pi (kernel timestamps and `/proc`, seconds after kernel start):

| Milestone | Pi OS, 2026-10-13 | Buildroot, 2026-10-15 |
|---|---|---|
| HDMI framebuffer ready | 15.5 s | 3.2 s |
| display client launched | 58 s | 13.6 s |
| first server image drawn | not measured | about 27 s |

A second observed reboot on the same day launched the client at 13.1 s and
drew the server image at about 25.7 s. At 368 s uptime, the client had logged
no errors and 342 MiB RAM remained available. The image timing is estimated
from the ETag file written after drawing, relative to uptime and the corrected
wall clock; allow about a second of precision. Across these two reboots the
client launched 77% earlier than Pi OS, and the HDMI framebuffer was ready
79% earlier. First-image time on Pi OS was not measured, so no percentage is
claimed for that milestone.

At 103 s uptime, the root remained read-only, one display client and one
NTP daemon were running, 345 MiB of 426 MiB RAM was available, and there
were no client errors in that boot's log. The display process used about
35 MiB RSS. Root ext4 allocated blocks (same calculation on the old SD
backup and the new root image) fell from 2,779,553,792 to 142,308,352 bytes,
about 95% less; this includes filesystem metadata, and excludes the separate
Buildroot data partition. These measurements validate this boot and first
image, not the CEC, sound, network-loss or long-running checks below. A power
cycle is also still required to validate a true cold boot.

Build with this checkout as the external tree:

```sh
make -C /path/to/buildroot O=/path/to/output \
  BR2_EXTERNAL="$PWD/raspberry-tv/buildroot" wignet_tv_pi0w_defconfig
make -C /path/to/buildroot O=/path/to/output
```

When reusing output from the first image, run `make -C /path/to/buildroot
O=/path/to/output busybox-dirclean kmod-dirclean` after loading the updated
defconfig and before the full build. Reconfiguring kmod alone can retain
objects compiled without XZ support.

The flashable image is `output/images/sdcard.img`. Preserve an image of a
working card before replacing it. Disable the host's automatic mounting
while flashing and verifying: mounting an ext4 partition changes its
superblock, so a subsequent byte comparison will fail even after a correct
write. Verify the written image and safely eject before booting the Pi.

## Updates over the air: A/B slots (fablamenas/wignet#322)

The card holds two root slots: `p1` boot (vfat) | `p2` root A | `p3` root B |
`p4` `/data`. The boot partition keeps the shared firmware (`bootcode.bin`,
`start.elf`, `fixup.dat`) at its top and each slot's kernel, device tree,
overlays and `cmdline.txt` in a directory named after it; `config.txt`
picks the running one with `os_prefix=p2/` or `p3/`. A card from before this
layout cannot be updated over the air: flash this image once.

`tv_ota.py` follows the ESP32 contract of `docs/ota.md` on the server (signed
`otaOffer` bound to the poll's `on=` nonce, `ota=`/`oc=` reports, `oh=` and a
signed `otaConfirm` while pending), with `hw` `raspberry_pi_hdmi`. The
artifact is the root filesystem image, served xz-compressed; `sha256` and
`size` are those of the uncompressed ext4, the bytes the slot holds. The
client writes the other slot, checks the hash, copies its `/boot` to `pN/`,
writes `tryboot.txt` and reboots once with the firmware's tryboot flag. A
signed confirmation turns `tryboot.txt` into `config.txt`; without one, the
client reboots at 600 s uptime (S00wignet's guard at 660 s, `panic=10`, a
power cut: any reboot loads `config.txt`, so the previous slot). The image
enables it with `WIGNET_TV_OTA=ab` in `/etc/wignet-tv.conf`; Pi OS installs
do not.

The root ext4 has no journal: the kernel rewrites a journal's superblock on
mount, even read-only, so the running slot would no longer hash to its image
and `oh=` would never match the offer.

Bench on the Pi Zero W, 2026-10-30, with a throwaway signing key and the
image served over an SSH tunnel: 320 MiB slot (46 MiB xz) downloaded and
written in about 110 s; tryboot into the new slot, confirmation, then a
normal reboot that stayed on it; an unconfirmed slot rebooted at the deadline
into the previous one and reported `rolledback` 20 times; the guard left a
confirmed boot alone past 660 s. The server supports this board since
fablamenas/wignet#327; a `v*` tag builds the release images
(`docs/flashing.md`). A run with the production key is still to come.

## Remaining hardware checks

Before considering a TV client released, check on the physical Pi: a first
RGB page, unchanged `304` without a redraw, a new publication, Wi-Fi loss
while the last page remains displayed, cold boot, and the server-selected
poll cadence. Also check the local splash, CEC wake from the TV's standby
state and input selection, with CEC enabled in the TV settings. For sound:
a pushed tune heard from a TV in standby (is 8 s of wake lead enough?), a
one-off event a few minutes ahead ringing on time, no replay after
`systemctl restart wignet-tv` just after a ring, and how long the Pi Zero
takes to render First Light (the log's `rendered in`; 0.19 s on a laptop).
These checks must use this device's actual MAC, not an invented one.
