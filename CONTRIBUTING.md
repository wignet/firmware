# Contributing

Issues and pull requests are welcome: a new board, a fix found on your
bench, a clearer sentence.

## Before you open a pull request

- A new board or screen: read [HARDWARE.md](HARDWARE.md) first. Say which
  hardware you tested on and paste the serial log of one full cycle (wake,
  image, schedule, sleep).
- A change to network code, wake timing or image handling: read
  [DEVICE_API.md](DEVICE_API.md).
- Never commit `esp32/src/config.h`, a Wi-Fi name, a password or the
  identifier of a real device. Use placeholders.
- Do not edit generated files by hand: `esp32/src/portal_assets.h` (rebuilt by
  every PlatformIO build) and `esp32/portal/wignet-portal.css`.

## Contributor licence agreement

The firmware is GPL-3.0-or-later. So that the project can keep the licence of
the whole code base coherent over time, every contribution comes with this
agreement. Opening a pull request means you accept it.

1. You wrote the contribution, or have the right to submit it, and it does not
   knowingly infringe anyone's rights.
2. You license it to the public under the GPL-3.0-or-later, like the rest of
   the repository.
3. You also grant the Wignet maintainers a perpetual, worldwide,
   non-exclusive, royalty-free, irrevocable licence to use, modify and
   distribute your contribution, and to relicense it under other terms,
   including in the hosted platform.
4. You keep the copyright of your contribution.

If you cannot accept point 3, say so in the pull request: we will discuss it
before merging.
