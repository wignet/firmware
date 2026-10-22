#pragma once

namespace UsbAwakeHAL {

// PhotoPainter only: true when the native USB Serial/JTAG interface sees an
// active host, whether or not a serial monitor has opened the CDC port.
bool hostConnected();

// Clear a latched PMIC interrupt after a wake.
void begin();

// Arm the PMIC's VBUS-insert interrupt as an additional deep-sleep wake source.
// The RTC alarm and timer remain available if this fails.
void prepareSleep();

} // namespace UsbAwakeHAL
