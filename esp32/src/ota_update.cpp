#include "ota_update.h"
#if defined(WIGNET_OTA_BENCH)
#include "ota_image.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_app_format.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <memory>
#include <time.h>

#if !defined(CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
#error "The OTA bench requires a rollback-enabled SDK and matching bootloader"
#endif

// Use IDF's bundled roots, not Arduino's similarly named bundle wrapper.
extern "C" esp_err_t esp_crt_bundle_attach(void *conf);
extern "C" const OtaImage::Descriptor wignet_ota_descriptor
    __attribute__((section(".rodata_custom_desc"), used)) = OtaImage::EXPECTED;
static_assert(OtaImage::DESCRIPTOR_OFFSET == sizeof(esp_image_header_t) +
    sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t), "image layout");

// Arduino otherwise marks a trial image valid before setup().
extern "C" bool verifyRollbackLater() { return true; }

namespace {
bool trial = false;
bool imageOk = false;
bool scheduleOk = false;
uint32_t holdUntil = 0;
esp_timer_handle_t deadline = nullptr;
void (*noticeHandler)() = nullptr;

bool error(const char *message) {
  Serial.printf("[bench] error OTA %s\n", message);
  return false;
}

bool layoutValid() {
  const auto *a = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
      ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  const auto *b = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
      ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  return ESP.getFlashChipSize() >= 0x400000 && a && b &&
      a->address == 0x10000 && b->address == 0x1d0000 &&
      a->size == OtaImage::SLOT_SIZE && b->size == OtaImage::SLOT_SIZE;
}

void reject(void *) {
  Serial.println("[OTA] Trial failed or exceeded 180 s; rolling back");
  Serial.flush();
  esp_ota_mark_app_invalid_rollback_and_reboot();
  // If no previous image exists (e.g. an incorrect initial installation),
  // still restart rather than leave a wedged application running.
  ESP.restart();
}

void confirm() {
#if defined(WIGNET_OTA_TEST_REJECT)
  return; // bench fault injection: exercise the real deadline/bootloader
#endif
  if (!trial || !imageOk || !scheduleOk) return;
  if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
    error("could not confirm application");
    return;
  }
  trial = false;
  esp_timer_stop(deadline);
  Serial.println("[OTA] Confirmed after image and valid schedule");
}

class HttpsStream : public Stream {
public:
  explicit HttpsStream(esp_http_client_handle_t client) : client_(client) {}
  int available() override { return 0; }
  int read() override { char c; return readBytes(&c, 1) == 1 ? uint8_t(c) : -1; }
  int peek() override { return -1; }
  size_t write(uint8_t) override { return 0; }
  size_t readBytes(char *buffer, size_t length) override {
    const int n = esp_http_client_read_response(client_, buffer, length);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }
private:
  esp_http_client_handle_t client_;
};

void download(const String &url, size_t size, const char *sha) {
  uint8_t digest[32];
  if (Ota::pending() || !layoutValid() || size < OtaImage::PREFIX_SIZE ||
      size > OtaImage::SLOT_SIZE || !OtaImage::parseSha256(sha, digest)) {
    error("invalid request, trial state or partition layout");
    return;
  }
  // Direct, temporary artifact URLs can carry a signed query string.
  // Stay below the USB console line budget including size/hash/command.
  if (!url.startsWith("https://") || url.length() > 1536 ||
      url.indexOf('@') >= 0 || url.indexOf(' ') >= 0 ||
      url.indexOf('\r') >= 0 || url.indexOf('\n') >= 0 ||
      WiFi.status() != WL_CONNECTED) {
    error("requires Wi-Fi and a direct HTTPS URL without credentials");
    return;
  }
  configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
  const uint32_t started = millis();
  while (time(nullptr) < 1735689600 && millis() - started < 15000) delay(50);
  if (time(nullptr) < 1735689600) { error("clock not synchronized"); return; }
  Serial.printf("[OTA] Before TLS: free=%u largest=%u bytes\n", ESP.getFreeHeap(),
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 10000;
  config.disable_auto_redirect = true;
  config.buffer_size = 4096;
  auto client = esp_http_client_init(&config);
  if (!client) { error("HTTP allocation failed"); return; }
  esp_http_client_set_header(client, "Accept-Encoding", "identity");
  bool installed = false;
  if (esp_http_client_open(client, 0) == ESP_OK &&
      esp_http_client_fetch_headers(client) == static_cast<int64_t>(size) &&
      esp_http_client_get_status_code(client) == 200 &&
      !esp_http_client_is_chunked_response(client)) {
    Serial.printf("[OTA] After TLS: free=%u largest=%u bytes\n", ESP.getFreeHeap(),
        heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    HttpsStream source(client);
    installed = Ota::writeImage(source, size, sha);
  } else {
    error("TLS/HTTP failed, redirect, or incorrect Content-Length");
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (installed) { Serial.flush(); ESP.restart(); }
}
} // namespace

namespace Ota {
void begin() {
  esp_ota_img_states_t state;
  trial = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY;
  Serial.printf("[OTA] layout=%s slot=%s trial=%d\n", layoutValid() ? "ota-4m-v1" : "invalid",
      esp_ota_get_running_partition()->label, trial);
  if (!trial) return;
  const esp_timer_create_args_t args = {reject, nullptr, ESP_TIMER_TASK, "ota-trial", false};
  if (esp_timer_create(&args, &deadline) != ESP_OK ||
      esp_timer_start_once(deadline, 180ULL * 1000000) != ESP_OK) reject(nullptr);
}
bool pending() { return trial; }
bool holding() {
  if (holdUntil && static_cast<int32_t>(holdUntil - millis()) > 0) return true;
  holdUntil = 0;
  return false;
}
void imageReceived() { imageOk = true; confirm(); }
void scheduleReceived(const String &payload) {
  if (!trial || payload.length() > 8192) return;
  JsonDocument doc;
  if (deserializeJson(doc, payload) || !doc["nextRefreshSec"].is<int>() ||
      doc["nextRefreshSec"].as<int>() <= 0) return;
  scheduleOk = true;
  confirm();
}
void setNoticeHandler(void (*handler)()) { noticeHandler = handler; }

bool writeImage(Stream &source, size_t size, const char *sha256) {
  uint8_t expected[32];
  if (trial || !layoutValid() || size < OtaImage::PREFIX_SIZE || size > OtaImage::SLOT_SIZE ||
      !OtaImage::parseSha256(sha256, expected)) return error("invalid size, hash, trial or layout");
  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[4096]);
  if (!buffer) return error("no transfer buffer");
  // Receive the entire first USB chunk before erasing flash. The transport
  // withholds its acknowledgement until the next read, after erase/write,
  // so a sender never overruns the UART while flash interrupts are masked.
  const size_t first = min(size_t(4096), size);
  if (source.readBytes(reinterpret_cast<char *>(buffer.get()), first) != first ||
      !OtaImage::compatible(buffer.get(), first)) return error("incompatible/truncated image prefix");
  const auto *partition = esp_ota_get_next_update_partition(nullptr);
  if (!partition || partition->address == esp_ota_get_running_partition()->address ||
      partition->size != OtaImage::SLOT_SIZE) return error("no inactive slot");
  esp_ota_handle_t handle;
  if (esp_ota_begin(partition, size, &handle) != ESP_OK) return error("could not open inactive slot");
  // The panel message takes seconds. No network/USB bytes are requested meanwhile.
  if (noticeHandler) noticeHandler();
  mbedtls_sha256_context hash;
  mbedtls_sha256_init(&hash);
  bool ok = mbedtls_sha256_starts_ret(&hash, 0) == 0 &&
      mbedtls_sha256_update_ret(&hash, buffer.get(), first) == 0;
  esp_err_t writeResult = esp_ota_write(handle, buffer.get(), first);
  ok = ok && writeResult == ESP_OK;
  size_t received = first;
  const uint32_t started = millis();
  while (ok && received < size && millis() - started < 300000) {
    const size_t wanted = min(size_t(4096), size - received);
    const size_t n = source.readBytes(reinterpret_cast<char *>(buffer.get()), wanted);
    if (n != wanted) Serial.printf("[OTA] Short read at %u: got %u / %u bytes\n",
        unsigned(received), unsigned(n), unsigned(wanted));
    ok = n == wanted && mbedtls_sha256_update_ret(&hash, buffer.get(), n) == 0 &&
        (writeResult = esp_ota_write(handle, buffer.get(), n)) == ESP_OK;
    received += n;
    delay(1);
  }
  uint8_t actual[32];
  ok = ok && received == size && mbedtls_sha256_finish_ret(&hash, actual) == 0 &&
      memcmp(actual, expected, sizeof(actual)) == 0;
  mbedtls_sha256_free(&hash);
  if (!ok) {
    Serial.printf("[OTA] Rejected at %u / %u bytes, flash=%s\n",
        unsigned(received), unsigned(size), esp_err_to_name(writeResult));
    esp_ota_abort(handle);
    return error("transfer, SHA-256 or deadline failed; boot slot unchanged");
  }
  // esp_ota_end also verifies the ESP image structure/checksum/chip revision.
  if (esp_ota_end(handle) != ESP_OK) return error("ESP image validation failed");
  if (esp_ota_set_boot_partition(partition) != ESP_OK) return error("could not select new slot");
  Serial.printf("[bench] OTA staged %u bytes in %s; rebooting\n", unsigned(size), partition->label);
  return true;
}

void command(const String &args) {
  if (args == "status" || args == "hold") {
    holdUntil = millis() + 30UL * 60000;
    Serial.printf("[bench] OTA slot=%s trial=%d layout=%s free=%u largest=%u; hold 30 min\n",
        esp_ota_get_running_partition()->label, trial, layoutValid() ? "ota-4m-v1" : "invalid",
        ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  } else if (args == "resume") {
    holdUntil = 0;
    Serial.println("[bench] OTA hold released");
  } else if (args == "reject" && trial) {
    reject(nullptr);
  } else {
    // USB authorizes an exact artifact; no HTTP /schedule offer is consumed.
    const int first = args.indexOf(' '), second = args.indexOf(' ', first + 1);
    if (first <= 0 || second <= first) { error("usage: ota <bytes> <sha256> <https-url>"); return; }
    const String length = args.substring(0, first);
    for (unsigned i = 0; i < length.length(); ++i)
      if (!isDigit(length[i])) { error("invalid length"); return; }
    const unsigned long bytes = strtoul(length.c_str(), nullptr, 10);
    if (length.length() > 7 || bytes > OtaImage::SLOT_SIZE) { error("invalid length"); return; }
    download(args.substring(second + 1), bytes, args.substring(first + 1, second).c_str());
  }
}
} // namespace Ota
#endif
