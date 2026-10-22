#include "power_hold_hal.h"

#if defined(WIGNET_POWER_HOLD_PIN)
#include <Arduino.h>

static constexpr int HOLD_PIN = WIGNET_POWER_HOLD_PIN;
static constexpr int BUTTON_PIN = WIGNET_POWER_BUTTON_PIN; // active low
static constexpr unsigned long LONG_PRESS_MS = 2000;
static constexpr unsigned long POLL_MS = 50;
static volatile bool shortPress = false;

static void watchButton(void *) {
  // The press that started the board may still be going on: only a press
  // that begins once the button has been released counts.
  bool released = false;
  unsigned long pressedSinceMs = 0;
  for (;;) {
    const bool pressed = digitalRead(BUTTON_PIN) == LOW;
    if (!pressed) {
      if (pressedSinceMs) shortPress = true; // let go before the long press
      released = true;
      pressedSinceMs = 0;
    } else if (released) {
      if (!pressedSinceMs) pressedSinceMs = millis();
      if (millis() - pressedSinceMs >= LONG_PRESS_MS) {
        Serial.println("[Power] PWR held: switching off (stays on while USB powers it)");
        Serial.flush();
        digitalWrite(HOLD_PIN, LOW);
        // On USB the board keeps running: wait for the button to be let go,
        // so the same press does not repeat.
        while (digitalRead(BUTTON_PIN) == LOW) vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        digitalWrite(HOLD_PIN, HIGH);
        released = true;
        pressedSinceMs = 0;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
}

namespace PowerHoldHAL {

void begin() {
  pinMode(HOLD_PIN, OUTPUT);
  digitalWrite(HOLD_PIN, HIGH);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
}

void startTask() {
  xTaskCreatePinnedToCore(watchButton, "power", 3072, nullptr, 1, nullptr, 1);
}

bool takeShortPress() {
  if (!shortPress) return false;
  shortPress = false;
  return true;
}

} // namespace PowerHoldHAL

#else

namespace PowerHoldHAL {
void begin() {}
void startTask() {}
bool takeShortPress() { return false; }
} // namespace PowerHoldHAL

#endif
