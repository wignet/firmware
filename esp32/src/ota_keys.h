#pragma once

// The server's public keys for OTA offers and confirmations (docs/ota.md,
// "Signing key"): SubjectPublicKeyInfo DER, as hex. `python -m backoffice.ota`
// prints the key and its id. Empty (only nullptr) means no key is known, so
// every offer and confirmation is refused. Rotation keeps both keys here.
static const char *const kOtaPublicKeysHex[] = {nullptr};
