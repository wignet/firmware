# Quick start: Waveshare ESP32 Driver Board + 7.5" e-paper

The default target, from an empty desk to a Wignet showing its welcome page.
For every other board, see [README.md](README.md).

## What you need

- Waveshare e-Paper ESP32 Driver Board
- Waveshare 7.5" V2 black and white e-paper display (800×480)
- A data-capable USB cable
- [PlatformIO](https://platformio.org) (VS Code extension or `pip install platformio`)

## 1. Connect the display

Insert the display's ribbon cable into the board's FPC connector and lock it.
**Mind the direction:** the ribbon's contacts face down, towards the board.

## 2. Set the switches

Set **Display Config A/B to B** for the 7.5" V2, and **Interface Config to 0**
(4-wire SPI).

## 3. Plug in the board

Connect the USB cable between your computer and the board.

## 4. Build and flash

```bash
cd esp32
cp src/config.h.example src/config.h
pio run -t upload
```

In VS Code: open the PlatformIO sidebar and click **Upload** under
"Project Tasks". The default environment is `waveshare_driver_75v2`.

## 5. Watch it start

```bash
pio device monitor
```

You should see:

```
=== Wignet ESP32 Client Starting ===
Display: 800x480
E-Paper initialized via HAL
```

On first boot the screen shows the Wi-Fi setup instructions. Join the
`Wignet-XXXX` network with your phone and choose your Wi-Fi. Once connected,
the Wignet downloads its 800×480 image and shows its welcome page: scan the
QR code to open your Wignet's page.

## Hardware configuration

`waveshare_driver_75v2` uses the `GxEPD2_750_T7` driver, 800×480, and the
board's built-in wiring: BUSY 25, RST 26, DC 27, CS 15, CLK 13, DIN 14.

## Common problems

**The background is grey instead of white.** Check that A/B is on **B** and
the interface on **0**.

**"Port not found".** Install the
[CP210x drivers](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers)
and check that the cable carries data, not only power.

**The screen stays blank.** Check the ribbon (inserted, right direction) and
the A/B switch, then press the board's **EN** (reset) button.

**"No module named platformio".** `pip install platformio`, or install the
PlatformIO extension in VS Code.
