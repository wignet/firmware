#pragma once

#include <Arduino.h>

// Where the frame's power comes from, read from the PhotoPainter's AXP2101
// (backoffice/power.py, HARDWARE.md "Power"), or a battery voltage
// divider and charge pin on the LCD boards (WIGNET_BATTERY_ADC_PIN,
// docs/lcd_154.md). Other targets can't sense it: available() is false and
// nothing is reported.
namespace PowerHAL {

struct Report {
  bool valid = false;     // the power chip answered
  bool external = false;  // VBUS present: mains, a charger or a computer
  int batteryPercent = -1; // fuel gauge, -1 when unknown
  int batteryMv = -1;      // battery voltage, -1 when unknown
  int charging = -1;       // 1 charging, 0 not, -1 unknown
};

// Enable the battery voltage ADC and the fuel gauge. Call after Wire is up.
void begin();

bool available();

Report read();

// "&pwr=ext&bat=87&mv=3920&chg=1", or "" when nothing can be read.
String queryParams();

// For the USB bench: raw registers and what they decode to.
void describe(Print &out);

} // namespace PowerHAL
