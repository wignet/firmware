#include "usb_awake_hal.h"

#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)

#include <Arduino.h>
#include <Wire.h>
#include <esp_sleep.h>

namespace {

// PhotoPainter schematic: AXP2101 IRQ is connected to ESP32-S3 GPIO21.
constexpr uint8_t PMIC_ADDRESS = 0x34;
constexpr gpio_num_t PMIC_IRQ_PIN = GPIO_NUM_21;
constexpr uint8_t IRQ_ENABLE_0 = 0x40;
constexpr uint8_t IRQ_ENABLE_1 = 0x41;
constexpr uint8_t IRQ_ENABLE_2 = 0x42;
constexpr uint8_t IRQ_STATUS_0 = 0x48;
constexpr uint8_t IRQ_STATUS_1 = 0x49;
constexpr uint8_t IRQ_STATUS_2 = 0x4A;
constexpr uint8_t VBUS_INSERT_IRQ = 0x80;

bool readRegister(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(PMIC_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(PMIC_ADDRESS, static_cast<uint8_t>(1)) != 1)
    return false;
  value = Wire.read();
  return true;
}

bool writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(PMIC_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool clearPendingInterrupts(uint8_t &vbusStatus) {
  uint8_t statuses[3] = {};
  for (uint8_t i = 0; i < 3; ++i) {
    if (!readRegister(IRQ_STATUS_0 + i, statuses[i])) return false;
  }
  vbusStatus = statuses[1] & VBUS_INSERT_IRQ;
  // These registers are write-one-to-clear. Clear all pending sources before
  // enabling a level-sensitive wake, or an old IRQ could cause a wake loop.
  for (uint8_t i = 0; i < 3; ++i) {
    if (statuses[i] && !writeRegister(IRQ_STATUS_0 + i, statuses[i]))
      return false;
  }
  return true;
}

} // namespace

namespace UsbAwakeHAL {

bool hostConnected() { return Serial.isPlugged(); }

void begin() {
  pinMode(static_cast<int>(PMIC_IRQ_PIN), INPUT_PULLUP);
  uint8_t vbusStatus = 0;
  if (!clearPendingInterrupts(vbusStatus)) {
    Serial.println("[USB] AXP2101 IRQ status unavailable");
  } else if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.printf("[USB] PMIC wake; VBUS insert IRQ=%u\n",
                  vbusStatus ? 1U : 0U);
  }
}

void prepareSleep() {
  // This firmware does not consume other AXP2101 interrupts. Limit its IRQ
  // output to VBUS insertion while asleep so unrelated PMIC events cannot
  // repeatedly wake the ESP32.
  if (!writeRegister(IRQ_ENABLE_0, 0) ||
      !writeRegister(IRQ_ENABLE_1, VBUS_INSERT_IRQ) ||
      !writeRegister(IRQ_ENABLE_2, 0)) {
    Serial.println("[USB] Could not configure PMIC VBUS wake; timer remains");
    return;
  }
  uint8_t ignored = 0;
  if (!clearPendingInterrupts(ignored) ||
      digitalRead(static_cast<int>(PMIC_IRQ_PIN)) == LOW) {
    Serial.println("[USB] PMIC IRQ still active; skipping USB wake source");
    return;
  }
  if (esp_sleep_enable_ext1_wakeup(1ULL << PMIC_IRQ_PIN,
                                  ESP_EXT1_WAKEUP_ANY_LOW) != ESP_OK) {
    Serial.println("[USB] Could not arm GPIO21 wake; timer remains");
  }
}

} // namespace UsbAwakeHAL

#endif
