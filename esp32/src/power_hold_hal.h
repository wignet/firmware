#pragma once
/**
 * A board that keeps itself on from its battery (WIGNET_POWER_HOLD_PIN).
 *
 * On the Waveshare ESP32-S3-LCD-1.54 the battery only feeds the board while
 * GPIO2 is held high: PWR starts it, and the firmware must take over before
 * the button is released, or the board stops as soon as USB is unplugged.
 * A long press on PWR lets go of it again, which is the only way to switch
 * the board off on battery (docs/lcd_154.md).
 */
namespace PowerHoldHAL {

// Holds the power on. First thing in setup(): PWR may still be held down.
void begin();

// Watches PWR in a task of its own (setup() may sit in the Wi-Fi portal):
// a long press, from a press that starts after boot, switches the board off.
void startTask();

// A short press of PWR (let go before the 2 s that switch off) since the
// last call: true once per press. Scenes bind it as pwr.short (pwr_short1).
bool takeShortPress();

} // namespace PowerHoldHAL
