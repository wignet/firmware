#include <Arduino.h>

#include "power_hal.h"
#include "board.h"

#if defined(WIGNET_POWER_AXP2101)

#include <Wire.h>

namespace {

// AXP2101 registers (datasheet v1.0 register map; the same ones XPowersLib
// uses). Checked against the PhotoPainter and the AMOLED 1.8" with the bench
// `power` command.
constexpr uint8_t PMIC_ADDRESS = 0x34;
constexpr uint8_t STATUS_1 = 0x00;         // bit5 VBUS good, bit3 battery present
constexpr uint8_t STATUS_2 = 0x01;         // bits6:5 battery current direction
constexpr uint8_t GAUGE_CONTROL = 0x18;    // bit3 fuel gauge enable
constexpr uint8_t ADC_ENABLE = 0x30;       // bit0 battery voltage ADC
constexpr uint8_t VBAT_HIGH = 0x34;        // battery voltage, 14 bits, mV
constexpr uint8_t VBAT_LOW = 0x35;
constexpr uint8_t BATTERY_PERCENT = 0xA4;  // fuel gauge, 0-100
constexpr uint8_t LDO_ENABLE = 0x90;       // bit2 ALDO3, the codec's rail (audio_hal.cpp)
constexpr uint8_t ALDO3 = 0x04;

bool readRegisterOnce(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(PMIC_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(PMIC_ADDRESS, static_cast<uint8_t>(1)) != 1)
    return false;
  value = Wire.read();
  return true;
}

// Twice: on the AMOLED 1.8" the bus also carries the touch and the IMU, and
// about one read in 25 failed on the bench (2026-10-31).
bool readRegister(uint8_t reg, uint8_t &value) {
  return readRegisterOnce(reg, value) || readRegisterOnce(reg, value);
}

bool setBits(uint8_t reg, uint8_t bits) {
  uint8_t value = 0;
  if (!readRegister(reg, value)) return false;
  if ((value & bits) == bits) return true;
  Wire.beginTransmission(PMIC_ADDRESS);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(value | bits));
  return Wire.endTransmission(true) == 0;
}

} // namespace

namespace PowerHAL {

void begin() {
#if defined(WIGNET_AUDIO_RAIL_AXP2101)
  // ALDO3 as found at boot, before any sound switches it on (playTune,
  // toneRing): after a cold power-up (battery and USB out), the AXP2101's
  // own default. Whether that is on decides what leaving it on after a ring
  // costs in deep sleep (fablamenas/wignet#56).
  uint8_t ldo = 0;
  if (readRegister(LDO_ENABLE, ldo))
    Serial.printf("[Power] AXP2101 at boot: 90=%02X, ALDO3 (codec rail) %s\n", ldo, ldo & ALDO3 ? "on" : "off");
#endif
  // Only set the bits this needs: the rest of these registers belongs to
  // the charger and the other ADCs, configured elsewhere.
  if (!setBits(ADC_ENABLE, 0x01) || !setBits(GAUGE_CONTROL, 0x08))
    Serial.println("[Power] AXP2101 ADC or gauge could not be enabled");
}

bool available() { return true; }

Report read() {
  Report report;
  uint8_t status1 = 0, status2 = 0;
  if (!readRegister(STATUS_1, status1) || !readRegister(STATUS_2, status2))
    return report;
  report.valid = true;
  report.external = status1 & 0x20;
  const bool battery = status1 & 0x08;
  if (battery) {
    uint8_t high = 0, low = 0, percent = 0;
    if (readRegister(VBAT_HIGH, high) && readRegister(VBAT_LOW, low)) {
      const int mv = ((high & 0x3F) << 8) | low;
      if (mv >= 2000 && mv <= 5500) report.batteryMv = mv;
    }
    if (readRegister(BATTERY_PERCENT, percent) && percent <= 100)
      report.batteryPercent = percent;
    const uint8_t direction = (status2 >> 5) & 0x03;  // 1 charge, 2 discharge
    report.charging = direction == 1 ? 1 : 0;
  }
  return report;
}

String queryParams() {
  const Report report = read();
  if (!report.valid) return "";
  String out = report.external ? "&pwr=ext" : "&pwr=bat";
  if (report.batteryPercent >= 0) out += "&bat=" + String(report.batteryPercent);
  if (report.batteryMv >= 0) out += "&mv=" + String(report.batteryMv);
  if (report.charging >= 0) out += report.charging ? "&chg=1" : "&chg=0";
  return out;
}

void describe(Print &out) {
  const uint8_t regs[] = {STATUS_1, STATUS_2, GAUGE_CONTROL, ADC_ENABLE,
                          VBAT_HIGH, VBAT_LOW, BATTERY_PERCENT, LDO_ENABLE};
  out.print("[bench] axp2101");
  for (uint8_t reg : regs) {
    uint8_t value = 0;
    if (readRegister(reg, value)) out.printf(" %02X=%02X", reg, value);
    else out.printf(" %02X=??", reg);
  }
  out.println();
  const Report r = read();
  out.printf("[bench] power valid=%d external=%d battery=%d%% %d mV charging=%d -> %s\n",
             r.valid, r.external, r.batteryPercent, r.batteryMv, r.charging,
             queryParams().c_str());
}

} // namespace PowerHAL

#elif defined(WIGNET_BATTERY_ADC_PIN)

// An LCD board's battery: its voltage through a divider on an ADC pin, and
// the charger's status pin (low while charging). Reported only: the board
// stays awake whatever its power (WIGNET_ALWAYS_POWERED, stays_awake1) until
// battery power management lands.
//
// Nothing on the board senses USB power itself, so `pwr=ext` means one of
// two certain signs: the charger is charging, or a USB host (a computer)
// sends frames to the S3's own USB port. A wall charger with a full battery
// shows neither, and reads as battery.
namespace {

#if !defined(WIGNET_BATTERY_DIVIDER)
#define WIGNET_BATTERY_DIVIDER 3
#endif
constexpr int SAMPLES = 16;

// A single-cell LiPo at rest, millivolts to percent. Rough, and only a
// level for the owner: the server keeps the millivolts for the drain.
constexpr int CURVE[][2] = {{4150, 100}, {4050, 90}, {3970, 80}, {3900, 70}, {3840, 60},
                            {3790, 50}, {3750, 40}, {3720, 30}, {3690, 20}, {3610, 10},
                            {3400, 0}};

int batteryMv() {
  uint32_t sum = 0;
  for (int i = 0; i < SAMPLES; i++) sum += analogReadMilliVolts(WIGNET_BATTERY_ADC_PIN);
  return static_cast<int>(sum / SAMPLES) * WIGNET_BATTERY_DIVIDER;
}

int percentFor(int mv) {
  if (mv >= CURVE[0][0]) return 100;
  constexpr int n = sizeof(CURVE) / sizeof(CURVE[0]);
  for (int i = 1; i < n; i++) {
    if (mv >= CURVE[i][0]) {
      const int span = CURVE[i - 1][0] - CURVE[i][0];
      return CURVE[i][1] + (mv - CURVE[i][0]) * (CURVE[i - 1][1] - CURVE[i][1]) / span;
    }
  }
  return 0;
}

} // namespace

namespace PowerHAL {

void begin() {
  analogSetPinAttenuation(WIGNET_BATTERY_ADC_PIN, ADC_11db);
#if defined(WIGNET_CHARGE_PIN)
  pinMode(WIGNET_CHARGE_PIN, INPUT_PULLUP);
#endif
}

bool available() { return true; }

Report read() {
  Report report;
  report.valid = true;
#if ARDUINO_USB_MODE
  report.external = Serial.isPlugged(); // as UsbAwakeHAL::hostConnected()
#endif
#if defined(WIGNET_CHARGE_PIN)
  report.external = report.external || digitalRead(WIGNET_CHARGE_PIN) == LOW;
#endif
  const int mv = batteryMv();
  // Outside a cell's range: no battery fitted, or a floating pin.
  if (mv >= 3000 && mv <= 4400) {
    report.batteryMv = mv;
    report.batteryPercent = percentFor(mv);
#if defined(WIGNET_CHARGE_PIN)
    report.charging = digitalRead(WIGNET_CHARGE_PIN) == LOW ? 1 : 0;
#endif
  }
  return report;
}

String queryParams() {
  const Report report = read();
  String out = report.external ? "&pwr=ext" : "&pwr=bat";
  if (report.batteryPercent >= 0) out += "&bat=" + String(report.batteryPercent);
  if (report.batteryMv >= 0) out += "&mv=" + String(report.batteryMv);
  if (report.charging >= 0) out += report.charging ? "&chg=1" : "&chg=0";
  return out;
}

void describe(Print &out) {
  const Report r = read();
  out.printf("[bench] battery adc=%d mV x%d -> %d mV, %d%%, charge pin=%d -> %s\n",
             static_cast<int>(analogReadMilliVolts(WIGNET_BATTERY_ADC_PIN)), WIGNET_BATTERY_DIVIDER,
             r.batteryMv, r.batteryPercent,
#if defined(WIGNET_CHARGE_PIN)
             digitalRead(WIGNET_CHARGE_PIN),
#else
             -1,
#endif
             queryParams().c_str());
}

} // namespace PowerHAL

#else

namespace PowerHAL {
void begin() {}
bool available() { return false; }
Report read() { return Report(); }
String queryParams() { return ""; }
void describe(Print &out) { out.println("[bench] power: this board can't sense it"); }
} // namespace PowerHAL

#endif
