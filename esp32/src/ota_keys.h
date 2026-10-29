#pragma once

// The server's public keys for OTA offers and confirmations (docs/ota.md,
// "Signing key"): SubjectPublicKeyInfo DER, as hex. `python -m backoffice.ota`
// prints the key and its id. Empty (only nullptr) means no key is known, so
// every offer and confirmation is refused. Rotation keeps both keys here.
// Production signing key, id 8675e48b (python -m backoffice.ota).
static const char *const kOtaPublicKeysHex[] = {
    "3059301306072a8648ce3d020106082a8648ce3d03010703420004d53bff3a738a2a87787245f1c3696a72db28342b52bc91ba449b4b49022df9e4a80729ff833807069ab07c0c8467fa2470649e4b7972e28b5dbca278fc4c2a21",
    nullptr,
};
