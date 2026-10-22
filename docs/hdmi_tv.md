# Wignet HDMI TV client

`rgb` is a hardware profile for a Wignet reader connected to an HDMI display.
It is intentionally separate from `spectra6`: Spectra 6 is six fixed eink
inks encoded as WGN6; RGB is an opaque full sRGB PNG. The platform prepares
the PNG before atomically switching the device's publication reference, so a
TV never fetches, scales or composes owner content itself.

## Contract

A client describes itself; the server knows display technologies (`mono1`,
`spectra6`, `rgb`), never client models. A new TV, tablet or page that shows
PNGs is a new client of `rgb` and needs nothing on the server.

- The reader declares itself on `/schedule`: `caps=v1:rgb,screen_size1` (plus
  `power_report1` with `pwr=ext`), and its real buffer size in `w`/`h`.
  `screen_size1` is what makes those `w`/`h` count: e-paper firmware sends
  them too, and for it the admin size, or 800×480, still renders.
- An admin-set `display_profile` is the fallback for firmware that cannot
  declare one; an admin-set `width`/`height` overrides a declared size.
- A change of declared profile or size re-renders the slides, push and events
  off the request; the next poll fetches them.
- The reader then calls `/pic/4displayB5.png` with its real MAC, retains its
  ETag and its last successfully displayed page, and respects
  `nextRefreshSec`. It may report `hw=raspberry_pi_hdmi`.
- PNG returns `406` until the device's profile is `rgb`, so the reader calls
  `/schedule` before its first image.
- Declarations on this MAC-only endpoint are observations, not attestation,
  as for `spectra6`: a false one only changes that device's own rendering.
- BMP/XBM/WGN6 remain byte-compatible routes for the installed e-paper fleet.
  Every publication still contains its mono BMP; RGB devices use the exact
  prepared `panel.png`.

## Raspberry Pi target

The maintained client is [raspberry-tv](../raspberry-tv/).
It uses Raspberry Pi OS Lite with pygame directly on HDMI — no Chromium,
desktop session or local HTTP service. Installation and physical release
checks are in its README. This target has not yet been validated on physical
hardware; do not count a server test or an image build as a TV check.
