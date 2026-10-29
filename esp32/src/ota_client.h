#pragma once

#include <Arduino.h>

// The OTA client of docs/ota.md (the `ota1` capability). Compiled only with
// WIGNET_OTA_CLIENT; `on=`, `oh=`, `ota=` and `oc=` are sent only with
// WIGNET_OTA_CAPABLE, which is what declares `ota1` in caps=.
namespace OtaClient {

// First thing in setup(): reads the attempt record, starts the 10-minute
// deadline when the running image is awaiting confirmation.
void begin();

// loop(): enforces the deadline of a pending image.
void tick();

// True while the running image awaits the server's confirmation.
bool pending();

// Before each /schedule GET: a fresh nonce, and the query fragment that
// carries it with oh=, ota= and oc= (docs/ota.md).
String pollQuery();

// After each 200 answer to that GET: acts on otaOffer or otaConfirm.
void onSchedule(const String &payload);

} // namespace OtaClient
