#include "wifi_provisioning.h"
#include "portal_assets.h"
#include <WiFiManager.h>

#if defined(ESP8266)
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#else
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#endif

// Configuration constants
// Keep provisioning available until credentials are saved. With a fixed
// timeout the AP could still be visible in the iPhone's saved networks while
// its DNS/web portal had already been shut down, especially after moving away
// from the previously configured Wi-Fi.
static const int CONFIG_PORTAL_TIMEOUT_SEC = 0;
static const char *AP_PASSWORD = nullptr;         // Open AP (no password)
// Scan results older than this are refreshed on the next network list request.
static const unsigned long SCAN_MAX_AGE_MS = 30000;
// How long a request waits for a scan WiFiManager already started.
static const unsigned long SCAN_WAIT_MS = 10000;
static const int MAX_SCANNED_NETWORKS = 64;
static const int MAX_LISTED_NETWORKS = 20;
#if !defined(ESP8266)
static const wifi_power_t WIFI_TX_POWER = WIFI_POWER_8_5dBm;
#endif

// Known networks (docs/wifi.md): up to KNOWN_MAX, the most recently joined
// first. The setup portal adds one; the Wignet joins the strongest one in range.
static const int KNOWN_MAX = 5;
static const unsigned long JOIN_TIMEOUT_MS = 15000;  // per network tried
// With known networks, the portal closes after this long without a visitor
// and the Wignet tries them again: a router that boots slower than the
// Wignet no longer leaves it waiting in setup for ever.
static const int KNOWN_RETRY_PORTAL_SEC = 180;

// Global WiFiManager instance and callback pointer
static WiFiManager wm;
static void (*onAPStartCallback)(String) = nullptr;
static unsigned long lastScanAt = 0;
// Whether POST /wifisave has asked WiFiManager to join a network since the
// portal opened (GET /save-status.json).
static bool saveRequested = false;

// WiFiManager's own pages behind ours: an unauthenticated firmware upload
// (/update, /u), erasing the Wi-Fi settings (/erase), restarts, its old
// setup and info pages. The setup network is open, so none of them may answer.
static const char *const BLOCKED_PORTAL_PATHS[] = {
    "/0wifi", "/info", "/param", "/paramsave", "/restart", "/exit",
    "/close", "/erase", "/status", "/update", "/u"};

struct KnownNetwork {
  char ssid[33];
  char pass[65];
};
struct KnownList {
  uint32_t magic;
  uint8_t count;
  KnownNetwork items[KNOWN_MAX];
};
static const uint32_t KNOWN_MAGIC = 0x57494631; // "WIF1"
static KnownList known;

static void loadKnown() {
  memset(&known, 0, sizeof(known));
#if defined(ESP8266)
  EEPROM.begin(sizeof(KnownList));
  EEPROM.get(0, known);
  EEPROM.end();
#else
  Preferences prefs;
  if (prefs.begin("wignet-wifi", true)) {
    prefs.getBytes("known", &known, sizeof(known));
    prefs.end();
  }
#endif
  if (known.magic != KNOWN_MAGIC || known.count > KNOWN_MAX) {
    memset(&known, 0, sizeof(known));
    known.magic = KNOWN_MAGIC;
  }
}

static void saveKnown() {
#if defined(ESP8266)
  EEPROM.begin(sizeof(KnownList));
  EEPROM.put(0, known);
  EEPROM.commit();
  EEPROM.end();
#else
  Preferences prefs;
  if (prefs.begin("wignet-wifi", false)) {
    prefs.putBytes("known", &known, sizeof(known));
    prefs.end();
  }
#endif
}

// Put a network first; the oldest falls off past KNOWN_MAX.
static void rememberNetwork(const String &ssid, const String &pass) {
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64) return;
  KnownNetwork entry;
  memset(&entry, 0, sizeof(entry));
  strncpy(entry.ssid, ssid.c_str(), sizeof(entry.ssid) - 1);
  strncpy(entry.pass, pass.c_str(), sizeof(entry.pass) - 1);
  int found = -1;
  for (int i = 0; i < known.count; i++)
    if (strcmp(known.items[i].ssid, entry.ssid) == 0) found = i;
  if (found == 0 && strcmp(known.items[0].pass, entry.pass) == 0) return; // nothing new
  const int last = found >= 0 ? found : min(static_cast<int>(known.count), KNOWN_MAX - 1);
  for (int i = last; i > 0; i--) known.items[i] = known.items[i - 1];
  known.items[0] = entry;
  if (found < 0 && known.count < KNOWN_MAX) known.count++;
  saveKnown();
  Serial.printf("[WiFi] Remembered \"%s\" (%d known)\n", entry.ssid, known.count);
}

// The network WiFiManager saved before this list existed becomes its first
// entry, so a Wignet updated in place keeps its Wi-Fi.
static void adoptSavedNetwork() {
  if (known.count > 0) return;
#if defined(ESP8266)
  const String ssid = WiFi.SSID(), pass = WiFi.psk();
#else
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) return;
  const String ssid = String(reinterpret_cast<char *>(conf.sta.ssid));
  const String pass = String(reinterpret_cast<char *>(conf.sta.password));
#endif
  if (ssid.length()) rememberNetwork(ssid, pass);
}

// Scan, then try the known networks in range, strongest first.
static bool joinKnownNetwork() {
  if (known.count == 0) return false;
  const int count = WiFi.scanNetworks();
  int order[KNOWN_MAX], rssi[KNOWN_MAX], inRange = 0;
  for (int k = 0; k < known.count; k++) {
    int best = -1000;
    for (int i = 0; i < count; i++)
      if (WiFi.SSID(i) == known.items[k].ssid && WiFi.RSSI(i) > best) best = WiFi.RSSI(i);
    if (best == -1000) continue;
    int at = inRange++;
    while (at > 0 && rssi[at - 1] < best) {
      order[at] = order[at - 1];
      rssi[at] = rssi[at - 1];
      at--;
    }
    order[at] = k;
    rssi[at] = best;
  }
  WiFi.scanDelete();
  Serial.printf("[WiFi] %d of %d known networks in range\n", inRange, known.count);
  for (int n = 0; n < inRange; n++) {
    const KnownNetwork net = known.items[order[n]];
    Serial.printf("[WiFi] Joining \"%s\" (%d dBm)\n", net.ssid, rssi[n]);
    WiFi.begin(net.ssid, net.pass);
    const unsigned long started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < JOIN_TIMEOUT_MS) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
      rememberNetwork(net.ssid, net.pass); // most recent first
      return true;
    }
    Serial.printf("[WiFi] Could not join \"%s\"\n", net.ssid);
    WiFi.disconnect();
  }
  return false;
}

static void sendNoCacheHeaders() {
  wm.server->sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
}

static void sendCaptivePortalRedirect() {
  const String location = "http://" + WiFi.softAPIP().toString() + "/";
  wm.server->sendHeader("Location", location, true);
  sendNoCacheHeaders();
  wm.server->send(302, "text/plain", "");
  wm.server->client().stop();
}

static bool isIpAddress(const String &host) {
  for (size_t i = 0; i < host.length(); i++) {
    const char c = host[i];
    if (c != '.' && (c < '0' || c > '9')) return false;
  }
  return host.length() > 0;
}

// The captive DNS answers every name with the AP, so a page may be requested
// as captive.apple.com/. Move it to the AP address first, as WiFiManager does,
// so the page and its fetch() calls share one origin.
static bool redirectToPortalHost() {
  if (isIpAddress(wm.server->hostHeader())) return false;
  sendCaptivePortalRedirect();
  return true;
}

static void appendJsonString(String &out, const String &value) {
  out += '"';
  for (size_t i = 0; i < value.length(); i++) {
    const unsigned char c = value[i];
    if (c == '"' || c == '\\') {
      out += '\\';
      out += (char)c;
    } else if (c < 0x20) {
      char escaped[7];
      snprintf(escaped, sizeof(escaped), "\\u%04x", c);
      out += escaped;
    } else {
      out += (char)c;
    }
  }
  out += '"';
}

static bool isSecureNetwork(int index) {
#if defined(ESP8266)
  return WiFi.encryptionType(index) != ENC_TYPE_NONE;
#else
  return WiFi.encryptionType(index) != WIFI_AUTH_OPEN;
#endif
}

static int scanNetworks(bool force) {
  int count = WiFi.scanComplete();
  const unsigned long waitStarted = millis();
  // WiFiManager starts an asynchronous scan when the portal opens: use it.
  while (count == WIFI_SCAN_RUNNING && millis() - waitStarted < SCAN_WAIT_MS) {
    delay(100);
    count = WiFi.scanComplete();
  }
  if (count >= 0 && lastScanAt == 0) lastScanAt = millis();
  if (force || count < 0 || millis() - lastScanAt > SCAN_MAX_AGE_MS) {
    count = WiFi.scanNetworks();
    lastScanAt = millis();
  }
  return count;
}

// GET /networks.json[?refresh=1]: the strongest entry per SSID, strongest
// first, as {"networks":[{"ssid","rssi","secure"}]} for portal/portal.js.
static void handleNetworks() {
  const int count = scanNetworks(wm.server->hasArg("refresh"));
  if (count < 0) {
    // WIFI_SCAN_FAILED: the portal says the search failed, not that no
    // network is in range.
    sendNoCacheHeaders();
    wm.server->send(503, "application/json", "{\"error\":\"scan_failed\"}");
    return;
  }
  int kept[MAX_SCANNED_NETWORKS];
  int keptCount = 0;
  for (int i = 0; i < count && i < MAX_SCANNED_NETWORKS; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    int same = -1;
    for (int k = 0; k < keptCount; k++) {
      if (WiFi.SSID(kept[k]) == ssid) {
        same = k;
        break;
      }
    }
    if (same < 0) {
      kept[keptCount++] = i;
    } else if (WiFi.RSSI(i) > WiFi.RSSI(kept[same])) {
      kept[same] = i;
    }
  }
  for (int a = 1; a < keptCount; a++) {
    const int index = kept[a];
    int b = a - 1;
    while (b >= 0 && WiFi.RSSI(kept[b]) < WiFi.RSSI(index)) {
      kept[b + 1] = kept[b];
      b--;
    }
    kept[b + 1] = index;
  }

  String json = "{\"networks\":[";
  for (int k = 0; k < keptCount && k < MAX_LISTED_NETWORKS; k++) {
    const int index = kept[k];
    if (k > 0) json += ',';
    json += "{\"ssid\":";
    appendJsonString(json, WiFi.SSID(index));
    json += ",\"rssi\":";
    json += String(WiFi.RSSI(index));
    json += ",\"secure\":";
    json += isSecureNetwork(index) ? "true" : "false";
    json += '}';
  }
  json += "]}";
  sendNoCacheHeaders();
  wm.server->send(200, "application/json", json);
}

// GET /device.json: the access point name (Wignet-XXXX) shown by the portal.
static void handleDevice() {
  String json = "{\"ap\":";
  appendJsonString(json, wm.getConfigPortalSSID());
  json += '}';
  sendNoCacheHeaders();
  wm.server->send(200, "application/json", json);
}

// GET /save-status.json: how the last Save went, for portal/portal.js.
// WiFiManager answers POST /wifisave first, then tries the network, blocking
// the web server; on success it closes the portal. So once a save was asked
// for, a request answered here comes after a failed attempt:
// {"state":"failed","reason":"password"|"not_found"|"other"}. Before any
// save, {"state":"idle"}.
static void handleSaveStatus() {
  sendNoCacheHeaders();
  if (!saveRequested) {
    wm.server->send(200, "application/json", "{\"state\":\"idle\"}");
    return;
  }
  const uint8_t result = wm.getLastConxResult();
  // 7 is WiFiManager's own WL_STATION_WRONG_PASSWORD (a private member).
  const char *reason = result == 7 ? "password" : result == WL_NO_SSID_AVAIL ? "not_found" : "other";
  String json = "{\"state\":\"failed\",\"reason\":\"";
  json += reason;
  json += "\"}";
  wm.server->send(200, "application/json", json);
}

static void sendNotFound() {
  wm.server->send(404, "text/plain", "Not found");
}

static void registerPortalRoutes() {
  // Called before WiFiManager installs its routes; the web server uses the
  // first matching handler, so these replace WiFiManager's own pages.
  // WiFiManager keeps POST /wifisave, which reads only `s` and `p`.
  //
  // Explicit OS connectivity-check URLs: iOS uses the Apple endpoints below
  // to decide whether to open the Captive Network Assistant automatically.
  wm.server->on("/hotspot-detect.html", sendCaptivePortalRedirect);
  wm.server->on("/library/test/success.html", sendCaptivePortalRedirect);
  wm.server->on("/generate_204", sendCaptivePortalRedirect);
  wm.server->on("/gen_204", sendCaptivePortalRedirect);
  wm.server->on("/connecttest.txt", sendCaptivePortalRedirect);
  wm.server->on("/ncsi.txt", sendCaptivePortalRedirect);

  // Setup pages and their assets, embedded from portal/ by embed_portal.py.
  for (const PortalAsset &entry : PORTAL_ASSETS) {
    const PortalAsset *asset = &entry;
    const bool isPage = strncmp(asset->path, "/static/", 8) != 0;
    wm.server->on(asset->path, isPage ? HTTP_ANY : HTTP_GET, [asset, isPage]() {
      if (isPage && redirectToPortalHost()) return;
      if (isPage) {
        sendNoCacheHeaders();
      } else {
        // Pages ask for /static/x?v=<content hash> (embed_portal.py): keep
        // them, so the next page only loads itself (picking a network took
        // 1-2 s while every page loaded 31 KB again).
        wm.server->sendHeader("Cache-Control", "public, max-age=31536000, immutable");
      }
      wm.server->send_P(200, asset->contentType, asset->body, asset->length);
    });
  }
  wm.server->on("/networks.json", HTTP_GET, handleNetworks);
  wm.server->on("/device.json", HTTP_GET, handleDevice);
  wm.server->on("/save-status.json", HTTP_GET, handleSaveStatus);
  // Registered first, so they win over WiFiManager's handlers for any
  // method; an upload to /u reaches no upload handler and is dropped.
  for (const char *path : BLOCKED_PORTAL_PATHS) wm.server->on(path, sendNotFound);
}

static void applyLowTxPower(const char *phase) {
#if defined(ESP8266)
  WiFi.setOutputPower(10.0f);
  Serial.printf("[WiFi] TX power limited during %s\n", phase);
#else
  if (WiFi.setTxPower(WIFI_TX_POWER)) {
    Serial.printf("[WiFi] TX power limited during %s\n", phase);
  } else {
    Serial.printf("[WiFi] Failed to limit TX power during %s\n", phase);
  }
#endif
}

// Internal callback for WiFiManager
static void internalConfigModeCallback(WiFiManager *myWiFiManager) {
  Serial.println("[WiFi] Entered config mode");
  applyLowTxPower("AP config mode");
  if (onAPStartCallback) {
    onAPStartCallback(myWiFiManager->getConfigPortalSSID());
  }
}

String getProvisioningAPName() {
  // Generate AP name from MAC address: Wignet-XXXX
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  return "Wignet-" + mac.substring(mac.length() - 4);
}

bool hasStoredCredentials() {
#if defined(ESP8266)
  return WiFi.SSID().length() > 0;
#else
  // WiFiManager stores credentials in NVS
  // Check if we have a stored SSID
  wifi_config_t conf;
  esp_wifi_get_config(WIFI_IF_STA, &conf);
  return strlen((char *)conf.sta.ssid) > 0;
#endif
}

void resetWiFiCredentials() {
  Serial.println("[WiFi] Resetting stored credentials...");
  loadKnown();
  known.count = 0;
  memset(known.items, 0, sizeof(known.items));
  saveKnown();
  wm.resetSettings();
  Serial.println("[WiFi] Credentials cleared. Restart to enter AP mode.");
}

bool setupWiFiProvisioning(void (*apCallback)(String)) {
  Serial.println("\n=== WiFi Provisioning ===");

  onAPStartCallback = apCallback;

  // Configure WiFiManager
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_SEC);
  wm.setConnectTimeout(20); // 20 seconds to connect to saved network
  wm.setCaptivePortalEnable(true);
  wm.setAPClientCheck(true);
  wm.setWebPortalClientCheck(true);
  wm.setWebServerCallback(registerPortalRoutes);
  wm.setPreSaveConfigCallback([]() { saveRequested = true; });

  // Set AP callback
  wm.setAPCallback(internalConfigModeCallback);

  // Start WiFi before limiting TX power; otherwise Arduino ignores the request.
  WiFi.mode(WIFI_STA);
  delay(100);
  applyLowTxPower("STA startup");

  // Generate unique AP name
  String apName = getProvisioningAPName();
  Serial.printf("[WiFi] AP Name: %s\n", apName.c_str());

  // A known network in range, else the setup portal. With known networks
  // the portal closes after KNOWN_RETRY_PORTAL_SEC without a visitor
  // (setAPClientCheck) and they are tried again.
  loadKnown();
  adoptSavedNetwork();
  bool connected = joinKnownNetwork();
  while (!connected) {
    wm.setConfigPortalTimeout(known.count ? KNOWN_RETRY_PORTAL_SEC : CONFIG_PORTAL_TIMEOUT_SEC);
    saveRequested = false;
    if (wm.startConfigPortal(apName.c_str(), AP_PASSWORD)) {
      rememberNetwork(wm.getWiFiSSID(), wm.getWiFiPass());
      connected = true;
      break;
    }
    Serial.println("[WiFi] Setup portal closed; trying the known networks again");
    WiFi.mode(WIFI_STA);
    delay(100);
    connected = joinKnownNetwork();
  }

  if (connected) {
    Serial.println("\n[WiFi] Connected successfully!");
    Serial.printf("[WiFi] IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi] MAC Address: %s\n", WiFi.macAddress().c_str());
    Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("\n[WiFi] Failed to connect (timeout or cancelled)");
    Serial.println("[WiFi] Continuing without WiFi...");
  }

  return connected;
}

bool reconnectKnownWiFi() {
  if (known.magic != KNOWN_MAGIC) loadKnown();
  WiFi.mode(WIFI_STA);
  const bool joined = joinKnownNetwork();
  if (joined) Serial.printf("[WiFi] Back on \"%s\"\n", WiFi.SSID().c_str());
  return joined;
}

static void appendJsonText(String &out, const char *text) {
  out += '"';
  for (const char *c = text; *c; c++) {
    if (*c == '"' || *c == '\\') {
      out += '\\';
      out += *c;
    } else if (static_cast<unsigned char>(*c) < 0x20) {
      char escaped[7];
      snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(*c));
      out += escaped;
    } else {
      out += *c;
    }
  }
  out += '"';
}

String wifiReportJson() {
  if (known.magic != KNOWN_MAGIC) loadKnown();
  String out = "{\"cur\":";
  if (WiFi.status() == WL_CONNECTED) appendJsonText(out, WiFi.SSID().c_str());
  else out += "null";
  out += ",\"rssi\":";
  out += WiFi.status() == WL_CONNECTED ? String(WiFi.RSSI()) : String("null");
  out += ",\"known\":[";
  for (int i = 0; i < known.count; i++) {
    if (i) out += ',';
    appendJsonText(out, known.items[i].ssid);
  }
  out += "]}";
  return out;
}

// One JSON string starting at payload[at] (its opening quote): the text,
// and `at` moved past its closing quote. \uXXXX below 0x80 only, as the
// server sends network names it received from here.
static String readJsonString(const String &payload, int &at) {
  String out;
  for (at++; at < static_cast<int>(payload.length()); at++) {
    const char c = payload[at];
    if (c == '"') {
      at++;
      break;
    }
    if (c == '\\' && at + 1 < static_cast<int>(payload.length())) {
      const char e = payload[++at];
      if (e == 'u' && at + 4 < static_cast<int>(payload.length())) {
        out += static_cast<char>(strtol(payload.substring(at + 1, at + 5).c_str(), nullptr, 16));
        at += 4;
      } else {
        out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
      }
    } else {
      out += c;
    }
  }
  return out;
}

int applyWifiForget(const String &payload) {
  const int key = payload.indexOf("\"wifiForget\"");
  if (key < 0) return 0;
  int at = payload.indexOf('[', key);
  const int end = payload.indexOf(']', key);
  if (at < 0 || end < at) return 0;
  if (known.magic != KNOWN_MAGIC) loadKnown();
  const String current = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
  int forgotten = 0;
  while (true) {
    at = payload.indexOf('"', at);
    if (at < 0 || at > end) break;
    const String ssid = readJsonString(payload, at);
    if (ssid == current) {
      Serial.printf("[WiFi] Not forgetting \"%s\": in use\n", ssid.c_str());
      continue;
    }
    for (int i = 0; i < known.count; i++) {
      if (ssid != known.items[i].ssid) continue;
      for (int j = i; j < known.count - 1; j++) known.items[j] = known.items[j + 1];
      known.count--;
      memset(&known.items[known.count], 0, sizeof(KnownNetwork));
      forgotten++;
      Serial.printf("[WiFi] Forgot \"%s\" (%d known)\n", ssid.c_str(), known.count);
      break;
    }
  }
  if (forgotten) saveKnown();
  return forgotten;
}

int knownNetworkCount() {
  if (known.magic != KNOWN_MAGIC) loadKnown();
  return known.count;
}
