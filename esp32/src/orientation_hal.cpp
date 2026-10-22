#include "orientation_hal.h"
#include "board.h"

#if defined(WIGNET_HAS_IMU)
#include <Arduino.h>
#include <Wire.h>

// QMI8658 on the Waveshare ESP32-S3-LCD-1.54 (Waveshare's bsp_qmi8658.c).
static constexpr int I2C_SDA = WIGNET_I2C_SDA;
static constexpr int I2C_SCL = WIGNET_I2C_SCL;
static constexpr uint8_t ADDRESSES[] = {0x6B, 0x6A};
static constexpr uint8_t REG_WHO_AM_I = 0x00; // reads 0x05
static constexpr uint8_t REG_CTRL1 = 0x02;
static constexpr uint8_t REG_CTRL2 = 0x03;
static constexpr uint8_t REG_CTRL7 = 0x08;
static constexpr uint8_t REG_AX_L = 0x35;
static constexpr uint8_t REG_RESET = 0x60;

// The sensor's axes on the screen, as the panel shows an unturned page:
// screen x to the right, screen y up. Each is one sensor axis (0 x, 1 y,
// 2 z) and a sign, per board (board.h). Measured on the 1.54" bench board on
// 2026-10-16: lying screen up it reads z = -1 g, so the sensor faces away
// from the screen (mirrored), and USB on the right reads x = -1 g and needs
// a 90 degree turn.
static constexpr int SCREEN_X_AXIS = WIGNET_IMU_SCREEN_X_AXIS, SCREEN_X_SIGN = WIGNET_IMU_SCREEN_X_SIGN;
static constexpr int SCREEN_Y_AXIS = WIGNET_IMU_SCREEN_Y_AXIS, SCREEN_Y_SIGN = WIGNET_IMU_SCREEN_Y_SIGN;
static constexpr int SCREEN_Z_SIGN = WIGNET_IMU_SCREEN_Z_SIGN;

static constexpr unsigned long POLL_MS = 200;
static constexpr unsigned long FOLLOW_POLL_MS = 20;
// A new turn must hold this long: no flicker while it is being moved.
static constexpr unsigned long SETTLE_MS = 600;
// Counts at +-4 g full scale.
static constexpr float COUNTS_PER_G = 8192.0f;

static uint8_t address = 0;
static int currentTurn = 0;
static int candidate = -1;
static unsigned long candidateSinceMs = 0;
static unsigned long lastPollMs = 0;
// The last reading, screen axes (gravity()); written by the polling task.
static volatile float lastX = 0, lastY = 0, lastZ = 0;
static volatile bool haveReading = false;
static volatile bool following = false;

static bool writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

static bool readRegisters(uint8_t reg, uint8_t *out, size_t length) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(address, static_cast<uint8_t>(length)) != length) return false;
  for (size_t i = 0; i < length; i++) out[i] = Wire.read();
  return true;
}

static bool readAcceleration(float g[3]) {
  uint8_t raw[6];
  if (!readRegisters(REG_AX_L, raw, sizeof(raw))) return false;
  for (int i = 0; i < 3; i++)
    g[i] = static_cast<int16_t>(raw[2 * i] | (raw[2 * i + 1] << 8)) / COUNTS_PER_G;
  return true;
}

// The turn that puts the page's top at the edge facing up, or -1 when no
// edge clearly does (lying flat, or near a diagonal).
static int turnFor(const float g[3]) {
  // At rest the sensor reads +1 g along the direction pointing up.
  const float upX = SCREEN_X_SIGN * g[SCREEN_X_AXIS];
  const float upY = SCREEN_Y_SIGN * g[SCREEN_Y_AXIS];
  const float inPlane = sqrtf(upX * upX + upY * upY);
  if (inPlane < 0.5f) return -1; // flat on a table: keep the last turn
  // At least 35 degrees from the diagonal, so the turn does not hesitate.
  if (fabsf(upY) > 1.43f * fabsf(upX)) return upY > 0 ? 0 : 180;
  if (fabsf(upX) > 1.43f * fabsf(upY)) return upX > 0 ? 90 : 270;
  return -1;
}

namespace OrientationHAL {

bool begin() {
  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  for (uint8_t candidateAddress : ADDRESSES) {
    address = candidateAddress;
    uint8_t id = 0;
    if (readRegisters(REG_WHO_AM_I, &id, 1) && id == 0x05) break;
    address = 0;
  }
  if (!address) {
    Serial.println("[Orientation] No QMI8658 answered; the turn stays 0");
    return false;
  }
  writeRegister(REG_RESET, 0xB0);
  delay(50);
  writeRegister(REG_CTRL1, 0x40); // register auto-increment
  writeRegister(REG_CTRL2, 0x15); // accelerometer +-4 g, about 235 Hz
  writeRegister(REG_CTRL7, 0x01); // accelerometer only
  delay(30);
  float g[3];
  if (readAcceleration(g)) {
    const int found = turnFor(g);
    if (found >= 0) currentTurn = found;
    Serial.printf("[Orientation] QMI8658 at 0x%02X: x=%.2f y=%.2f z=%.2f g, turn %d\n",
                  address, g[0], g[1], g[2], currentTurn);
  }
  return true;
}

// One reading; true when a new turn has held for SETTLE_MS.
static bool sample() {
  float g[3];
  if (!readAcceleration(g)) return false;
  lastX = SCREEN_X_SIGN * g[SCREEN_X_AXIS];
  lastY = SCREEN_Y_SIGN * g[SCREEN_Y_AXIS];
  lastZ = SCREEN_Z_SIGN * g[3 - SCREEN_X_AXIS - SCREEN_Y_AXIS];
  haveReading = true;
  if (following) {
    candidate = -1;
    return false;
  }
  const int found = turnFor(g);
  if (found < 0 || found == currentTurn) {
    candidate = -1;
    return false;
  }
  if (found != candidate) {
    candidate = found;
    candidateSinceMs = millis();
    return false;
  }
  if (millis() - candidateSinceMs < SETTLE_MS) return false;
  Serial.printf("[Orientation] x=%.2f y=%.2f z=%.2f g: turn %d -> %d\n",
                g[0], g[1], g[2], currentTurn, found);
  currentTurn = found;
  candidate = -1;
  return true;
}

bool poll() {
  if (!address || millis() - lastPollMs < POLL_MS) return false;
  lastPollMs = millis();
  return sample();
}

int turn() { return currentTurn; }

bool gravity(float &x, float &y, float &z) {
  x = lastX;
  y = lastY;
  z = lastZ;
  return haveReading;
}

void follow(bool on) {
  if (on != following) Serial.printf("[Orientation] %s\n", on ? "Following the tilt: turn held" : "Turn free again");
  following = on;
}

static void (*turnHandler)(int) = nullptr;

static void pollTask(void *) {
  for (;;) {
    if (sample() && turnHandler) turnHandler(currentTurn);
    vTaskDelay(pdMS_TO_TICKS(following ? FOLLOW_POLL_MS : POLL_MS));
  }
}

void startTask(void (*onTurn)(int degrees)) {
  if (!address || turnHandler) return;
  turnHandler = onTurn;
  // Core 1 with the loop, one priority above it: a few ms every 200 ms.
  xTaskCreatePinnedToCore(pollTask, "orientation", 4096, nullptr, 2, nullptr, 1);
}

} // namespace OrientationHAL

#else

namespace OrientationHAL {
bool begin() { return false; }
bool poll() { return false; }
void startTask(void (*)(int)) {}
int turn() { return 0; }
bool gravity(float &, float &, float &) { return false; }
void follow(bool) {}
} // namespace OrientationHAL

#endif
