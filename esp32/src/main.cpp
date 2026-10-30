/**
 * ESP32 Wignet Client + Local Web Server
 *
 * Dual-mode e-paper display controller:
 * 1. Wignet Client: Polls api.wignet.app every minute for BMP image
 * 2. Local Server: HTTP interface for text display and configuration
 *
 * Hardware-agnostic: Uses HAL for display operations
 */

#include "config.h"
#include "firmware_version_gen.h"
#include "alarm_clock_hal.h"
#include "audio_hal.h"
#include "hal/display_hal.h"
#include "wifi_provisioning.h"
#include <Arduino.h>
#include <GxEPD2_BW.h>
#include "usb_console.h"
#include "board.h"

#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6) || defined(WIGNET_BATTERY_ADC_PIN)
#include "power_hal.h"
#endif
#if defined(WIGNET_OTA_CLIENT)
#include "ota_client.h"
#include <esp_ota_ops.h>
// The OTA checks run on the loop task: an ECDSA verification and a TLS
// download on top of fetchSchedule() overflow the default 8 KB stack.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif
#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
#include <esp_heap_caps.h>
#include "usb_awake_hal.h"

// The PlatformIO environment, stamped by firmware_version.py (`&hw=`).
#ifndef WIGNET_HW_TARGET
#define WIGNET_HW_TARGET "unknown"
#endif
#include "wgn6_format.h"
#endif

#if defined(WIGNET_DISPLAY_RGB)
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include "boot_image.h"
#endif
#include <Fonts/Org_01.h> // small print, LCD and e-paper alike
#if !defined(WIGNET_DISPLAY_RGB)
// The e-paper Wi-Fi setup screen, at three sizes (displayProvisioningScreen).
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
// Small print on the start screens: yellow on Spectra 6, its most legible
// ink on black; white on black and white (HARDWARE.md "Start screens").
#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
#define EPAPER_SMALL GxEPD_YELLOW
#else
#define EPAPER_SMALL GxEPD_WHITE
#endif
#endif
#if defined(WIGNET_HAS_IMU)
#include "orientation_hal.h"
#endif
#if defined(WIGNET_LVGL_SPIKE)
#include "lvgl_spike.h"
#endif
#include "scene_player.h"
#if defined(WIGNET_HAS_IMU) && !defined(WIGNET_SELF_ORIENT)
// A panel that is not square says how it is held and the platform lays its
// pages out for it (orientation_report1, docs/lcd_2.md); a square one turns
// its picture itself (WIGNET_SELF_ORIENT, self_orient1, docs/lcd_154.md).
#define WIGNET_ORIENTATION_REPORT 1
#endif
#if defined(WIGNET_DISPLAY_RGB)
// The way the board is held, from the orientation task (lcdOnTurn).
static volatile int lcdHeldTurn = 0;
// Which of the board's own screens is up, to draw it again when the board
// turns (orientation_report1). None once a platform page shows.
enum class LcdScreen { None, Boot, Provisioning, Startup, Welcome };
static volatile LcdScreen lcdScreen = LcdScreen::None;
// welcome_local1: while /schedule says the page is Welcome, the board draws it
// itself (lcdWelcomeScreen) rather than showing /pic.
static bool lcdWelcomeWanted = false;
static void lcdApplyWelcome(const String &payload);
#endif
#if defined(WIGNET_ORIENTATION_REPORT)
// The turn the platform page on screen was laid out for (X-Wignet-Turn).
static int lcdLayoutTurn = 0;
// Set by the orientation task, taken by the loop: report the turn at once.
static volatile bool lcdTurnToReport = false;
// Until a page laid out the new way arrives, fetch every couple of seconds.
static unsigned long lcdRelayoutSinceMs = 0;
static bool lcdRelayoutPending = false;
static constexpr unsigned long LCD_RELAYOUT_POLL_MS = 2000;
static constexpr unsigned long LCD_RELAYOUT_GIVE_UP_MS = 30000;
#endif
#include "power_hold_hal.h"

#if defined(ESP8266)
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
using WignetWebServer = ESP8266WebServer;
#else
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
using WignetWebServer = WebServer;
#endif

// Convenience macro for display access (dereferences the HAL pointer)
#define display (*DisplayHAL::getDisplay())

int getDefaultDisplayRotation() {
#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
  // The integrated 7.3-inch panel is mounted upside-down relative to the
  // controller's native scan direction.
  return 2;
#elif defined(TARGET_WAVESHARE_DRIVER_35) || defined(TARGET_D1_MINI_35)
  // The 3.52" panel is physically mounted in landscape orientation.
  return 1;
#elif defined(WIGNET_DISPLAY_RGB)
  // Drawn upright into the LCD's frame; the HAL turns it on the way out.
  return 0;
#else
  return (DisplayHAL::getWidth() < 400) ? 1 : 0;
#endif
}

// ============ Global Variables ============
WignetWebServer server(HTTP_PORT);
unsigned long lastWignetPoll = 0;
bool wignetEnabled = true;
String lastFetchStatus = "Not started";

// Deep sleep variables
unsigned long bootTime = 0;
bool deepSleepAllowed = false;
int nextWakeupSec = DEFAULT_REFRESH_SEC;
bool wokeFromDeepSleep = false;
String scheduledService = "";
// Set once a fetch stores an ETag this boot, so the cache also works while
// the device stays awake across multiple polls without ever deep-sleeping.
bool etagValidThisBoot = false;
#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
bool usbAwake = false;
unsigned long usbLastSeenMs = 0;
constexpr unsigned long USB_DISCONNECT_GRACE_MS = 5000;
// On mains (VBUS present, a charger as much as a computer) the frame never
// deep-sleeps: it stays awake and syncs every `nextRefreshSec` the platform
// answers, the owner's powered cadence (10 s unless set, and at least;
// docs/power.md). A change of power source triggers a sync at once, so
// the platform learns it and answers the matching sleep.
bool mainsAwake = false;
int lastExternal = -1; // -1 not read yet
unsigned long lastPowerCheckMs = 0;
constexpr unsigned long POWER_CHECK_MS = 2000;
// The server never answers a powered frame less than this; a shorter answer
// (an event edge a second away) must not turn the awake loop into a flood.
constexpr int MAINS_MIN_POLL_SEC = 10;
#endif

// ============ Board Status LED ============

namespace StatusLED {

void begin() {
#if defined(TARGET_FIREBEETLE_75V2)
#if defined(ESP32)
  // GPIO2 is held LOW during deep sleep. Release the hold before trying to
  // turn the status LED back on after a timer wakeup.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(static_cast<gpio_num_t>(LED_BUILTIN));
#endif
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.println("[LED] FireBeetle status LED on");
#endif
}

void offForSleep() {
#if defined(TARGET_FIREBEETLE_75V2)
  digitalWrite(LED_BUILTIN, LOW);
#if defined(ESP32)
  // Keep GPIO2/D9 LOW after the digital GPIO domain powers down in deep
  // sleep, otherwise the onboard blue LED can light again from a floating
  // pin.
  gpio_hold_en(static_cast<gpio_num_t>(LED_BUILTIN));
  gpio_deep_sleep_hold_en();
#endif
  Serial.println("[LED] FireBeetle status LED off for deep sleep");
#endif
}

} // namespace StatusLED

// ============ BMP Header Structure ============
struct BMPHeader {
  uint32_t fileSize;
  uint32_t dataOffset;
  int32_t width;
  int32_t height;
  uint16_t bitsPerPixel;
  uint32_t compression;
  uint32_t imageSize;
  bool topDown;
};

// ============ BMP Parsing Functions ============

// A stalled image download fails after this long without a byte.
constexpr unsigned long IMAGE_READ_TIMEOUT_MS = 10000;

// The image decoders take any Stream: an HTTP download, or the USB bench
// (usb_console.cpp "image"), which feeds them the same bytes.
bool readExact(Stream &client, uint8_t *buffer, size_t length) {
  return client.readBytes(buffer, length) == length;
}

uint16_t readLe16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) |
         (static_cast<uint16_t>(p[1]) << 8U);
}

uint32_t readLe32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8U) |
         (static_cast<uint32_t>(p[2]) << 16U) |
         (static_cast<uint32_t>(p[3]) << 24U);
}

bool beginHttpRequest(HTTPClient &http, WiFiClient &client, const String &url) {
#if defined(ESP8266)
  return http.begin(client, url);
#else
  (void)client;
  return http.begin(url);
#endif
}

bool parseBMPHeaderAfterSignature(Stream &client, BMPHeader &header) {
  // The two-byte "BM" signature has already been consumed. Read through the
  // imageSize field (38 bytes total from the beginning of the BMP).
  uint8_t bytes[36];
  if (!readExact(client, bytes, sizeof(bytes))) {
    Serial.println("Truncated BMP header");
    return false;
  }

  header.fileSize = readLe32(bytes);
  header.dataOffset = readLe32(bytes + 8);
  const uint32_t dibHeaderSize = readLe32(bytes + 12);
  const uint32_t rawWidth = readLe32(bytes + 16);
  const uint32_t rawHeight = readLe32(bytes + 20);
  memcpy(&header.width, &rawWidth, sizeof(header.width));
  memcpy(&header.height, &rawHeight, sizeof(header.height));
  const uint16_t planes = readLe16(bytes + 24);
  header.bitsPerPixel = readLe16(bytes + 26);
  header.compression = readLe32(bytes + 28);
  header.imageSize = readLe32(bytes + 32);

  if (dibHeaderSize < 40 || planes != 1 || header.width <= 0 ||
      header.height == 0 || header.height == INT32_MIN ||
      header.dataOffset < 38) {
    Serial.println("Invalid BMP header");
    return false;
  }

  // Height can be negative (top-down bitmap)
  header.topDown = header.height < 0;
  if (header.topDown)
    header.height = -header.height;

  Serial.printf("BMP: %dx%d, %d bpp, offset=%d\n", header.width, header.height,
                header.bitsPerPixel, header.dataOffset);

  // Only support uncompressed 1-bit monochrome.
  if (header.bitsPerPixel != 1 || header.compression != 0) {
    Serial.printf("Unsupported BMP format: %d bpp, compression=%u\n",
                  header.bitsPerPixel,
                  static_cast<unsigned>(header.compression));
    return false;
  }

  return true;
}

bool parseBMPHeader(Stream &client, BMPHeader &header) {
  uint8_t signature[2];
  if (!readExact(client, signature, sizeof(signature)) ||
      signature[0] != 'B' || signature[1] != 'M') {
    Serial.println("Invalid BMP signature");
    return false;
  }
  return parseBMPHeaderAfterSignature(client, header);
}

bool displayBMPFromStream(Stream &client, BMPHeader &header) {
  // We already read 38 bytes in parseBMPHeader
  int toSkip = header.dataOffset - 38;
  if (toSkip < 0)
    toSkip = 0; // Should not happen with valid BMP

  // readBytes waits for each byte; read() does not, and on a download that
  // had not delivered these yet it skipped nothing, shifting the image.
  uint8_t skipped;
  while (toSkip > 0) {
    if (client.readBytes(&skipped, 1) != 1) {
      Serial.println("Truncated BMP header");
      return false;
    }
    toSkip--;
  }

  if (header.width <= 0 || header.height <= 0) {
    Serial.println("Invalid BMP dimensions");
    return false;
  }

  // Calculate row size (padded to 4 bytes)
  size_t rowSize = ((static_cast<size_t>(header.width) + 31) / 32) * 4;

#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
  // The 7-color driver uses a paged 4-bpp buffer. The HTTP stream cannot be
  // replayed for every display page, so retain the compact 1-bpp BMP in PSRAM.
  if (rowSize > SIZE_MAX / static_cast<size_t>(header.height)) {
    Serial.println("BMP dimensions overflow buffer size");
    return false;
  }
  size_t imageBytes = rowSize * static_cast<size_t>(header.height);
  uint8_t *imageBuffer = static_cast<uint8_t *>(heap_caps_malloc(
      imageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!imageBuffer) {
    Serial.printf("Failed to allocate %u BMP bytes in PSRAM\n",
                  static_cast<unsigned>(imageBytes));
    return false;
  }
  if (client.readBytes(imageBuffer, imageBytes) != imageBytes) {
    Serial.println("Failed to read complete BMP into PSRAM");
    free(imageBuffer);
    return false;
  }

  display.setRotation(getDefaultDisplayRotation());
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    for (int sourceY = 0; sourceY < header.height; sourceY++) {
      int displayY = header.topDown ? sourceY : (header.height - 1 - sourceY);
      if (displayY >= DisplayHAL::getHeight())
        continue;

      const uint8_t *row = imageBuffer + static_cast<size_t>(sourceY) * rowSize;
      for (int x = 0; x < header.width && x < DisplayHAL::getWidth(); x++) {
        int byteIndex = x / 8;
        int bitIndex = 7 - (x % 8);
        bool pixel = (row[byteIndex] >> bitIndex) & 1;
        display.drawPixel(x, displayY, pixel ? GxEPD_WHITE : GxEPD_BLACK);
      }
    }
  } while (display.nextPage());

  free(imageBuffer);
  Serial.println("BMP display complete");
  return true;
#else
  uint8_t *rowBuffer = (uint8_t *)malloc(rowSize);

  if (!rowBuffer) {
    Serial.println("Failed to allocate row buffer");
    return false;
  }

  // Set rotation: 1 for small screens (2.7"), 0 for large (7.5")
  // Or simply 1 if the user requested 90 degrees for the device.
  int rotation = getDefaultDisplayRotation();
  display.setRotation(rotation);
  display.setFullWindow();
  display.firstPage();

  bool complete = true;
  do {
    display.fillScreen(GxEPD_WHITE);

    for (int y = 0; y < header.height && y < DisplayHAL::getHeight(); y++) {
      // For bottom-up BMPs, we need to read sequentially but draw inverted
      // Since we stream, we read in order and adjust display position
      if (client.readBytes(rowBuffer, rowSize) != rowSize) {
        Serial.printf("Failed to read row %d\n", y);
        complete = false;
        break;
      }

      int displayY = header.topDown ? y : (header.height - 1 - y);
      if (displayY >= DisplayHAL::getHeight())
        continue;

      for (int x = 0; x < header.width && x < DisplayHAL::getWidth(); x++) {
        int byteIndex = x / 8;
        int bitIndex = 7 - (x % 8);
        bool pixel = (rowBuffer[byteIndex] >> bitIndex) & 1;

        // In BMP, 1 = white, 0 = black (for monochrome with default palette)
        display.drawPixel(x, displayY, pixel ? GxEPD_WHITE : GxEPD_BLACK);
      }
    }
  } while (complete && display.nextPage());

  free(rowBuffer);
  if (complete)
    Serial.println("BMP display complete");
  return complete;
#endif
}

struct DecodedImageInfo {
  int width = 0;
  int height = 0;
  bool color = false;
};

#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
bool displayWgn6FromStream(Stream &client,
                           const uint8_t signaturePrefix[2],
                           int responseLength,
                           DecodedImageInfo &info) {
  uint8_t headerBytes[Wgn6::HEADER_SIZE];
  headerBytes[0] = signaturePrefix[0];
  headerBytes[1] = signaturePrefix[1];
  if (!readExact(client, headerBytes + 2, sizeof(headerBytes) - 2)) {
    Serial.println("[WGN6] Truncated header");
    return false;
  }

  Wgn6::Header header;
  if (!Wgn6::decodeHeader(headerBytes, sizeof(headerBytes), header)) {
    Serial.println("[WGN6] Unsupported or invalid header");
    return false;
  }

  size_t expectedLength = 0;
  if (!Wgn6::expectedPayloadLength(header, expectedLength) ||
      header.width != DisplayHAL::getWidth() ||
      header.height != DisplayHAL::getHeight()) {
    Serial.printf("[WGN6] Invalid geometry/payload: %ux%u, %u bytes\n",
                  header.width, header.height,
                  static_cast<unsigned>(header.payloadLength));
    return false;
  }

  if (responseLength >= 0 &&
      static_cast<size_t>(responseLength) != Wgn6::HEADER_SIZE + expectedLength) {
    Serial.printf("[WGN6] HTTP body length mismatch: expected %u, got %d\n",
                  static_cast<unsigned>(Wgn6::HEADER_SIZE + expectedLength),
                  responseLength);
    return false;
  }

  uint8_t *payload = static_cast<uint8_t *>(heap_caps_malloc(
      expectedLength, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!payload) {
    Serial.printf("[WGN6] Could not allocate %u payload bytes in PSRAM\n",
                  static_cast<unsigned>(expectedLength));
    return false;
  }

  bool valid = readExact(client, payload, expectedLength);
  if (!valid) {
    Serial.println("[WGN6] Truncated payload");
  } else {
    const uint32_t actualCrc = Wgn6::crc32(payload, expectedLength);
    if (actualCrc != header.payloadCrc32) {
      Serial.printf("[WGN6] CRC mismatch: expected %08x, got %08x\n",
                    static_cast<unsigned>(header.payloadCrc32),
                    static_cast<unsigned>(actualCrc));
      valid = false;
    }
  }

  if (valid && !Wgn6::validatePalette(header, payload, expectedLength)) {
    Serial.println("[WGN6] Invalid palette value or final padding nibble");
    valid = false;
  }

  // Do not touch the panel until the complete response has passed all header,
  // length, palette, and CRC checks. A broken download therefore leaves the
  // previous e-paper image intact.
  if (valid) {
    valid = DisplayHAL::renderPackedColor(payload, expectedLength, header.width,
                                          header.height);
  }

  heap_caps_free(payload);
  if (!valid) {
    return false;
  }

  info.width = header.width;
  info.height = header.height;
  info.color = true;
  Serial.println("[WGN6] Six-color display complete");
  return true;
}
#endif

#if defined(WIGNET_DISPLAY_RGB)
// ============ PNG (rgb profile, docs/hdmi_tv.md) ============
// The whole file is read into PSRAM and decoded into a scratch frame; the
// panel's frame changes only once the PNG has decoded completely, at the
// panel's exact size, so a bad file leaves the screen as it was.
static constexpr size_t PNG_MAX_BYTES = 1024 * 1024;
static PNG png;
static uint16_t *pngScratch = nullptr;

static int pngDrawLine(PNGDRAW *draw) {
  png.getLineAsRGB565(draw, pngScratch + draw->y * DisplayHAL::getWidth(),
                      PNG_RGB565_BIG_ENDIAN, 0xffffffff);
  return 1;
}

bool displayPngFromStream(Stream &client, const uint8_t signature[2],
                          int responseLength, DecodedImageInfo &info) {
  const int width = DisplayHAL::getWidth(), height = DisplayHAL::getHeight();
  if (responseLength <= 2 || static_cast<size_t>(responseLength) > PNG_MAX_BYTES ||
      !DisplayHAL::frame()) {
    Serial.printf("[PNG] Unusable length %d\n", responseLength);
    return false;
  }
  uint8_t *file = static_cast<uint8_t *>(heap_caps_malloc(responseLength, MALLOC_CAP_SPIRAM));
  if (!pngScratch)
    pngScratch = static_cast<uint16_t *>(
        heap_caps_malloc(width * height * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
  if (!file || !pngScratch) {
    Serial.println("[PNG] Out of PSRAM");
    free(file);
    return false;
  }
  file[0] = signature[0];
  file[1] = signature[1];
  bool ok = readExact(client, file + 2, responseLength - 2);
  if (!ok) Serial.println("[PNG] Truncated response");
  if (ok && png.openRAM(file, responseLength, pngDrawLine) != PNG_SUCCESS) {
    Serial.println("[PNG] Not a PNG");
    ok = false;
  }
  if (ok && (png.getWidth() != width || png.getHeight() != height)) {
    Serial.printf("[PNG] %dx%d, panel is %dx%d\n", png.getWidth(), png.getHeight(), width, height);
    png.close();
    ok = false;
  }
  if (ok) {
    ok = png.decode(nullptr, 0) == PNG_SUCCESS;
    png.close();
    if (!ok) Serial.println("[PNG] Decode failed");
  }
  free(file);
  if (!ok) return false;
  memcpy(DisplayHAL::frame(), pngScratch, width * height * sizeof(uint16_t));
  DisplayHAL::showFrame();
  info.width = width;
  info.height = height;
  info.color = true;
  Serial.println("[PNG] Displayed");
  return true;
}
#endif

bool decodeAndDisplayImage(Stream &client, bool allowColor,
                           int responseLength,
                           DecodedImageInfo &info) {
  uint8_t signature[2];
  if (!readExact(client, signature, sizeof(signature))) {
    Serial.println("Truncated image response");
    return false;
  }

  if (signature[0] == 'B' && signature[1] == 'M') {
    BMPHeader header;
    if (!parseBMPHeaderAfterSignature(client, header) ||
        !displayBMPFromStream(client, header)) {
      return false;
    }
    info.width = header.width;
    info.height = header.height;
    info.color = false;
    return true;
  }

#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
  if (allowColor && signature[0] == 'W' && signature[1] == 'G') {
    return displayWgn6FromStream(client, signature, responseLength, info);
  }
#elif defined(WIGNET_DISPLAY_RGB)
  if (allowColor && signature[0] == 0x89 && signature[1] == 'P') {
    return displayPngFromStream(client, signature, responseLength, info);
  }
#else
  (void)allowColor;
#endif

  Serial.printf("Unsupported image signature: %02x %02x\n", signature[0],
                signature[1]);
  return false;
}

// ============ Schedule Functions ============

int fetchSchedule() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Schedule] WiFi not connected, using default");
    return DEFAULT_REFRESH_SEC;
  }

  HTTPClient http;
  String mac = WiFi.macAddress();
  String url = String("http://") + WIGNET_HOST + ":" + String(WIGNET_PORT) +
               WIGNET_SCHEDULE_PATH + "?mac=" + mac +
               "&w=" + String(DisplayHAL::getWidth()) +
               "&h=" + String(DisplayHAL::getHeight());
#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
  // Functional, firmware-level declaration. Keep it off the image URL so
  // image cache keys and ETags remain unchanged.
  // tune_mml1: plays MML v1 alarm tunes (mml_tune.cpp, docs/mml.md).
  // power_report1: the pwr/bat/mv/chg below (power_hal.cpp, docs/power.md).
  // sound_list1: holds the upcoming sounds of /schedule `sounds`
  // (alarm_clock_hal.cpp, sound_list.cpp).
  // spectra6: draws WGN6 six-colour frames; this is what switches the
  // device to colour on the server (docs/wgn6.md).
  url += "&caps=v1%3Awake_alarm%2Caudio_out%2Ctune_mml1%2Cpower_report1%2Csound_list1%2Cspectra6";
#if defined(WIGNET_OTA_CAPABLE)
  // ota1: this board installs firmware offered on /schedule (docs/ota.md).
  url += "%2Cota1";
#endif
  url += PowerHAL::queryParams();
#elif defined(WIGNET_DISPLAY_RGB)
  // The colour LCD boards (docs/lcd_154.md, docs/lcd_2.md), from features:
  // rgb + screen_size1: full-colour PNGs at this panel's own w/h.
  url += "&caps=v1%3Argb%2Cscreen_size1";
  // welcome_local1: draws the Welcome page itself from /schedule `welcome`
  // (the owner page's address and QR modules), crisp at its own size.
  url += "%2Cwelcome_local1";
#if defined(WIGNET_SELF_ORIENT)
  // self_orient1: the accelerometer turns the upright page itself (square
  // panels only); ori= is the turn applied now, for the owner's page.
  url += "%2Cself_orient1";
#elif defined(WIGNET_ORIENTATION_REPORT)
  // orientation_report1: ori= is how the board is held, and the platform
  // lays its pages out for it (docs/lcd_2.md).
  url += "%2Corientation_report1";
#endif
#if defined(WIGNET_ALWAYS_POWERED)
  // power_report1: pwr= is reported. stays_awake1: the board never sleeps,
  // on battery too, so the platform keeps the powered cadence whatever pwr=
  // says (docs/power.md); pwr= is for the owner to see.
  url += "%2Cpower_report1%2Cstays_awake1";
#endif
#if defined(WIGNET_HAS_AUDIO)
  // wake_alarm, audio_out, tune_mml1, sound_list1: the ES8311 speaker, as
  // the PhotoPainter's (audio_hal.cpp, alarm_clock_hal.cpp).
  url += "%2Cwake_alarm%2Caudio_out%2Ctune_mml1%2Csound_list1";
#endif
#if defined(WIGNET_SCENE_PLAYER)
  // scene2: plays scenes with the scene runtime 2 (scene_player.cpp,
  // scene_runtime.cpp, docs/scene_runtime.md); in= the buttons a scene may
  // use, sev= the last scene event received. It stands for what scene1,
  // scene_turn1, scene_avoid1 and, with audio_out, scene_sound1 and
  // scene_tone1 said: besides it a scene player declares hardware alone.
  url += "%2Cscene2";
  // scene_i4: sprite sheets may come as I4 (scene_player.cpp describe),
  // a sixth of RGB565A8 (docs/scene_runtime.md "Indexed pictures").
  url += "%2Cscene_i4";
  // scene_3mb: a bundle of up to 3 MB held raw (scene_player.cpp
  // BUNDLE_MAX_BYTES); without it the server keeps scenes to 1.5 MB.
  url += "%2Cscene_3mb";
#if defined(WIGNET_HAS_IMU)
  // motion1: tilt values and tilt/shake/face_down events (OrientationHAL::gravity).
  url += "%2Cmotion1";
#endif
#if defined(WIGNET_POWER_HOLD_PIN)
  // pwr_short1: a short press of PWR is the scene's (PowerHoldHAL::takeShortPress).
  url += "%2Cpwr_short1";
#endif
  url += ScenePlayer::scheduleParams();
#endif
#if defined(WIGNET_BATTERY_ADC_PIN)
  // pwr=ext and, with a battery fitted, its level (power_hal.cpp).
  url += PowerHAL::queryParams();
#elif defined(WIGNET_ALWAYS_POWERED)
  url += "&pwr=ext";
#endif
#if defined(WIGNET_SELF_ORIENT)
  url += "&ori=" + String(DisplayHAL::turn());
#elif defined(WIGNET_ORIENTATION_REPORT)
  url += "&ori=" + String(lcdHeldTurn);
#endif
#elif defined(TARGET_WAVESHARE_DRIVER_75V2) && defined(WIGNET_OTA_CAPABLE)
  // ota1 only once the USB migration has put the 1.75 MiB slots on the board
  // (partitions/ota_4m.csv): this image flashed alone over the old table
  // can't hold a larger one (docs/ota.md "Board ids").
  {
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    if (next && next->size >= 0x1c0000) url += "&caps=v1%3Aota1";
  }
#endif
  // The Wi-Fi networks this Wignet knows, never passwords (docs/wifi.md).
  {
    const String report = wifiReportJson();
    String encoded;
    for (size_t i = 0; i < report.length(); i++) {
      const unsigned char c = report[i];
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
        encoded += static_cast<char>(c);
      } else {
        char hex[4];
        snprintf(hex, sizeof(hex), "%%%02X", c);
        encoded += hex;
      }
    }
    url += "&wifi=" + encoded;
  }
  // Git commit this image was built from (firmware_version.py); the
  // backoffice reports it and how many firmware commits the device is behind.
  url += "&fw=" WIGNET_FW_VERSION;
  // The board this build is for, so the platform can tell boards apart.
  url += "&hw=" WIGNET_HW_TARGET;
#if defined(WIGNET_OTA_CAPABLE)
  // on=, and oh=/ota=/oc= while an update is pending or reported (ota_client.cpp).
  url += OtaClient::pollQuery();
#endif

  Serial.println("[Schedule] Fetching: " + url);

  WiFiClient client;
  if (!beginHttpRequest(http, client, url)) {
    Serial.println("[Schedule] Failed to initialize HTTP request");
    return DEFAULT_REFRESH_SEC;
  }

  http.setTimeout(10000); // 10 second timeout

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    // With the list of sounds and its tunes the answer runs to kilobytes:
    // log its start only.
    if (payload.length() > 600) {
      Serial.printf("[Schedule] Response (%u bytes): %s...\n",
                    static_cast<unsigned>(payload.length()), payload.substring(0, 600).c_str());
    } else {
      Serial.println("[Schedule] Response: " + payload);
    }
#if defined(WIGNET_HAS_AUDIO)
    AlarmClockHAL::updateFromSchedule(payload);
#endif
#if defined(WIGNET_SCENE_PLAYER)
    ScenePlayer::onSchedule(payload);
#endif

#if defined(WIGNET_OTA_CLIENT)
    OtaClient::onSchedule(payload); // otaOffer / otaConfirm, within 60 s of the poll
#endif
    applyWifiForget(payload); // networks the owner asked this Wignet to forget
#if defined(WIGNET_DISPLAY_RGB)
    lcdApplyWelcome(payload);
#endif
    int serviceKey = payload.indexOf("\"currentService\"");
    if (serviceKey >= 0) {
      int colonIdx = payload.indexOf(':', serviceKey);
      int quoteStart = payload.indexOf('"', colonIdx + 1);
      int quoteEnd = payload.indexOf('"', quoteStart + 1);
      if (colonIdx >= 0 && quoteStart >= 0 && quoteEnd > quoteStart) {
        scheduledService = payload.substring(quoteStart + 1, quoteEnd);
        Serial.println("[Schedule] Current service: " + scheduledService);
      }
    }

    // Simple JSON parsing for nextRefreshSec
    // Expected format: {"nextRefreshSec": 180, ...}
    int idx = payload.indexOf("\"nextRefreshSec\"");
    if (idx >= 0) {
      int colonIdx = payload.indexOf(':', idx);
      int commaIdx = payload.indexOf(',', colonIdx);
      int braceIdx = payload.indexOf('}', colonIdx);
      int endIdx = (commaIdx > 0 && commaIdx < braceIdx) ? commaIdx : braceIdx;

      if (colonIdx > 0 && endIdx > colonIdx) {
        String valueStr = payload.substring(colonIdx + 1, endIdx);
        valueStr.trim();
        int value = valueStr.toInt();
        if (value > 0) {
#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
          // An older server can request a day-long sleep without exposing the
          // alarm contract. Poll until it is deployed so this frame picks up
          // its first alarm without a manual reset. Other devices are unchanged.
          if (payload.indexOf("\"alarmSyncMaxSec\"") < 0 && value > 900) {
            value = 900;
            Serial.println("[Alarm] Legacy schedule; retrying in 15 minutes");
          }
#endif
          Serial.printf("[Schedule] Next refresh in %d seconds\n", value);
          http.end();
          return value;
        }
      }
    }
    Serial.println("[Schedule] Failed to parse response, using default");
  } else {
    Serial.printf("[Schedule] HTTP error: %d, using default\n", httpCode);
  }

  http.end();
  return DEFAULT_REFRESH_SEC;
}

#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
// A sound plays in its own task (AudioHAL::startAlarm) while the loop fetches
// and draws. Sleep would cut it: let it end first. The buttons still stop it.
void waitForSound() {
  if (!AudioHAL::isPlaying()) return;
  Serial.println("[DeepSleep] Sound playing; sleeping once it ends");
  while (AudioHAL::isPlaying()) delay(50);
}
#endif

// `evenOnUsb`: the USB bench's "sleep" (benchSleep), to test a real wake.
void enterDeepSleep(int seconds, bool evenOnUsb = false) {
#if defined(WIGNET_ALWAYS_POWERED)
  // Plugged in, and an LCD shows nothing asleep: stay awake and sync at the
  // platform's powered cadence (loop()). Battery sleep comes later.
  (void)seconds;
  (void)evenOnUsb;
  return;
#endif
#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
  waitForSound();
  if (!evenOnUsb && UsbAwakeHAL::hostConnected()) {
    if (!usbAwake) Serial.println("[USB] Computer connected; staying awake");
    usbAwake = true;
    usbLastSeenMs = millis();
    return;
  }
  if (!evenOnUsb && PowerHAL::read().external) {
    if (!mainsAwake) Serial.printf("[Power] On mains; staying awake, syncing every %d s\n", max(seconds, MAINS_MIN_POLL_SEC));
    mainsAwake = true;
    return;
  }
#else
  DisplayHAL::hibernate();
#endif

  Serial.printf(
      "[DeepSleep] Waiting 10 seconds before sleep for stability...\n");
  StatusLED::offForSleep();
  delay(10000);

#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
  // A computer may enumerate during the stability delay. Check again before
  // switching off Wi-Fi and USB; the PMIC handles later cable insertion.
  if (!evenOnUsb && UsbAwakeHAL::hostConnected()) {
    Serial.println("[USB] Computer connected; staying awake");
    usbAwake = true;
    usbLastSeenMs = millis();
    return;
  }
  // Plugged in during the stability delay: stay on, like a computer.
  if (!evenOnUsb && PowerHAL::read().external) {
    Serial.printf("[Power] On mains; staying awake, syncing every %d s\n", max(seconds, MAINS_MIN_POLL_SEC));
    mainsAwake = true;
    return;
  }
  DisplayHAL::hibernate();
  // Recompute after the stability delay; otherwise an alarm can ring late by
  // those ten seconds even when the server sent an exact wake interval.
  AlarmClockHAL::ringIfDue();
  waitForSound();
  AlarmClockHAL::prepareSleep(seconds);
  UsbAwakeHAL::prepareSleep();
#endif

  Serial.printf("[DeepSleep] Entering deep sleep for %d seconds...\n", seconds);
  Serial.flush();

  // Disconnect WiFi to save power
#if defined(ESP8266)
  WiFi.disconnect();
#else
  WiFi.disconnect(true);
#endif
  WiFi.mode(WIFI_OFF);

  // Enter deep sleep
#if defined(ESP8266)
  ESP.deepSleep((uint64_t)seconds * 1000000ULL, WAKE_RF_DEFAULT);
  delay(1000);
#else
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
#endif
}

#if defined(WIGNET_BENCH_CONSOLE)
// USB bench "image": a BMP or WGN6 file over USB, drawn by the decoder that
// draws downloads, with all its checks: a bad file leaves the panel alone.
// While the hold lasts the frame neither fetches nor sleeps, so it keeps
// listening for the next image.
bool benchHold = false;
unsigned long benchHoldUntilMs = 0;

bool benchImage(Stream &source, size_t length) {
  DecodedImageInfo info;
  if (!decodeAndDisplayImage(source, true, static_cast<int>(length), info)) return false;
  // Keep it up to be looked at: the platform's next refresh would replace it.
  benchHold = true;
  benchHoldUntilMs = millis() + UsbConsole::BENCH_HOLD_MINUTES * 60000UL;
  return true;
}

void benchResume() { benchHold = false; }

// True while a bench image holds the panel; logs once when the hold ends.
bool benchHolding() {
  if (!benchHold) return false;
  if (static_cast<long>(millis() - benchHoldUntilMs) < 0) return true;
  benchHold = false;
  Serial.println("[Bench] Hold over; platform refresh resumes");
  return false;
}
#endif

#if defined(WIGNET_BENCH_AUDIO)
// USB bench "sleep": deep sleep now, even with the computer connected, to
// test a real wake. The next alarm caps the sleep; with rtcOnly the timer
// backup is two minutes late, so the RTC interrupt has to do the waking.
void benchSleep(bool rtcOnly) {
  AlarmClockHAL::setTimerBackupSlack(rtcOnly ? 120 : 0);
  enterDeepSleep(nextWakeupSec > 0 ? nextWakeupSec : DEFAULT_REFRESH_SEC, true);
  AlarmClockHAL::setTimerBackupSlack(0); // only reached if it did not sleep
}
#endif

// ============ Persisted ETag (survives deep sleep) ============
// Only trusted when wokeFromDeepSleep is true - a cold boot / power loss
// leaves this memory in an undefined state, so it's always ignored then.

static const size_t ETAG_STORAGE_SIZE = 40; // fits a quoted MD5 hex etag (34)

#if defined(ESP8266)
String loadPersistedEtag() {
  uint32_t buf[ETAG_STORAGE_SIZE / 4];
  if (!ESP.rtcUserMemoryRead(0, buf, sizeof(buf))) {
    return String();
  }
  char etag[ETAG_STORAGE_SIZE + 1];
  memcpy(etag, buf, ETAG_STORAGE_SIZE);
  etag[ETAG_STORAGE_SIZE] = '\0';
  return String(etag);
}

void savePersistedEtag(const String &etag) {
  uint32_t buf[ETAG_STORAGE_SIZE / 4];
  memset(buf, 0, sizeof(buf));
  size_t len = etag.length();
  if (len > ETAG_STORAGE_SIZE - 1)
    len = ETAG_STORAGE_SIZE - 1;
  memcpy(buf, etag.c_str(), len);
  ESP.rtcUserMemoryWrite(0, buf, sizeof(buf));
}
#else
RTC_DATA_ATTR char rtcLastEtag[ETAG_STORAGE_SIZE];

String loadPersistedEtag() {
  char etag[ETAG_STORAGE_SIZE + 1];
  memcpy(etag, rtcLastEtag, ETAG_STORAGE_SIZE);
  etag[ETAG_STORAGE_SIZE] = '\0';
  return String(etag);
}

void savePersistedEtag(const String &etag) {
  memset(rtcLastEtag, 0, ETAG_STORAGE_SIZE);
  size_t len = etag.length();
  if (len > ETAG_STORAGE_SIZE - 1)
    len = ETAG_STORAGE_SIZE - 1;
  memcpy(rtcLastEtag, etag.c_str(), len);
}
#endif

// ============ Wignet Client Functions ============

enum class ImageRequestResult {
  Displayed,
  NotModified,
  InvalidImage,
  HttpError,
  // orientation_report1: a page laid out for another turn (the new layout is
  // still rendering); not shown, asked again shortly.
  OtherLayout,
};

ImageRequestResult fetchWignetRepresentation(const String &url,
                                              bool preferColor,
                                              bool sendCachedEtag,
                                              DecodedImageInfo &info) {
  HTTPClient http;
  WiFiClient client;
  if (!beginHttpRequest(http, client, url)) {
    lastFetchStatus = "HTTP init error";
    Serial.println("Failed to initialize HTTP request");
    return ImageRequestResult::HttpError;
  }

  http.setTimeout(30000); // 30 second timeout
  if (preferColor) {
#if defined(WIGNET_DISPLAY_RGB)
    // The PNG URL answers 406 until /schedule has declared rgb; the BMP
    // retry in fetchWignetImage() then shows the 1-bit page meanwhile.
    http.addHeader("Accept", "image/png");
#else
    // This is an additive opt-in on the historical .bmp URL. Old servers
    // ignore it and return BMP; updated servers return WGN6 only when the
    // device is also explicitly assigned the Spectra 6 hardware profile.
    http.addHeader(
        "Accept",
        "application/vnd.wignet.spectra6, image/bmp;q=0.5");
#endif
  } else {
    // Used for ordinary monochrome targets and for the recovery request after
    // a color response fails validation.
    http.addHeader("Accept", "image/bmp");
  }

  const char *etagHeaderKeys[] = {"ETag", "X-Wignet-Turn"};
  http.collectHeaders(etagHeaderKeys, 2);

  // A cold boot / power loss leaves RTC memory undefined, so only trust a
  // stored ETag right after an actual deep-sleep wakeup - or once this boot
  // has already saved one itself (device staying awake across polls without
  // ever deep-sleeping). The explicit BMP recovery request deliberately
  // bypasses a cached color ETag.
  Serial.printf("[ETag] wokeFromDeepSleep=%d etagValidThisBoot=%d\n",
                wokeFromDeepSleep, etagValidThisBoot);
  if (sendCachedEtag && (wokeFromDeepSleep || etagValidThisBoot)) {
    String cachedEtag = loadPersistedEtag();
    Serial.printf("[ETag] Loaded from RTC: %s (len=%d)\n", cachedEtag.c_str(),
                  cachedEtag.length());
    if (cachedEtag.length() > 0) {
      http.addHeader("If-None-Match", cachedEtag);
      Serial.println("[ETag] Sent If-None-Match: " + cachedEtag);
    }
  }

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
#if defined(WIGNET_ORIENTATION_REPORT)
    const int laidOut = http.hasHeader("X-Wignet-Turn") ? http.header("X-Wignet-Turn").toInt()
                                                        : lcdHeldTurn;
    if (preferColor && laidOut != lcdHeldTurn) {
      Serial.printf("[Orientation] Page laid out for %d, held at %d: waiting for the new layout\n",
                    laidOut, lcdHeldTurn);
      lastFetchStatus = "Waiting for the new layout";
      http.end();
      return ImageRequestResult::OtherLayout;
    }
#endif
    WiFiClient *stream = http.getStreamPtr();
    // The default 1 s read timeout failed whole images on a brief Wi-Fi
    // stall ("Failed to read complete BMP into PSRAM"), costing a retry.
    stream->setTimeout(IMAGE_READ_TIMEOUT_MS);
    const int responseLength = http.getSize();
    if (decodeAndDisplayImage(*stream, preferColor, responseLength, info)) {
#if defined(WIGNET_DISPLAY_RGB)
      lcdScreen = LcdScreen::None; // a platform page now, not one of ours
#endif
#if defined(WIGNET_ORIENTATION_REPORT)
      if (preferColor) {
        lcdLayoutTurn = laidOut;
        lcdRelayoutPending = false;
      }
#endif
      lastFetchStatus = "OK - " + String(info.width) + "x" +
                        String(info.height) + (info.color ? " color" : " mono");
      if (http.hasHeader("ETag")) {
        String newEtag = http.header("ETag");
        Serial.println("[ETag] Received ETag: " + newEtag);
        savePersistedEtag(newEtag);
        etagValidThisBoot = true;
      } else {
        Serial.println("[ETag] No ETag header in 200 response");
      }
      http.end();
      return ImageRequestResult::Displayed;
    }
    lastFetchStatus = preferColor ? "Invalid color/image response"
                                  : "Invalid BMP response";
    http.end();
    return ImageRequestResult::InvalidImage;
  } else if (httpCode == HTTP_CODE_NOT_MODIFIED) {
    // Server confirms our cached ETag is still current: skip the full-screen
    // redraw entirely, but stay indistinguishable from a 200 to loop()'s
    // control flow (schedule fetch + deep sleep still happen normally).
    lastFetchStatus = "Not modified (cached)";
    Serial.println("Image unchanged (304), skipping redraw");
    http.end();
    return ImageRequestResult::NotModified;
  } else {
    lastFetchStatus = "HTTP " + String(httpCode);
    Serial.printf("HTTP error: %d\n", httpCode);
  }

  http.end();
  return ImageRequestResult::HttpError;
}

bool fetchWignetImage() {
  if (WiFi.status() != WL_CONNECTED) {
    lastFetchStatus = "WiFi disconnected";
    return false;
  }

  String mac = WiFi.macAddress();
  const String query = String("?mac=") + mac +
                       "&w=" + String(DisplayHAL::getWidth()) +
                       "&h=" + String(DisplayHAL::getHeight());
  String url = String("http://") + WIGNET_HOST + ":" + String(WIGNET_PORT) +
               WIGNET_IMAGE_PATH + query;
#if defined(WIGNET_DISPLAY_RGB)
  // The rgb profile's own URL (docs/hdmi_tv.md); the BMP one stays the
  // fallback below.
  const String bmpUrl = url;
  url = String("http://") + WIGNET_HOST + ":" + String(WIGNET_PORT) +
        "/pic/4displayB5.png" + query;
#endif
  Serial.println("Fetching: " + url);

#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6) || defined(WIGNET_DISPLAY_RGB)
  constexpr bool preferColor = true;
#else
  constexpr bool preferColor = false;
#endif

#if defined(WIGNET_DISPLAY_RGB)
  if (lcdWelcomeWanted) {
    // The board draws Welcome itself: /pic would only cover it.
    lastFetchStatus = "Welcome, drawn here";
    return true;
  }
#endif
  DecodedImageInfo info;
  ImageRequestResult result =
      fetchWignetRepresentation(url, preferColor, true, info);
  if (result == ImageRequestResult::Displayed ||
      result == ImageRequestResult::NotModified) {
    return true;
  }

#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
  if (result == ImageRequestResult::InvalidImage) {
    // A malformed/truncated WGN6 frame never touches the panel. Ask again for
    // the legacy representation explicitly, without the color ETag, so this
    // hardware keeps working even during a bad color deployment.
    Serial.println("[WGN6] Response rejected; retrying explicit BMP fallback");
    DecodedImageInfo fallbackInfo;
    result = fetchWignetRepresentation(url, false, false, fallbackInfo);
    return result == ImageRequestResult::Displayed ||
           result == ImageRequestResult::NotModified;
  }
#elif defined(WIGNET_DISPLAY_RGB)
  if (result == ImageRequestResult::HttpError || result == ImageRequestResult::InvalidImage) {
    // A server that does not know this frame as rgb yet (406), or a PNG it
    // could not draw: the 1-bit BMP still shows something.
    Serial.println("[PNG] Not served; retrying the BMP");
    DecodedImageInfo fallbackInfo;
    result = fetchWignetRepresentation(bmpUrl, false, false, fallbackInfo);
    return result == ImageRequestResult::Displayed ||
           result == ImageRequestResult::NotModified;
  }
#endif

  return false;
}

// ============ Text Display Function ============

// Convert UTF-8 to CP437 for Adafruit_GFX's built-in font.
// The bundled FreeSans GFX fonts are ASCII-only, while this path keeps common
// French accents visible and can be scaled up for 7.5" readability.
String utf8ToDisplayText(String s) {
  String res = "";
  for (unsigned int i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (c < 0x80) {
      res += (char)c;
    } else if (c == 0xC3) {
      i++;
      if (i >= s.length())
        break;

      switch ((uint8_t)s[i]) {
      case 0x80: // À
        res += 'A';
        break;
      case 0x82: // Â
        res += 'A';
        break;
      case 0x84: // Ä
        res += (char)0x8E;
        break;
      case 0x87: // Ç
        res += (char)0x80;
        break;
      case 0x88: // È
        res += 'E';
        break;
      case 0x89: // É
        res += (char)0x90;
        break;
      case 0x8A: // Ê
        res += 'E';
        break;
      case 0x8B: // Ë
        res += 'E';
        break;
      case 0x8E: // Î
        res += 'I';
        break;
      case 0x8F: // Ï
        res += 'I';
        break;
      case 0x94: // Ô
        res += 'O';
        break;
      case 0x96: // Ö
        res += (char)0x99;
        break;
      case 0x99: // Ù
        res += 'U';
        break;
      case 0x9B: // Û
        res += 'U';
        break;
      case 0x9C: // Ü
        res += (char)0x9A;
        break;
      case 0xA0: // à
        res += (char)0x85;
        break;
      case 0xA2: // â
        res += (char)0x83;
        break;
      case 0xA4: // ä
        res += (char)0x84;
        break;
      case 0xA7: // ç
        res += (char)0x87;
        break;
      case 0xA8: // è
        res += (char)0x8A;
        break;
      case 0xA9: // é
        res += (char)0x82;
        break;
      case 0xAA: // ê
        res += (char)0x88;
        break;
      case 0xAB: // ë
        res += (char)0x89;
        break;
      case 0xAE: // î
        res += (char)0x8C;
        break;
      case 0xAF: // ï
        res += (char)0x8B;
        break;
      case 0xB4: // ô
        res += (char)0x93;
        break;
      case 0xB6: // ö
        res += (char)0x94;
        break;
      case 0xB9: // ù
        res += (char)0x97;
        break;
      case 0xBB: // û
        res += (char)0x96;
        break;
      case 0xBC: // ü
        res += (char)0x81;
        break;
      default:
        res += '?';
        break;
      }
    } else if (c == 0xC2) {
      // Non-breaking space, degree sign, etc.
      i++;
      if (i >= s.length())
        break;
      if ((uint8_t)s[i] == 0xA0) {
        res += ' ';
      } else if ((uint8_t)s[i] == 0xB0) {
        res += (char)0xF8;
      } else {
        res += '?';
      }
    } else if (c == 0xE2) {
      // Handle typographic quotes and apostrophes (', ', ", ", …)
      if (i + 2 < s.length()) {
        uint8_t c2 = (uint8_t)s[i + 1];
        uint8_t c3 = (uint8_t)s[i + 2];
        if (c2 == 0x80) {
          if (c3 == 0x99 || c3 == 0x98) { // ' or '
            res += '\'';
          } else if (c3 == 0x9C || c3 == 0x9D) { // " or "
            res += '"';
          } else if (c3 == 0xA6) { // …
            res += "...";
          } else {
            res += '?'; // Unknown
          }
        }
        i += 2;
      }
    }
    // Skip other multi-byte sequences we don't handle
  }
  return res;
}

bool isDisplaySpace(char c) { return c == ' ' || c == '\t'; }

int wrapDisplayText(const String &text, int textSize, String lines[],
                    int maxLines, bool &truncated) {
  truncated = false;
  int lineCount = 0;
  int charWidth = 6 * textSize;
  int maxChars = max(1, display.width() / charWidth);
  int pos = 0;

  while (pos <= (int)text.length() && lineCount < maxLines) {
    int paragraphEnd = text.indexOf('\n', pos);
    bool hasNextParagraph = paragraphEnd >= 0;
    if (!hasNextParagraph)
      paragraphEnd = text.length();

    String paragraph = text.substring(pos, paragraphEnd);
    paragraph.trim();

    while (paragraph.length() > 0 && lineCount < maxLines) {
      if ((int)paragraph.length() <= maxChars) {
        lines[lineCount++] = paragraph;
        paragraph = "";
      } else {
        int breakAt = maxChars;
        for (int i = maxChars; i > 0; i--) {
          if (isDisplaySpace(paragraph[i])) {
            breakAt = i;
            break;
          }
        }

        String line = paragraph.substring(0, breakAt);
        line.trim();
        lines[lineCount++] = line;

        paragraph = paragraph.substring(breakAt);
        paragraph.trim();
      }
    }

    if (paragraph.length() > 0)
      truncated = true;

    if (lineCount < maxLines && paragraphEnd == pos)
      lines[lineCount++] = "";

    if (!hasNextParagraph)
      break;

    pos = paragraphEnd + 1;
  }

  if (pos < (int)text.length() && lineCount >= maxLines)
    truncated = true;

  return lineCount;
}

int chooseReadableTextSize(const String &text, int maxLines) {
  bool smallScreen = (display.width() < 400 || display.height() < 300);
  const int *sizes;
  int sizeCount;
  static const int smallSizes[] = {3, 2, 1};
  static const int largeSizes[] = {6, 5, 4, 3, 2};

  if (smallScreen) {
    sizes = smallSizes;
    sizeCount = sizeof(smallSizes) / sizeof(smallSizes[0]);
  } else {
    sizes = largeSizes;
    sizeCount = sizeof(largeSizes) / sizeof(largeSizes[0]);
  }

  String trialLines[12];
  for (int i = 0; i < sizeCount; i++) {
    bool truncated = false;
    int lines = wrapDisplayText(text, sizes[i], trialLines, maxLines, truncated);
    int lineHeight = 8 * sizes[i] + max(4, sizes[i]);
    int totalHeight = lines * lineHeight;
    if (!truncated && totalHeight <= (display.height() * 9) / 10)
      return sizes[i];
  }

  return sizes[sizeCount - 1];
}

void drawCenteredBuiltInLine(const String &line, int y, int textSize) {
  int lineWidth = line.length() * 6 * textSize;
  int x = max(0, (display.width() - lineWidth) / 2);
  display.setTextSize(textSize);
  display.setCursor(x, y);
  display.print(line);
}

void displayText(String text, int rotation = 0, bool blackBackground = false);

#if defined(WIGNET_BENCH_AUDIO)
// USB bench "ringdraw": a full refresh of the panel while the sound plays.
// Held like a bench image, so the platform does not redraw over it.
void benchDraw(int index, int count) {
  displayText(String("ringdraw ") + index + "/" + count, 0, index % 2 == 0);
  benchHold = true;
  benchHoldUntilMs = millis() + UsbConsole::BENCH_HOLD_MINUTES * 60000UL;
}
#endif

void displayText(String text, int rotation, bool blackBackground) {
  text = utf8ToDisplayText(text);
  Serial.println("Displaying text: " + text);

  display.setRotation(rotation);
  display.setFullWindow();

  uint16_t bgColor = blackBackground ? GxEPD_BLACK : GxEPD_WHITE;
  uint16_t fgColor = blackBackground ? GxEPD_WHITE : GxEPD_BLACK;

  display.setFont(nullptr);
  display.cp437(true);
  display.setTextColor(fgColor);

  text.replace("\r\n", "\n");
  const int maxLines = 12;
  String lines[maxLines];
  int textSize = chooseReadableTextSize(text, maxLines);
  bool truncated = false;
  int lineCount = wrapDisplayText(text, textSize, lines, maxLines, truncated);
  int lineHeight = 8 * textSize + max(4, textSize);
  int totalHeight = lineCount * lineHeight;

  display.setTextSize(textSize);
  display.firstPage();
  do {
    display.fillScreen(bgColor);

    int startY = (display.height() - totalHeight) / 2;
    for (int i = 0; i < lineCount; i++) {
      drawCenteredBuiltInLine(lines[i], startY + (i * lineHeight), textSize);
    }

    if (truncated) {
      display.setCursor(display.width() - (4 * 6 * textSize), display.height() -
                                                               (8 * textSize));
      display.print("...");
    }
  } while (display.nextPage());

  Serial.println("Text display complete");
}

#if defined(WIGNET_DISPLAY_RGB)
// ============ Colour LCD screens (docs/lcd_154.md, docs/lcd_2.md) ============
// Drawn straight into the LCD's frame, in colour: left-aligned, light text
// (#F7F2E8) on black, FreeSans for the text and Org_01 in #D56668 for the
// small print. The 1-bit GxEPD2 pages could not draw these colours.
//
// The frame is the panel's landscape buffer (getWidth x getHeight). A page is
// drawn upright and turned into it by `turn`, as the server turns its pages:
// a square board turns the whole frame itself (setTurn) and draws at 0; a
// board that reports how it is held (orientation_report1) draws its own
// screens at that turn, in portrait or landscape.

static int lcdPageWidth(int turn) {
  return (turn == 90 || turn == 270) ? DisplayHAL::getHeight() : DisplayHAL::getWidth();
}
static int lcdPageHeight(int turn) {
  return (turn == 90 || turn == 270) ? DisplayHAL::getWidth() : DisplayHAL::getHeight();
}
// Where page pixel (x, y) lands in the frame: a clockwise turn of the page.
static inline int lcdFrameIndex(int x, int y, int turn) {
  const int pw = lcdPageWidth(turn), ph = lcdPageHeight(turn);
  int fx = x, fy = y;
  switch (turn) {
  case 90:  fx = ph - 1 - y; fy = x;          break;
  case 180: fx = pw - 1 - x; fy = ph - 1 - y; break;
  case 270: fx = y;          fy = pw - 1 - x; break;
  default: break;
  }
  return fy * DisplayHAL::getWidth() + fx;
}

// The turn the board's own screens are drawn at (see above).
static int lcdLocalTurn() {
#if defined(WIGNET_ORIENTATION_REPORT)
  return lcdHeldTurn;
#else
  return 0;
#endif
}

class LcdFrame : public Adafruit_GFX {
public:
  explicit LcdFrame(int turn = lcdLocalTurn())
      : Adafruit_GFX(lcdPageWidth(turn), lcdPageHeight(turn)), turn_(turn) {}
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    uint16_t *frame = DisplayHAL::frame();
    if (!frame || x < 0 || y < 0 || x >= width() || y >= height()) return;
    // The frame is big-endian, as the panel takes it.
    frame[lcdFrameIndex(x, y, turn_)] = static_cast<uint16_t>((color << 8) | (color >> 8));
  }

private:
  int turn_;
};

// RGB565 of the brand colours.
static constexpr uint16_t LCD_TEXT = 0xF79D;       // #F7F2E8
static constexpr uint16_t LCD_BACKGROUND = 0x0000; // black
static constexpr uint32_t LCD_BACKGROUND_RGB = 0x000000;
static constexpr uint16_t LCD_SMALL = 0xD32D;      // #D56668
static constexpr int LCD_MARGIN = 16;

// One line at baseline y.
static void lcdLine(LcdFrame &gfx, const GFXfont *font, uint16_t color, int y,
                    const String &text) {
  gfx.setFont(font);
  gfx.setTextSize(1);
  gfx.setTextColor(color);
  gfx.setCursor(LCD_MARGIN, y);
  gfx.print(text);
}

static void lcdSmall(LcdFrame &gfx, int y, const String &text) {
  lcdLine(gfx, &Org_01, LCD_SMALL, y, text);
}

static String lcdApName, lcdMac, lcdIp;

// The power-on screen: the boot picture (assets/boot_lcd154.png, embedded
// by embed_boot_image.py) centred on the background, its transparency
// blended onto it.
static int bootX = 0, bootY = 0, bootTurn = 0;
static int bootDrawLine(PNGDRAW *draw) {
  uint16_t line[320]; // the widest frame (the 2" board's 320)
  if (draw->iWidth > lcdPageWidth(bootTurn) || draw->iWidth > 320) return 0;
  png.getLineAsRGB565(draw, line, PNG_RGB565_BIG_ENDIAN, LCD_BACKGROUND_RGB);
  uint16_t *frame = DisplayHAL::frame();
  for (int x = 0; x < draw->iWidth; ++x)
    frame[lcdFrameIndex(bootX + x, bootY + draw->y, bootTurn)] = line[x];
  return 1;
}

static void lcdBootScreen() {
  lcdScreen = LcdScreen::Boot;
  bootTurn = lcdLocalTurn();
  DisplayHAL::setBackground(LCD_BACKGROUND);
  if (DisplayHAL::frame() &&
      png.openRAM(const_cast<uint8_t *>(BOOT_IMAGE_PNG), sizeof(BOOT_IMAGE_PNG),
                  bootDrawLine) == PNG_SUCCESS) {
    bootX = (lcdPageWidth(bootTurn) - png.getWidth()) / 2;
    bootY = (lcdPageHeight(bootTurn) - png.getHeight()) / 2;
    if (bootX >= 0 && bootY >= 0) png.decode(nullptr, 0);
    png.close();
  }
  DisplayHAL::showFrame();
}

// Laid out for 240 px of height: a taller page (portrait) puts the header
// and footer at its edges and centres the rest.
static void lcdProvisioningScreen(const String &apName) {
  lcdScreen = LcdScreen::Provisioning;
  lcdApName = apName;
  LcdFrame gfx;
  const int middle = (gfx.height() - 240) / 2;
  gfx.fillScreen(LCD_BACKGROUND);
  lcdSmall(gfx, 28, "WIGNET  WI-FI SETUP");
  lcdLine(gfx, &FreeSans12pt7b, LCD_TEXT, middle + 74, "Connect to the");
  lcdLine(gfx, &FreeSans12pt7b, LCD_TEXT, middle + 100, "Wi-Fi network");
  lcdLine(gfx, &FreeSansBold12pt7b, LCD_TEXT, middle + 140, apName);
  lcdLine(gfx, &FreeSans12pt7b, LCD_TEXT, middle + 178, "to set up");
  lcdLine(gfx, &FreeSans12pt7b, LCD_TEXT, middle + 204, "this Wignet");
  lcdSmall(gfx, gfx.height() - 12, "MAC " + WiFi.macAddress());
  DisplayHAL::showFrame();
}

static void lcdStartupScreen(const String &mac, const String &ip) {
  lcdScreen = LcdScreen::Startup;
  lcdMac = mac;
  lcdIp = ip;
  LcdFrame gfx;
  const int middle = (gfx.height() - 240) / 2;
  gfx.fillScreen(LCD_BACKGROUND);
  lcdSmall(gfx, 28, "WIGNET  CONNECTED");
  lcdLine(gfx, &FreeSansBold12pt7b, LCD_TEXT, middle + 80, "Wi-Fi OK");
  lcdSmall(gfx, middle + 116, "MAC");
  lcdLine(gfx, &FreeSans12pt7b, LCD_TEXT, middle + 140, mac);
  lcdSmall(gfx, middle + 168, "IP");
  lcdLine(gfx, &FreeSans12pt7b, LCD_TEXT, middle + 192, ip);
  // The platform's first page replaces this screen within seconds.
  lcdSmall(gfx, gfx.height() - 12, "WAITING FOR THE SERVER");
  DisplayHAL::showFrame();
}

// The Welcome page (welcome_local.py on the server): the owner page's
// address and its QR code, drawn like the board's other screens. The code
// is dark modules on a light tile with a quiet zone of two modules, each
// module a whole number of pixels, as large as the page allows.
static String lcdWelcomeAddress, lcdWelcomeQr;
static int lcdWelcomeSize = 0;
static constexpr int LCD_QR_QUIET = 2;

static bool lcdQrDark(int x, int y) {
  const int bit = y * lcdWelcomeSize + x;
  const int at = (bit / 8) * 2;
  if (at + 2 > static_cast<int>(lcdWelcomeQr.length())) return false;
  const int byte = strtol(lcdWelcomeQr.substring(at, at + 2).c_str(), nullptr, 16);
  return byte & (0x80 >> (bit % 8));
}

static void lcdWelcomeScreen() {
  lcdScreen = LcdScreen::Welcome;
  LcdFrame gfx;
  gfx.fillScreen(LCD_BACKGROUND);
  lcdSmall(gfx, 28, "WIGNET  WELCOME");
  // Between the header and the two lines at the bottom.
  const int top = 40, bottom = gfx.height() - 58;
  const int span = lcdWelcomeSize + 2 * LCD_QR_QUIET;
  const int module = lcdWelcomeSize ? max(1, min((gfx.width() - 2 * LCD_MARGIN) / span, (bottom - top) / span)) : 0;
  if (module) {
    const int side = span * module;
    const int y0 = top + (bottom - top - side) / 2;
    gfx.fillRect(LCD_MARGIN, y0, side, side, LCD_TEXT);
    for (int y = 0; y < lcdWelcomeSize; ++y)
      for (int x = 0; x < lcdWelcomeSize; ++x)
        if (lcdQrDark(x, y))
          gfx.fillRect(LCD_MARGIN + (LCD_QR_QUIET + x) * module, y0 + (LCD_QR_QUIET + y) * module, module,
                       module, LCD_BACKGROUND);
  }
  lcdLine(gfx, &FreeSans12pt7b, LCD_TEXT, gfx.height() - 30, "Scan to set it up");
  lcdSmall(gfx, gfx.height() - 12, lcdWelcomeAddress);
  DisplayHAL::showFrame();
}

// A string value of `key` inside `object`, or "".
static String lcdJsonString(const String &object, const char *key) {
  const int at = object.indexOf(String("\"") + key + "\"");
  if (at < 0) return "";
  const int open = object.indexOf('"', object.indexOf(':', at) + 1);
  const int close = object.indexOf('"', open + 1);
  return open >= 0 && close > open ? object.substring(open + 1, close) : "";
}

// /schedule `welcome`: drawn here while present, the platform page back when null.
static void lcdApplyWelcome(const String &payload) {
  const int key = payload.indexOf("\"welcome\"");
  const int colon = key < 0 ? -1 : payload.indexOf(':', key);
  const int brace = colon < 0 ? -1 : payload.indexOf('{', colon);
  const int null = colon < 0 ? -1 : payload.indexOf("null", colon);
  const bool present = brace >= 0 && (null < 0 || brace < null);
  if (!present) {
    if (lcdWelcomeWanted) {
      Serial.println("[Welcome] Done: the platform page again");
      lcdWelcomeWanted = false;
      lcdScreen = LcdScreen::None;
      etagValidThisBoot = false; // fetch the page whole, not a 304 on an old one
      memset(rtcLastEtag, 0, sizeof(rtcLastEtag));
      lastWignetPoll = 0;
    }
    return;
  }
  const String object = payload.substring(brace, payload.indexOf('}', brace) + 1);
  const String address = lcdJsonString(object, "address");
  const String qr = lcdJsonString(object, "qr");
  const int sizeKey = object.indexOf("\"qrSize\"");
  const int size = sizeKey < 0 ? 0 : object.substring(object.indexOf(':', sizeKey) + 1).toInt();
  if (size <= 0 || size > 177 || static_cast<int>(qr.length()) < (size * size + 7) / 8 * 2) {
    Serial.println("[Welcome] Unreadable QR: showing the platform page");
    return;
  }
  const bool changed = !lcdWelcomeWanted || address != lcdWelcomeAddress || qr != lcdWelcomeQr ||
                       lcdScreen != LcdScreen::Welcome;
  lcdWelcomeWanted = true;
  lcdWelcomeAddress = address;
  lcdWelcomeQr = qr;
  lcdWelcomeSize = size;
  if (changed) {
    Serial.printf("[Welcome] Drawn here: %s, %dx%d QR\n", address.c_str(), size, size);
    lcdWelcomeScreen();
  }
}

#if defined(WIGNET_ORIENTATION_REPORT)
// The platform page on screen, shrunk to fit the way the board is now held
// (black bars), while the server lays it out again: never sideways.
static void lcdShowInterim(int from, int to) {
  uint16_t *frame = DisplayHAL::frame();
  const size_t bytes = DisplayHAL::getWidth() * DisplayHAL::getHeight() * sizeof(uint16_t);
  uint16_t *copy = frame ? static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM)) : nullptr;
  if (!copy) return;
  memcpy(copy, frame, bytes);
  const int fromW = lcdPageWidth(from), fromH = lcdPageHeight(from);
  const int toW = lcdPageWidth(to), toH = lcdPageHeight(to);
  const float scale = min(static_cast<float>(toW) / fromW, static_cast<float>(toH) / fromH);
  const int shownW = static_cast<int>(fromW * scale), shownH = static_cast<int>(fromH * scale);
  const int left = (toW - shownW) / 2, top = (toH - shownH) / 2;
  for (int y = 0; y < toH; ++y) {
    for (int x = 0; x < toW; ++x) {
      uint16_t pixel = 0x0000;
      if (x >= left && x < left + shownW && y >= top && y < top + shownH) {
        const int sx = min(fromW - 1, static_cast<int>((x - left) / scale));
        const int sy = min(fromH - 1, static_cast<int>((y - top) / scale));
        pixel = copy[lcdFrameIndex(sx, sy, from)];
      }
      frame[lcdFrameIndex(x, y, to)] = pixel;
    }
  }
  free(copy);
  DisplayHAL::showFrame();
}
#endif

// From the orientation task, on a steady new turn.
static void lcdOnTurn(int turn) {
#if defined(WIGNET_ORIENTATION_REPORT)
  lcdHeldTurn = turn;
#if defined(WIGNET_SCENE_PLAYER)
  ScenePlayer::onTurn(turn); // scene_turn1: the scene moves to the page held now
#endif
  switch (lcdScreen) {
  case LcdScreen::Boot: lcdBootScreen(); break;
  case LcdScreen::Provisioning: lcdProvisioningScreen(lcdApName); break;
  case LcdScreen::Startup: lcdStartupScreen(lcdMac, lcdIp); break;
  case LcdScreen::Welcome: lcdWelcomeScreen(); break;
  case LcdScreen::None: break; // a platform page: the loop reports it
  }
  lcdTurnToReport = true;
#else
  DisplayHAL::setTurn(turn);
#if defined(WIGNET_SCENE_PLAYER)
  ScenePlayer::onTurn(turn); // the scene, drawn again the new way up
#endif
#endif
}
#endif

void displayProvisioningScreen(String apName) {
  Serial.println("Displaying Provisioning Screen");
#if defined(WIGNET_DISPLAY_RGB)
  lcdProvisioningScreen(apName);
#else

  // The LCD's layout (lcdProvisioningScreen, drawn for 240 px of height)
  // scaled to the panel: white on black, left-aligned, a small header and
  // the MAC in small print at the edges (HARDWARE.md "Start screens").
  display.setRotation(getDefaultDisplayRotation());
  display.setFullWindow();
  display.firstPage();

  const int w = display.width(), h = display.height();
  const int u = max(2, min(w, h) / 64);
  const int margin = 4 * u;
  const int textSize = max(1, u / 3);     // Org_01 is 5 px tall
  // 24 pt on 480 px, 12 pt on 240 px (the LCD's), 9 pt below.
  const GFXfont *regular = &FreeSans9pt7b, *bold = &FreeSansBold9pt7b;
  int k4 = 3;                             // scale from 240 px, in quarters
  if (h >= 480) regular = &FreeSans24pt7b, bold = &FreeSansBold24pt7b, k4 = 8;
  else if (h >= 240) regular = &FreeSans12pt7b, bold = &FreeSansBold12pt7b, k4 = 4;
  const int middle = (h - 240 * k4 / 4) / 2;
  struct Line { const GFXfont *font; int y; const char *text; };
  const Line narrow[] = {{regular, 74, "Connect to the"},
                         {regular, 100, "Wi-Fi network"},
                         {bold, 140, apName.c_str()},
                         {regular, 178, "to set up"},
                         {regular, 204, "this Wignet"}};
  // A wide panel keeps each sentence on one line.
  const Line wide[] = {{regular, 100, "Connect to the Wi-Fi network"},
                       {bold, 140, apName.c_str()},
                       {regular, 178, "to set up this Wignet"}};
  int16_t bx, by;
  uint16_t bw, bh;
  display.setFont(regular);
  display.setTextSize(1);
  display.getTextBounds(wide[0].text, 0, 0, &bx, &by, &bw, &bh);
  const bool fits = bx + bw <= w - 2 * margin;
  const Line *lines = fits ? wide : narrow;
  const int lineCount = fits ? 3 : 5;

  do {
    display.fillScreen(GxEPD_BLACK);
    display.setTextColor(GxEPD_WHITE);
    display.setTextSize(1);
    for (int i = 0; i < lineCount; i++) {
      display.setFont(lines[i].font);
      display.setCursor(margin, middle + lines[i].y * k4 / 4);
      display.print(lines[i].text);
    }
    display.setFont(&Org_01);
    display.setTextSize(textSize);
    display.setTextColor(EPAPER_SMALL);
    display.setCursor(margin, margin + 5 * textSize);
    display.print("WIGNET  WI-FI SETUP");
    display.setCursor(margin, h - margin);
    display.print("MAC " + WiFi.macAddress());
    display.setFont(nullptr);
  } while (display.nextPage());
#endif
}

void displayStartupScreen(String mac, String ip) {
  Serial.println("Displaying Startup Screen");
#if defined(WIGNET_DISPLAY_RGB)
  lcdStartupScreen(mac, ip);
#else

  // The Wignet mark centred, white on black, and the MAC and IP in small
  // print at the bottom left. The platform's first page replaces it within
  // seconds. The mark (HARDWARE.md "The Wignet mark"): two squares of side S
  // side by side; in the left one the bottom quarter cut by both diagonals,
  // in the right one the bottom-left half below its top-left/bottom-right
  // diagonal. Drawn row by row so each square is exactly S x S pixels.
  display.setRotation(getDefaultDisplayRotation());
  display.setFullWindow();
  display.firstPage();

  const int w = display.width(), h = display.height();
  const int u = max(2, min(w, h) / 64);   // 7 px on 800x480
  const int S = 16 * u;                   // 112 px: a 224x112 mark
  const int x0 = (w - 2 * S) / 2, y0 = (h - S) / 2;
  const int margin = 4 * u;
  const int textSize = max(1, u / 3);     // Org_01 is 5 px tall
  const int lineHeight = 10 * textSize;
  const String lines[] = {"MAC " + mac, "IP " + ip};

  do {
    display.fillScreen(GxEPD_BLACK);
    for (int r = 0; r < S; r++) {
      const int y = y0 + r;
      // Left square: rows of the bottom quarter, centred, widening to S.
      if (2 * r >= S - 1)
        display.drawFastHLine(x0 + S - 1 - r, y, 2 * r - S + 2, GxEPD_WHITE);
      // Right square: from its left edge to the diagonal, r + 1 wide.
      display.drawFastHLine(x0 + S, y, r + 1, GxEPD_WHITE);
    }

    display.setFont(&Org_01);
    display.setTextSize(textSize);
    display.setTextColor(EPAPER_SMALL);
    for (int i = 0; i < 2; i++) {
      display.setCursor(margin, h - margin - (1 - i) * lineHeight);
      display.print(lines[i]);
    }
    display.setFont(nullptr);
  } while (display.nextPage());
#endif
}

void clearStoredWiFiOnceForBuild() {
#if defined(ESP8266)
  Serial.println("[WiFi] One-shot WiFi reset build flag is ESP32-only");
#else
  static const char *WIFI_RESET_NAMESPACE = "wignet";
  static const char *WIFI_RESET_KEY = "wifiResetBuild";
  static const char *BUILD_ID = __DATE__ " " __TIME__;

  Preferences prefs;
  if (!prefs.begin(WIFI_RESET_NAMESPACE, false)) {
    Serial.println("[WiFi] Could not open Preferences for one-shot reset");
    return;
  }

  String previousBuild = prefs.getString(WIFI_RESET_KEY, "");
  if (previousBuild == BUILD_ID) {
    Serial.println("[WiFi] One-shot WiFi reset already applied for this build");
    prefs.end();
    return;
  }

  Serial.printf("[WiFi] Clearing stored credentials once for build %s\n",
                BUILD_ID);
  resetWiFiCredentials();
  prefs.putString(WIFI_RESET_KEY, BUILD_ID);
  prefs.end();
  delay(100);
#endif
}

// ============ HTTP Handlers ============

void handleRoot() {
  Serial.println("[Web] Request: /");
  int defaultRotation = getDefaultDisplayRotation();

  String html = R"(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Wignet E-Paper Client</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; padding: 20px; max-width: 600px; margin: 0 auto; background: #1a1a2e; color: #eee; }
    input, select, button, textarea { width: 100%; padding: 12px; margin: 8px 0; font-size: 16px; border-radius: 8px; border: none; box-sizing: border-box; }
    textarea { height: 120px; font-family: inherit; }
    button { background: #4CAF50; color: white; cursor: pointer; font-weight: bold; }
    button:hover { background: #45a049; }
    .info { background: #16213e; padding: 15px; border-radius: 8px; margin: 15px 0; }
    .status { color: #0f0; }
    h1, h2 { color: #e94560; }
    a { color: #4CAF50; }
  </style>
</head>
<body>
  <h1>🖥️ Wignet E-Paper Client</h1>
  <div class="info">
    <strong>MAC:</strong> )" +
                WiFi.macAddress() + R"(<br>
    <strong>IP:</strong> )" +
                WiFi.localIP().toString() + R"(<br>
    <strong>RSSI:</strong> )" +
                String(WiFi.RSSI()) + R"( dBm<br>
    <strong>Wignet Status:</strong> <span class="status">)" +
                lastFetchStatus + R"(</span><br>
    <strong>Polling:</strong> )" +
                String(wignetEnabled ? "Enabled" : "Disabled") + R"( (every )" +
                String(WIGNET_POLL_INTERVAL_SEC) + R"(s)
  </div>

  <h2>Manual Text Display</h2>
  <form action="/display" method="POST">
    <textarea name="text" placeholder="Entrez votre texte ici (accents supportés)..." required></textarea>
    <select name="orientation">
      <option value="0" )" +
                String(defaultRotation == 0 ? "selected" : "") +
                R"(>Landscape (0°)</option>
      <option value="1" )" +
                String(defaultRotation == 1 ? "selected" : "") +
                R"(>Portrait (90°)</option>
      <option value="2" )" +
                String(defaultRotation == 2 ? "selected" : "") +
                R"(>Landscape inverted (180°)</option>
      <option value="3" )" +
                String(defaultRotation == 3 ? "selected" : "") +
                R"(>Portrait inverted (270°)</option>
    </select>
    <select name="background">
      <option value="black" selected>Black background</option>
      <option value="white">White background</option>
    </select>
    <button type="submit">Display</button>
  </form>

  <h2>Wignet Control</h2>
  <form action="/wignet/fetch" method="POST" style="display:inline">
    <button type="submit">Fetch Now</button>
  </form>
  <form action="/wignet/toggle" method="POST" style="display:inline">
    <button type="submit" style="background:#e94560">Toggle Polling</button>
  </form>

  <p><a href="/status">JSON Status</a></p>
</body>
</html>
  )";

  server.send(200, "text/html", html);
}

void handleDisplay() {
  String text = server.arg("text");
  Serial.printf("[Web] Request: /display - Text: '%s'\n", text.c_str());
  if (server.hasArg("text")) {
    String text = server.arg("text");
    int orientation =
        server.hasArg("orientation") ? server.arg("orientation").toInt() : 0;
    bool blackBg =
        server.hasArg("background") && server.arg("background") == "black";

    server.send(200, "text/plain", "OK - Displaying text");
    delay(100);
    displayText(text, orientation, blackBg);
  } else {
    server.send(400, "text/plain", "Error: 'text' parameter missing");
  }
}

void handleWignetFetch() {
  Serial.println("[Web] Request: /wignet/fetch");
  server.send(200, "text/plain", "Fetching from Wignet...");
  delay(100);
  fetchWignetImage();
}

void handleWignetToggle() {
  wignetEnabled = !wignetEnabled;
  Serial.printf("[Web] Request: /wignet/toggle - Now: %s\n",
                wignetEnabled ? "ENABLED" : "DISABLED");
  server.send(200, "text/plain",
              String("Wignet polling: ") +
                  (wignetEnabled ? "enabled" : "disabled"));
}

void handleStatus() {
  String json = "{";
  json += "\"mac\":\"" + WiFi.macAddress() + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"wignetEnabled\":" + String(wignetEnabled ? "true" : "false") + ",";
  json += "\"lastFetchStatus\":\"" + lastFetchStatus + "\",";
  json += "\"pollInterval\":" + String(WIGNET_POLL_INTERVAL_SEC) + ",";
  json += "\"uptime\":" + String(millis() / 1000);
  json += "}";

  server.send(200, "application/json", json);
}

void handleWiFiReset() {
  Serial.println("[Web] Request: /wifi/reset");
  server.send(200, "text/html",
              "<html><body "
              "style='font-family:sans-serif;text-align:center;padding:50px;'>"
              "<h1>WiFi Reset</h1>"
              "<p>Les credentials WiFi ont été effacés.</p>"
              "<p>Le device va redémarrer en mode configuration...</p>"
              "</body></html>");
  delay(2000);
  resetWiFiCredentials();
  ESP.restart();
}

// ============ WiFi Connection ============
// NOTE: WiFi provisioning is now handled by wifi_provisioning module
// The connectWiFi() function is replaced by setupWiFiProvisioning()

// ============ Setup & Loop ============

void setup() {
  // On battery the board stays on only once the firmware holds it: before
  // anything else, while PWR may still be held down (power_hold_hal.cpp).
  PowerHoldHAL::begin();
  // Record boot time for deep sleep safety period
  bootTime = millis();

  // 1. Lower CPU frequency (240MHz -> 160MHz or 80MHz reduces spikes)
#if defined(ESP32)
  setCpuFrequencyMhz(80);
#endif

#if defined(WIGNET_BENCH_CONSOLE)
  UsbConsole::prepareSerial(); // before begin(): sizes the receive buffer
  UsbConsole::setImageHandler(benchImage);
  UsbConsole::setResumeHandler(benchResume);
#endif
#if defined(WIGNET_BENCH_AUDIO)
  UsbConsole::setSleepHandler(benchSleep);
  UsbConsole::setDrawHandler(benchDraw);
#endif
  Serial.begin(115200);
  Serial.println("\n[Boot] Firmware " WIGNET_FW_VERSION);
#if defined(WIGNET_OTA_CLIENT)
  // The OTA deadline starts here, before Wi-Fi (docs/ota.md "Confirmation"),
  // after Serial.begin(): on a UART console (the 7.5 V2) what it says at
  // boot would be lost before.
  OtaClient::begin();
#endif
  StatusLED::begin();

#if defined(WIGNET_HAS_AUDIO)
  AlarmClockHAL::begin();
#endif
#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
  UsbAwakeHAL::begin();
  PowerHAL::begin(); // after AlarmClockHAL::begin() has started I2C
#elif defined(WIGNET_BATTERY_ADC_PIN)
  PowerHAL::begin(); // the battery ADC and charge pin
#endif
#if defined(WIGNET_HAS_AUDIO)
  AlarmClockHAL::ringIfDue(); // Cached occurrence can ring without Wi-Fi.
#endif

#if defined(ESP8266)
  String resetReason = ESP.getResetReason();
  bool isWakeup = resetReason.indexOf("Deep-Sleep") >= 0;
  Serial.printf("\n[Boot] Reset reason: %s\n", resetReason.c_str());
#else
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  bool isWakeup = (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER
#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
                   || wakeup_reason == ESP_SLEEP_WAKEUP_EXT0
                   || wakeup_reason == ESP_SLEEP_WAKEUP_EXT1
#endif
  );

  Serial.printf("\n[Boot] Wakeup cause: %d\n", wakeup_reason);
#endif
  wokeFromDeepSleep = isWakeup;
  Serial.printf("[Boot] Deep sleep safety period: %lu ms\n", SAFE_PERIOD_MS);
  delay(100); // Small delay to let things stabilize

  Serial.println("\n\n=== Wignet E-Paper Client ===");
  Serial.printf("Display: %dx%d\n", DisplayHAL::getWidth(),
                DisplayHAL::getHeight());

  // 2. Initialize display via HAL
#if defined(WIGNET_HAS_IMU)
  OrientationHAL::begin(); // before the first draw, so it is already turned
#endif
  DisplayHAL::init();
#if defined(WIGNET_HAS_IMU)
  lcdHeldTurn = OrientationHAL::turn();
  DisplayHAL::setTurn(OrientationHAL::turn()); // a square board turns itself
#endif
#if defined(WIGNET_DISPLAY_RGB)
  lcdBootScreen(); // the first thing the backlight shows
#endif
#if defined(WIGNET_LVGL_SPIKE)
  LvglSpike::start(); // takes the panel; the loop below runs as usual
#endif
#if defined(WIGNET_SCENE_PLAYER)
  ScenePlayer::begin(); // the panel stays the loop's until a scene plays
#endif
#if defined(WIGNET_HAS_IMU)
  // From here on the screen follows the board, Wi-Fi portal included; the
  // next /schedule reports the turn (ori=).
  OrientationHAL::startTask(lcdOnTurn);
#endif
#if defined(WIGNET_POWER_HOLD_PIN)
  PowerHoldHAL::startTask(); // long press on PWR: off
#endif
  Serial.println("E-Paper initialized via HAL");

#if defined(WIGNET_CLEAR_WIFI_ON_FIRST_BOOT)
  clearStoredWiFiOnceForBuild();
#endif

  // 3. Setup WiFi via provisioning (captive portal if needed)
  Serial.println("Starting WiFi provisioning...");
#if defined(WIGNET_DISPLAY_RGB) && defined(WIGNET_BENCH_CONSOLE)
  // The Wi-Fi portal blocks setup() until a network is saved, and loop()
  // polls the bench console: poll it from a task meanwhile, so a test card
  // can be sent before Wi-Fi exists. Stopped before loop() takes over.
  static volatile bool provisioning = true;
  static TaskHandle_t benchTask = nullptr;
  xTaskCreatePinnedToCore([](void *) {
    while (provisioning) {
      UsbConsole::poll();
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    benchTask = nullptr;
    vTaskDelete(nullptr);
  }, "bench", 8192, nullptr, 1, &benchTask, 1);
#endif
  bool wifiConnected = setupWiFiProvisioning(displayProvisioningScreen);
#if defined(WIGNET_DISPLAY_RGB) && defined(WIGNET_BENCH_CONSOLE)
  provisioning = false;
  while (benchTask) delay(5); // never two readers of the console
#endif

  if (wifiConnected) {
#if defined(WIGNET_HAS_AUDIO)
    AlarmClockHAL::syncFromNtp();
    AlarmClockHAL::ringIfDue();
#endif
    // Setup HTTP server routes
    server.on("/", handleRoot);
    server.on("/display", HTTP_POST, handleDisplay);
    server.on("/wignet/fetch", HTTP_POST, handleWignetFetch);
    server.on("/wignet/toggle", HTTP_POST, handleWignetToggle);
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/wifi/reset", HTTP_GET, handleWiFiReset);
    server.on("/wifi/reset", HTTP_POST, handleWiFiReset);

    server.begin();
    Serial.printf("[Server] HTTP server listening on port %d\n", HTTP_PORT);
    Serial.println("[Server] Ready to receive commands");

    if (isWakeup) {
      Serial.println(
          "[Wignet] Deep sleep wakeup detected - skipping startup screen");
      // For wakeup, we allow deep sleep immediately and schedule fetch NOW
      deepSleepAllowed = true;
      lastWignetPoll = millis() - (WIGNET_POLL_INTERVAL_SEC * 1000UL);
    } else {
      // Announce ourselves before the ~5 s startup refresh: /schedule creates
      // a new device and starts its first render, so the image is ready by the
      // first fetch below instead of the "not assigned" placeholder. Only the
      // side effect matters; the next wake-up comes from that first fetch.
      fetchSchedule();

      // Show startup info on display for fresh boot
      displayStartupScreen(WiFi.macAddress(), WiFi.localIP().toString());

      // Schedule first fetch using the shorter startup delay, then continue
      // with the normal polling interval.
      const int firstPollDelaySec =
          min(WIGNET_INITIAL_POLL_DELAY_SEC, WIGNET_POLL_INTERVAL_SEC);
      Serial.printf("[Wignet] First fetch scheduled in %d seconds\n",
                    firstPollDelaySec);
      lastWignetPoll =
          millis() - ((WIGNET_POLL_INTERVAL_SEC - firstPollDelaySec) * 1000UL);
    }
  } else {
    // WiFi not connected - show provisioning screen
    if (!isWakeup) {
      String apName = getProvisioningAPName();
      displayProvisioningScreen(apName);
      Serial.println("[Provisioning] Waiting for WiFi configuration...");
      Serial.printf("[Provisioning] Connect to '%s' to configure\n",
                    apName.c_str());
    }
  }
}

// Wi-Fi lost while running (loop()): when, and the last try of another known network.
static const unsigned long WIFI_SWITCH_AFTER_MS = 60000;
static unsigned long wifiLostSinceMs = 0;
static unsigned long wifiLastSwitchTryMs = 0;

void loop() {
  if (WiFi.status() == WL_CONNECTED) wifiLostSinceMs = 0;
  UsbConsole::poll(); // bench commands over USB (play_mml.py, show_image.py)
#if defined(WIGNET_OTA_CLIENT)
  OtaClient::tick(); // the 10-minute deadline of a pending image
#endif
#if defined(WIGNET_ORIENTATION_REPORT)
  if (lcdTurnToReport) {
    lcdTurnToReport = false;
    if (lcdScreen == LcdScreen::None && lcdLayoutTurn != lcdHeldTurn) {
      // A platform page: shown shrunk the new way at once, laid out again by
      // the platform, which hears of the turn now rather than at the next sync.
      lcdShowInterim(lcdLayoutTurn, lcdHeldTurn);
      lcdLayoutTurn = lcdHeldTurn;
      // The cached page is the old layout: fetch it whole next time.
      etagValidThisBoot = false;
      memset(rtcLastEtag, 0, sizeof(rtcLastEtag));
      lcdRelayoutPending = true;
      lcdRelayoutSinceMs = millis();
      if (WiFi.status() == WL_CONNECTED) fetchSchedule();
      lastWignetPoll = millis();
    }
  }
  if (lcdRelayoutPending && millis() - lcdRelayoutSinceMs > LCD_RELAYOUT_GIVE_UP_MS) {
    Serial.println("[Orientation] No new layout after 30 s; back to the normal cadence");
    lcdRelayoutPending = false;
  }
#endif
#if defined(WIGNET_HAS_AUDIO)
  AlarmClockHAL::ringIfDue();
#endif
#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
  // Not while a sound plays: its task uses the same I2C bus for the codec.
  if (!AudioHAL::isPlaying() && millis() - lastPowerCheckMs >= POWER_CHECK_MS) {
    lastPowerCheckMs = millis();
    const PowerHAL::Report power = PowerHAL::read();
    if (power.valid) {
      const int external = power.external ? 1 : 0;
      if (lastExternal >= 0 && external != lastExternal) {
        Serial.printf("[Power] %s; syncing now\n", external ? "Plugged in" : "Unplugged");
        if (!external) mainsAwake = false;
        // Due at once: the next pass of the poll below reports the change.
        // The last answer was for the other power source; on mains the poll
        // follows it, so drop a long one or the sync would wait for it.
        nextWakeupSec = DEFAULT_REFRESH_SEC;
        lastWignetPoll = millis() - 3600UL * 1000UL;
      }
      lastExternal = external;
    }
  }
  if (usbAwake) {
    const unsigned long now = millis();
    if (UsbAwakeHAL::hostConnected()) {
      usbLastSeenMs = now;
    } else if (now - usbLastSeenMs >= USB_DISCONNECT_GRACE_MS) {
      usbAwake = false;
      Serial.println("[USB] Computer disconnected; resuming timed sleep");
      // Keep the server's next due time rather than starting its interval
      // again when the cable is removed.
      const unsigned long elapsedSec = (now - lastWignetPoll) / 1000UL;
      const int remaining = elapsedSec < static_cast<unsigned long>(nextWakeupSec)
                                ? nextWakeupSec - static_cast<int>(elapsedSec)
                                : 0;
      if (remaining > DEFAULT_REFRESH_SEC) {
        enterDeepSleep(remaining);
      } else {
        lastWignetPoll = now - WIGNET_POLL_INTERVAL_SEC * 1000UL;
      }
    }
  }
#endif
  // Check if safety period has elapsed
  if (!deepSleepAllowed && (millis() - bootTime > SAFE_PERIOD_MS)) {
    deepSleepAllowed = true;
    Serial.println("[DeepSleep] Safety period elapsed, deep sleep now allowed");

    // A successful fetch may already have supplied the next wake-up time
    // while the cold-boot safety period was active. Sleep now instead of
    // staying awake until the next 60-second Wignet poll.
    bool held = false;
#if defined(WIGNET_BENCH_CONSOLE)
    held = benchHolding(); // a bench image keeps the frame awake
#endif
    if (nextWakeupSec > DEFAULT_REFRESH_SEC && !held) {
      enterDeepSleep(nextWakeupSec);
    }
  }

  // Handle local HTTP server
  if (WiFi.status() == WL_CONNECTED) {
    server.handleClient();

    // Periodic Wignet polling
    unsigned long pollIntervalSec = WIGNET_POLL_INTERVAL_SEC;
#if defined(TARGET_WAVESHARE_PHOTOPAINTER_73E6)
    // Staying awake must not redraw/fetch every 60 seconds. Use the same
    // platform interval that would have driven the next deep-sleep wake.
    if (usbAwake && nextWakeupSec > WIGNET_POLL_INTERVAL_SEC)
      pollIntervalSec = nextWakeupSec;
    // On mains, the platform's answer is the owner's powered cadence, shorter
    // or longer than 60 s: follow it, never faster than MAINS_MIN_POLL_SEC.
    if (lastExternal == 1)
      pollIntervalSec = max(nextWakeupSec, MAINS_MIN_POLL_SEC);
#elif defined(WIGNET_ALWAYS_POWERED)
    // Always awake: follow the platform's powered cadence (10 s unless the
    // owner set another), never faster than every 10 s.
    pollIntervalSec = max(nextWakeupSec, 10);
#endif
    unsigned long pollIntervalMs = pollIntervalSec * 1000UL;
#if defined(WIGNET_ORIENTATION_REPORT)
    // Just turned: the new layout takes a second or two to render.
    if (lcdRelayoutPending) pollIntervalMs = LCD_RELAYOUT_POLL_MS;
#endif
#if defined(WIGNET_OTA_CLIENT)
    // A pending image polls every 30 s and never sleeps (docs/ota.md).
    if (OtaClient::pending()) pollIntervalMs = 30000UL;
#endif
    bool held = false;
#if defined(WIGNET_BENCH_CONSOLE)
    held = benchHolding();
#endif
    if (wignetEnabled && !held &&
        (millis() - lastWignetPoll >= pollIntervalMs)) {
      Serial.println("Periodic Wignet fetch...");
      bool fetchSuccess = fetchWignetImage();
      lastWignetPoll = millis();

      // After displaying image, fetch schedule for deep sleep timing
#if defined(WIGNET_HAS_AUDIO)
      // Capability reporting and alarm cancellation must reach /schedule
      // even while the first image is still being rendered. Keep the short
      // image retry on failure rather than sleeping for the schedule delay.
      const int scheduledWakeupSec = fetchSchedule();
      if (fetchSuccess) nextWakeupSec = scheduledWakeupSec;
      else nextWakeupSec = DEFAULT_REFRESH_SEC;
#else
      if (fetchSuccess) nextWakeupSec = fetchSchedule();
#endif
#if defined(WIGNET_OTA_CLIENT)
      if (OtaClient::pending() && nextWakeupSec > 30) nextWakeupSec = 30;
#endif

      if (fetchSuccess) {
        // Enter deep sleep if allowed and delay is significant
        if (deepSleepAllowed && nextWakeupSec > DEFAULT_REFRESH_SEC) {
          enterDeepSleep(nextWakeupSec);
        } else if (!deepSleepAllowed) {
          Serial.printf(
              "[DeepSleep] Still in safety period, waiting %lu more seconds\n",
              (SAFE_PERIOD_MS > (millis() - bootTime))
                  ? (SAFE_PERIOD_MS - (millis() - bootTime)) / 1000
                  : 0);
        }
      }
    }
  } else {
    // The same network first, every 10 s; after a minute without it, any
    // known network in range (docs/wifi.md), then again every minute.
    if (!wifiLostSinceMs) wifiLostSinceMs = millis();
    if (millis() % 10000 < 50) {
      Serial.println("WiFi lost, attempting reconnection...");
      WiFi.reconnect();
    }
    if (millis() - wifiLostSinceMs > WIFI_SWITCH_AFTER_MS &&
        millis() - wifiLastSwitchTryMs > WIFI_SWITCH_AFTER_MS) {
      wifiLastSwitchTryMs = millis();
      reconnectKnownWiFi();
    }
  }

#if defined(WIGNET_SCENE_PLAYER)
  if (WiFi.status() == WL_CONNECTED) ScenePlayer::service();
#endif
  delay(10);
}
