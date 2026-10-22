# FireBeetle ESP32 e-Paper Fix Walkthrough

This document summarizes the debugging process and solution for connecting a Waveshare e-Paper Driver HAT to a DFRobot FireBeetle ESP32.

## The Problem
The e-paper display failed to refresh, reporting continuous "Busy Timeout!" errors. This indicates the ESP32 was unable to communicate with the display controller.

### Root Cause
1.  **Pin Mapping Conflict**: The initial wiring used pin **D5** for the Chip Select (CS) line.
2.  **Hardware Limitation**: On the DFRobot FireBeetle ESP32, **D5 corresponds to GPIO 9**.
3.  **Conflict**: GPIO 9 is connected internally to the ESP32's SPI Flash memory. Using it as a Chip Select line caused a conflict with the flash controller, preventing successful communication with the display (and normally causes boot loops).

## The Solution
We resolved the issue using a software-only workaround, avoiding the need for resoldering.

### 1. Enable DIO Flash Mode
By switching the ESP32's flash mode from `qio` (Quad I/O) to `dio` (Dual I/O), we freed up GPIO 9 and 10 from flash operations.

In `platformio.ini`:
```ini
[env:firebeetle_75v2]
board = firebeetle32
board_build.flash_mode = dio  ; <--- Key fix
```

### 2. Validated Configuration
With `flash_mode = dio`, the existing wiring functions correctly without interference.

**Verified Wiring:**
| Function | e-Paper HAT | FireBeetle Pin | ESP32 GPIO | Note |
|----------|-------------|----------------|------------|------|
| **BUSY** | BUSY        | **D4**         | **27**     | Input |
| **RST**  | RST         | **IO21**       | **21**     | |
| **DC**   | DC          | **IO22**       | **22**     | |
| **CS**   | CS          | **D5**         | **9**      | **Requires DIO Mode** |
| **CLK**  | CLK         | **SCK**        | **18**     | VSPI |
| **DIN**  | DIN         | **MOSI**       | **23**     | VSPI |

## Consequence of DIO Mode
*   **Flash Speed**: Reading from flash is slightly slower in DIO mode compared to QIO.
*   **Impact**: For this application (updating an e-paper display once per minute), the impact is **negligible**. The stability gain outweighs the theoretical speed loss.

## Status
✅ The system is fully operational with the 7.5" display.
✅ Code changes have been committed and pushed to GitHub.
