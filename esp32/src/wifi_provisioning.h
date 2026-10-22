#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include <Arduino.h>

/**
 * WiFi Provisioning Module (docs/wifi.md)
 *
 * A Wignet remembers up to 5 Wi-Fi networks, the most recently joined first.
 * At start-up it joins the strongest one in range. Only when none is, it
 * opens its setup access point "Wignet-XXXX" (WiFiManager's captive portal);
 * a network saved there is added to the list. With known networks the
 * portal closes after 3 minutes without a visitor and they are tried again.
 */

// Initialize WiFiManager and attempt connection
// Creates a captive portal AP if no stored credentials are available or if the
// saved network cannot be reached (for example after moving the device).
// Setup WiFi provisioning (starts AP until valid credentials are saved)
// Takes an optional callback for when the configuration portal starts
bool setupWiFiProvisioning(void (*apCallback)(String) = nullptr);

// Forget every known network; the Wignet opens its setup portal on next boot.
void resetWiFiCredentials();

// After losing Wi-Fi: scan and join the strongest known network in range.
bool reconnectKnownWiFi();

// How many networks the Wignet knows (0 to 5).
int knownNetworkCount();

// For /schedule `wifi=` (docs/wifi.md): {"cur": ssid in use, "rssi": dBm,
// "known": [ssid, ...]} as JSON, never passwords.
String wifiReportJson();

// Apply /schedule `wifiForget`: forget each listed network, never the one
// in use. Takes the whole /schedule answer; returns how many were forgotten.
int applyWifiForget(const String &schedulePayload);

// Check if WiFi credentials are stored in flash
bool hasStoredCredentials();

// Get the AP name used for provisioning
String getProvisioningAPName();

#endif
