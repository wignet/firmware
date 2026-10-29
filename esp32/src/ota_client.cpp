#include "ota_client.h"

#if defined(WIGNET_OTA_CLIENT)

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <time.h>

#include "ota_ca.h"
#include "ota_keys.h"
#include "power_hal.h"

namespace {

constexpr uint64_t kResponseLimitUs = 60ULL * 1000000ULL;
constexpr uint64_t kDeadlineUs = 600ULL * 1000000ULL;
constexpr uint8_t kReportPolls = 20;
constexpr char kUrlPrefix[] = "https://api.wignet.app/firmware/";
constexpr uint64_t kMaxCounter = (1ULL << 53) - 1;
constexpr time_t kPlausibleTime = 1600000000;
constexpr size_t kChunk = 4096;
constexpr uint32_t kStallMs = 15000;
constexpr size_t kMaxKeys = 4;
constexpr size_t kMaxDer = 128;

enum Phase : uint8_t {
  kIdle = 0,
  kInstalling = 1, // record persisted, slot not selected yet
  kSelected = 2,   // slot selected; reboot expected
  kConfirmed = 3,
  kFailed = 4,
  kRolledBack = 5,
};

enum Reason : uint8_t {
  kNoReason = 0,
  kDownload,
  kHash,
  kSize,
  kWrite,
  kPower,
  kDeferred, // no CA certificate configured: the download waits
};

const char *reasonName(uint8_t reason) {
  switch (reason) {
    case kDownload: return "download";
    case kHash: return "hash";
    case kSize: return "size";
    case kWrite: return "write";
    case kPower: return "power";
    default: return "download";
  }
}

// The attempt record, one NVS blob (namespace "ota"). `counter` is the
// last installation attempt's counter (ota_ctr): it is never lowered.
struct Record {
  uint8_t version;
  uint8_t phase;
  uint8_t reason;
  uint8_t reportsLeft;
  uint8_t target;  // esp_partition_subtype_t of the slot the attempt wrote
  uint8_t prev;    // esp_partition_subtype_t of the slot it replaced
  uint8_t spare[2];
  uint64_t counter;
  uint8_t hash[32];
  uint32_t size;
};

Record rec;
bool pendingFlag = false;
uint64_t deadlineUs = 0;
bool runningHashReady = false;
uint8_t runningHash[32];
uint32_t runningLength = 0;

char macLower[18];
char nonceOutstanding[33];
bool nonceValid = false;
uint64_t nonceUs = 0;
char pollNonce[33]; // the nonce of the last poll, for the confirmation

struct KeyEntry {
  char id[9];
  uint8_t der[kMaxDer];
  size_t derLen;
};
KeyEntry keys[kMaxKeys];
size_t keyCount = 0;

void toHex(const uint8_t *in, size_t n, char *out) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = digits[in[i] >> 4];
    out[2 * i + 1] = digits[in[i] & 0x0f];
  }
  out[2 * n] = '\0';
}

// Decodes lowercase or uppercase hex of exactly `n` bytes.
bool fromHex(const char *in, size_t n, uint8_t *out) {
  if (strlen(in) != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    uint8_t v[2];
    for (int k = 0; k < 2; k++) {
      char c = in[2 * i + k];
      if (c >= '0' && c <= '9') v[k] = c - '0';
      else if (c >= 'a' && c <= 'f') v[k] = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v[k] = c - 'A' + 10;
      else return false;
    }
    out[i] = (v[0] << 4) | v[1];
  }
  return true;
}

bool isLowerHex(const char *s, size_t n) {
  if (strlen(s) != n) return false;
  for (size_t i = 0; i < n; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
  return true;
}

// Copies a JSON string field, failing when it is absent or too long.
bool copyString(JsonObjectConst o, const char *key, char *dst, size_t cap) {
  const char *v = o[key].as<const char *>();
  if (!v) return false;
  size_t n = strlen(v);
  if (n >= cap) return false;
  memcpy(dst, v, n + 1);
  return true;
}

void loadKeys() {
  keyCount = 0;
  for (size_t i = 0; kOtaPublicKeysHex[i] && keyCount < kMaxKeys; i++) {
    const char *hex = kOtaPublicKeysHex[i];
    size_t len = strlen(hex) / 2;
    if (len == 0 || len > kMaxDer || !fromHex(hex, len, keys[keyCount].der)) {
      Serial.printf("[OTA] Public key %u ignored: not hex DER\n", static_cast<unsigned>(i));
      continue;
    }
    keys[keyCount].derLen = len;
    uint8_t digest[32];
    mbedtls_sha256(keys[keyCount].der, len, digest, 0);
    toHex(digest, 4, keys[keyCount].id); // first 8 hex
    keyCount++;
  }
}

KeyEntry *findKey(const char *id) {
  for (size_t i = 0; i < keyCount; i++)
    if (strcmp(keys[i].id, id) == 0) return &keys[i];
  return nullptr;
}

bool loadRecord() {
  memset(&rec, 0, sizeof(rec));
  rec.version = 1;
  Preferences prefs;
  if (!prefs.begin("ota", true)) return false;
  bool ok = prefs.getBytesLength("rec") == sizeof(Record) &&
            prefs.getBytes("rec", &rec, sizeof(Record)) == sizeof(Record);
  prefs.end();
  if (!ok) memset(&rec, 0, sizeof(rec));
  rec.version = 1;
  return ok;
}

bool saveRecord() {
  Preferences prefs;
  if (!prefs.begin("ota", false)) return false;
  size_t n = prefs.putBytes("rec", &rec, sizeof(Record));
  prefs.end();
  return n == sizeof(Record);
}

void lowerMac() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(macLower, sizeof(macLower), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
}

// The 16-hex id of an installation: the first 16 hex of its hash.
void idOf(const uint8_t *hash, char *out) {
  toHex(hash, 8, out);
}

// SHA-256 over the running image: the header, its segments, the padding and
// the checksum byte, then the appended hash when there is one (docs/ota.md
// `oh=`). The slot's unused padding is excluded.
bool computeRunningImage(const esp_partition_t *part) {
  esp_image_header_t header;
  if (esp_partition_read(part, 0, &header, sizeof(header)) != ESP_OK ||
      header.magic != ESP_IMAGE_HEADER_MAGIC)
    return false;
  uint32_t pos = sizeof(header);
  for (uint8_t i = 0; i < header.segment_count; i++) {
    esp_image_segment_header_t seg;
    if (esp_partition_read(part, pos, &seg, sizeof(seg)) != ESP_OK) return false;
    pos += sizeof(seg) + seg.data_len;
    if (pos > part->size) return false;
  }
  // The checksum byte closes the last 16-byte block of the image.
  uint32_t end = ((pos + 16) / 16) * 16;
  if (header.hash_appended) end += 32;
  if (end > part->size) return false;

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  uint8_t buf[kChunk];
  for (uint32_t off = 0; off < end;) {
    size_t n = min<uint32_t>(sizeof(buf), end - off);
    if (esp_partition_read(part, off, buf, n) != ESP_OK) {
      mbedtls_sha256_free(&ctx);
      return false;
    }
    mbedtls_sha256_update(&ctx, buf, n);
    off += n;
  }
  mbedtls_sha256_finish(&ctx, runningHash);
  mbedtls_sha256_free(&ctx);
  runningLength = end;
  return true;
}

bool ecdsaVerify(const uint8_t *der, size_t derLen, const uint8_t *hash, const uint8_t *sig,
                 size_t sigLen) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  int r = mbedtls_pk_parse_public_key(&pk, der, derLen);
  if (r == 0 && (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECKEY ||
                 mbedtls_pk_ec(pk)->grp.id != MBEDTLS_ECP_DP_SECP256R1))
    r = -1;
  if (r == 0) r = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sig, sigLen);
  mbedtls_pk_free(&pk);
  return r == 0;
}

// Verifies a base64 DER signature over the canonical text, with the key the
// signer named. Unknown key or bad signature: false.
bool verifySignature(const char *keyId, const String &canonical, const char *sigB64) {
  KeyEntry *key = findKey(keyId);
  if (!key) return false;
  uint8_t sig[128];
  size_t sigLen = 0;
  if (mbedtls_base64_decode(sig, sizeof(sig), &sigLen, reinterpret_cast<const unsigned char *>(sigB64),
                            strlen(sigB64)) != 0 ||
      sigLen == 0)
    return false;
  uint8_t digest[32];
  mbedtls_sha256(reinterpret_cast<const uint8_t *>(canonical.c_str()), canonical.length(), digest, 0);
  return ecdsaVerify(key->der, key->derLen, digest, sig, sigLen);
}

// May this board install now? Plugged in, as it measures it. Without a
// measurement, the signed declaration (or the owner's override) decides.
bool powerAllows(const char *powerSource, bool powerOverride) {
  if (PowerHAL::available()) {
    PowerHAL::Report report = PowerHAL::read();
    if (report.valid) return report.external;
  }
  return strcmp(powerSource, "powered") == 0 || powerOverride;
}

// Records a failed attempt (its counter stays consumed) and reports it.
void recordFailure(Reason reason) {
  rec.phase = kFailed;
  rec.reason = reason;
  rec.reportsLeft = kReportPolls;
  if (!saveRecord()) Serial.println("[OTA] Could not persist the failed attempt");
}

// Streams the offered image into `slot`, hashing as it goes. Returns the
// reason on failure, kNoReason on success (the slot is then ready to select).
Reason downloadInto(const char *url, uint32_t size, const uint8_t *expectedHash,
                    const esp_partition_t *slot) {
  WiFiClientSecure secure;
  secure.setCACert(kOtaCaPem);
  secure.setTimeout(15);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  if (!http.begin(secure, url)) return kDownload;
  if (http.GET() != HTTP_CODE_OK) {
    http.end();
    return kDownload;
  }
  if (http.getSize() != static_cast<int>(size)) {
    http.end();
    return kSize;
  }

  esp_ota_handle_t handle;
  if (esp_ota_begin(slot, size, &handle) != ESP_OK) {
    http.end();
    return kWrite;
  }
  WiFiClient *stream = http.getStreamPtr();
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  uint8_t buf[kChunk];
  uint32_t got = 0;
  uint32_t lastDataMs = millis();
  Reason reason = kNoReason;
  while (got < size) {
    esp_task_wdt_reset();
    int avail = stream->available();
    if (avail <= 0) {
      if (!http.connected() || millis() - lastDataMs > kStallMs) break;
      delay(2);
      continue;
    }
    size_t want = min<size_t>(min<size_t>(avail, sizeof(buf)), size - got);
    int n = stream->readBytes(buf, want);
    if (n <= 0) break;
    lastDataMs = millis();
    mbedtls_sha256_update(&ctx, buf, n);
    if (esp_ota_write(handle, buf, n) != ESP_OK) {
      reason = kWrite;
      break;
    }
    got += n;
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  http.end();

  if (reason == kNoReason && got != size) reason = kDownload;
  if (reason == kNoReason && memcmp(digest, expectedHash, 32) != 0) reason = kHash;
  if (reason != kNoReason) {
    esp_ota_abort(handle);
    return reason;
  }
  if (esp_ota_end(handle) != ESP_OK) return kWrite;

  // The image must be for this chip, as the running one is.
  esp_image_header_t mine, theirs;
  if (esp_partition_read(slot, 0, &theirs, sizeof(theirs)) != ESP_OK ||
      theirs.magic != ESP_IMAGE_HEADER_MAGIC)
    return kWrite;
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (esp_partition_read(running, 0, &mine, sizeof(mine)) != ESP_OK ||
      mine.chip_id != theirs.chip_id)
    return kWrite;
  return kNoReason;
}

struct Offer {
  char id[17];
  char url[320];
  char sha[65];
  char hw[49];
  char nonce[33];
  char powerSource[12];
  char key[9];
  char sig[160];
  uint32_t size;
  uint64_t counter;
  int64_t expires;
  bool powerOverride;
};

bool readOffer(JsonObjectConst o, Offer &of) {
  if (!copyString(o, "id", of.id, sizeof(of.id)) || !copyString(o, "url", of.url, sizeof(of.url)) ||
      !copyString(o, "sha256", of.sha, sizeof(of.sha)) ||
      !copyString(o, "hw", of.hw, sizeof(of.hw)) ||
      !copyString(o, "nonce", of.nonce, sizeof(of.nonce)) ||
      !copyString(o, "powerSource", of.powerSource, sizeof(of.powerSource)) ||
      !copyString(o, "key", of.key, sizeof(of.key)) ||
      !copyString(o, "sig", of.sig, sizeof(of.sig)))
    return false;
  if (!o["size"].is<uint32_t>() || !o["counter"].is<uint64_t>() || !o["expires"].is<int64_t>() ||
      !o["powerOverride"].is<bool>())
    return false;
  of.size = o["size"].as<uint32_t>();
  of.counter = o["counter"].as<uint64_t>();
  of.expires = o["expires"].as<int64_t>();
  of.powerOverride = o["powerOverride"].as<bool>();
  return true;
}

// The canonical lines an offer is signed over (docs/ota.md, "The offer").
String offerCanonical(const Offer &of) {
  char num[24];
  String s = "wignet-ota1\n";
  s += macLower;
  s += "\n";
  s += of.hw;
  s += "\n";
  s += of.sha;
  s += "\n";
  snprintf(num, sizeof(num), "%u", static_cast<unsigned>(of.size));
  s += num;
  s += "\n";
  snprintf(num, sizeof(num), "%llu", static_cast<unsigned long long>(of.counter));
  s += num;
  s += "\n";
  snprintf(num, sizeof(num), "%lld", static_cast<long long>(of.expires));
  s += num;
  s += "\n";
  s += of.nonce;
  s += "\n";
  s += of.powerSource;
  s += "\n";
  s += of.powerOverride ? "true" : "false";
  s += "\n";
  s += of.url;
  return s;
}

void install(const Offer &of) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (!running || !next) return;

  uint8_t expected[32];
  fromHex(of.sha, 32, expected);

  // The attempt is persisted before anything is written, so an interrupted
  // installation still keeps its counter.
  rec.counter = of.counter;
  rec.target = static_cast<uint8_t>(next->subtype);
  rec.prev = static_cast<uint8_t>(running->subtype);
  rec.size = of.size;
  memcpy(rec.hash, expected, 32);
  rec.phase = kInstalling;
  rec.reason = kNoReason;
  rec.reportsLeft = 0;
  if (!saveRecord()) {
    Serial.println("[OTA] Could not persist the attempt; not installing");
    return;
  }

  Reason reason = downloadInto(of.url, of.size, expected, next);
  if (reason == kDeferred) {
    Serial.println("[OTA] Download deferred: no CA certificate configured");
    rec.phase = kIdle;
    saveRecord();
    return;
  }
  if (reason != kNoReason) {
    Serial.printf("[OTA] Installation failed: %s\n", reasonName(reason));
    recordFailure(static_cast<Reason>(reason));
    return;
  }

  // Selecting the slot is the last step before the reboot. A failed write
  // here leaves the record Installing: the next boot sees it as interrupted.
  if (esp_ota_set_boot_partition(next) != ESP_OK) {
    recordFailure(kWrite);
    return;
  }
  rec.phase = kSelected;
  if (!saveRecord()) Serial.println("[OTA] Could not persist the selected slot");
  Serial.println("[OTA] Rebooting into the new image");
  delay(100);
  esp_restart();
}

#define REFUSE(why)                              \
  do {                                           \
    Serial.printf("[OTA] Offer refused: %s\n", why); \
    return;                                      \
  } while (0)

void handleOffer(JsonObjectConst o) {
  Offer of;
  if (!readOffer(o, of)) REFUSE("unreadable fields");
  Serial.printf("[OTA] Offer %s seen: counter %llu, hw %s, expires %lld, clock %ld, last counter %llu\n",
                of.id, static_cast<unsigned long long>(of.counter), of.hw,
                static_cast<long long>(of.expires), static_cast<long>(time(nullptr)),
                static_cast<unsigned long long>(rec.counter));

  // Silent refusals: the offer is not for this board, or it is not signed.
  if (!findKey(of.key)) REFUSE("unknown key");
  if (strcmp(of.hw, WIGNET_HW_TARGET) != 0) REFUSE("hw is not this board");
  if (strncmp(of.url, kUrlPrefix, strlen(kUrlPrefix)) != 0) REFUSE("url outside /firmware/");
  if (!verifySignature(of.key, offerCanonical(of), of.sig)) REFUSE("signature");
  if (of.counter == 0 || of.counter > kMaxCounter) REFUSE("counter out of range");
  if (of.counter <= rec.counter) REFUSE("counter not above the last attempt");
  if (of.size == 0) REFUSE("size zero");
  // Only a plausible clock can judge `expires`; NTP is not the replay defence.
  if (time(nullptr) > kPlausibleTime && time(nullptr) >= of.expires) REFUSE("expired");
  if (strlen(of.id) != 16 || strncmp(of.id, of.sha, 16) != 0) REFUSE("id is not the hash");
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (!next || of.size > next->size) REFUSE("size exceeds the slot");

  Serial.printf("[OTA] Offer %s accepted (counter %llu)\n", of.id,
                static_cast<unsigned long long>(of.counter));
  if (!powerAllows(of.powerSource, of.powerOverride)) {
    rec.counter = of.counter;
    fromHex(of.sha, 32, rec.hash);
    rec.size = of.size;
    recordFailure(kPower);
    return;
  }
  install(of);
}

void handleConfirm(JsonObjectConst c) {
  char sha[65], nonce[33], key[9], sig[160];
  if (!copyString(c, "sha256", sha, sizeof(sha)) || !copyString(c, "nonce", nonce, sizeof(nonce)) ||
      !copyString(c, "key", key, sizeof(key)) || !copyString(c, "sig", sig, sizeof(sig)) ||
      !c["counter"].is<uint64_t>())
    return;
  if (c["counter"].as<uint64_t>() != rec.counter) return;
  if (strcmp(nonce, pollNonce) != 0) return;
  char runningHex[65];
  toHex(runningHash, 32, runningHex);
  if (strcmp(sha, runningHex) != 0) return;
  if (!findKey(key)) return;

  char num[24];
  snprintf(num, sizeof(num), "%llu", static_cast<unsigned long long>(rec.counter));
  String canonical = "wignet-ota-confirm1\n";
  canonical += macLower;
  canonical += "\n";
  canonical += WIGNET_HW_TARGET;
  canonical += "\n";
  canonical += runningHex;
  canonical += "\n";
  canonical += num;
  canonical += "\n";
  canonical += nonce;
  if (!verifySignature(key, canonical, sig)) return;

  if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
    Serial.println("[OTA] Could not mark the image valid; still pending");
    return;
  }
  pendingFlag = false;
  rec.phase = kConfirmed;
  rec.reason = kNoReason;
  rec.reportsLeft = kReportPolls;
  if (!saveRecord()) Serial.println("[OTA] Could not persist the confirmation");
  Serial.println("[OTA] Image confirmed by the server");
}

// Reject the pending image: it goes back to the previous slot.
void rejectPending() {
  Serial.println("[OTA] Deadline passed without confirmation; rolling back");
  rec.phase = kRolledBack;
  rec.reportsLeft = kReportPolls;
  saveRecord();
  esp_ota_mark_app_invalid_rollback_and_reboot();
  Serial.println("[OTA] Rollback failed; staying on the image");
}

} // namespace

// The Arduino core confirms a new image before setup() unless this returns
// true (esp32-hal-misc.c). A pending image is confirmed only by the server.
extern "C" bool verifyRollbackLater() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
         state == ESP_OTA_IMG_PENDING_VERIFY;
}

namespace OtaClient {

void begin() {
  lowerMac();
  loadKeys();
  loadRecord();
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running) esp_ota_get_state_partition(running, &state);

  if (running && state == ESP_OTA_IMG_PENDING_VERIFY) {
    deadlineUs = esp_timer_get_time() + kDeadlineUs;
    pendingFlag = true;
    if (computeRunningImage(running)) runningHashReady = true;
    if ((rec.phase == kInstalling || rec.phase == kSelected) &&
        running->subtype == rec.target && rec.phase != kSelected) {
      rec.phase = kSelected;
      saveRecord();
    }
    Serial.println("[OTA] Running a new image; awaiting the server's confirmation");
  } else if (running && rec.phase == kSelected && running->subtype == rec.prev) {
    // Selected, yet the previous slot runs: the bootloader rolled back.
    rec.phase = kRolledBack;
    rec.reportsLeft = kReportPolls;
    saveRecord();
  } else if (running && rec.phase == kInstalling && running->subtype == rec.prev) {
    // Interrupted before the selection: not a rollback, the counter stays.
    rec.phase = kIdle;
    saveRecord();
  }
}

void tick() {
  if (pendingFlag && esp_timer_get_time() >= static_cast<int64_t>(deadlineUs)) rejectPending();
}

bool pending() {
  return pendingFlag;
}

String pollQuery() {
  uint8_t raw[16];
  esp_fill_random(raw, sizeof(raw));
  toHex(raw, sizeof(raw), nonceOutstanding);
  memcpy(pollNonce, nonceOutstanding, sizeof(pollNonce));
  nonceValid = true;
  nonceUs = esp_timer_get_time();

  String query = "&on=";
  query += nonceOutstanding;
  if (pendingFlag && runningHashReady) {
    char hex[65];
    toHex(runningHash, 32, hex);
    query += "&oh=";
    query += hex;
  }

  String state;
  char idHex[17];
  idOf(rec.hash, idHex);
  if (pendingFlag) {
    state = String("pending:") + idHex;
  } else if ((rec.phase == kConfirmed || rec.phase == kFailed || rec.phase == kRolledBack) &&
               rec.reportsLeft > 0) {
    if (rec.phase == kConfirmed) state = String("confirmed:") + idHex;
    else if (rec.phase == kFailed) state = String("failed:") + idHex + ":" + reasonName(rec.reason);
    else state = String("rolledback:") + idHex;
    rec.reportsLeft--;
    saveRecord();
  }
  if (state.length() > 0) {
    char num[24];
    snprintf(num, sizeof(num), "%llu", static_cast<unsigned long long>(rec.counter));
    query += "&ota=" + state + "&oc=" + num;
  }
  return query;
}

void onSchedule(const String &payload) {
  if (!nonceValid) return;
  nonceValid = false; // one answer per poll
  if (esp_timer_get_time() - static_cast<int64_t>(nonceUs) > static_cast<int64_t>(kResponseLimitUs)) {
    Serial.println("[OTA] Answer older than 60 s; ignored");
    return;
  }
  if (payload.indexOf("\"otaOffer\"") < 0 && payload.indexOf("\"otaConfirm\"") < 0) return;

  JsonDocument filter;
  filter["otaOffer"] = true;
  filter["otaConfirm"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) {
    Serial.println("[OTA] Could not read otaOffer/otaConfirm");
    return;
  }
  if (pendingFlag) {
    if (doc["otaConfirm"].is<JsonObjectConst>()) handleConfirm(doc["otaConfirm"].as<JsonObjectConst>());
    return;
  }
  if (doc["otaOffer"].is<JsonObjectConst>()) handleOffer(doc["otaOffer"].as<JsonObjectConst>());
}

} // namespace OtaClient

#endif // WIGNET_OTA_CLIENT
