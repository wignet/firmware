#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Immediately after esp_app_desc_t in the first DROM segment. Both the
// packager and receiver check this before accepting an application image.
namespace OtaImage {
constexpr size_t DESCRIPTOR_OFFSET = 24 + 8 + 256;
struct Descriptor {
  char magic[16];
  char target[48];
  char layout[16];
};
constexpr Descriptor EXPECTED = {
    "WIGNET_OTA_V1", "waveshare_driver_75v2", "ota-4m-v1"};
constexpr size_t PREFIX_SIZE = DESCRIPTOR_OFFSET + sizeof(Descriptor);
constexpr size_t SLOT_SIZE = 0x1c0000;
static_assert(sizeof(Descriptor) == 80, "OTA descriptor format");

inline bool compatible(const uint8_t *prefix, size_t size) {
  // ESP32 application, first segment long enough for both descriptors.
  if (size < PREFIX_SIZE || prefix[0] != 0xe9 || prefix[1] == 0 ||
      prefix[12] != 0 || prefix[13] != 0) return false;
  const uint32_t firstLength = uint32_t(prefix[28]) | (uint32_t(prefix[29]) << 8) |
      (uint32_t(prefix[30]) << 16) | (uint32_t(prefix[31]) << 24);
  return firstLength >= 256 + sizeof(Descriptor) &&
      memcmp(prefix + DESCRIPTOR_OFFSET, &EXPECTED, sizeof(EXPECTED)) == 0;
}

inline bool parseSha256(const char *hex, uint8_t *out) {
  if (!hex || strlen(hex) != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    uint8_t byte = 0;
    for (size_t j = 0; j < 2; ++j) {
      const char c = hex[2 * i + j];
      int v = c >= '0' && c <= '9' ? c - '0' :
          c >= 'a' && c <= 'f' ? c - 'a' + 10 :
          c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (v < 0) return false;
      byte = (byte << 4) | v;
    }
    out[i] = byte;
  }
  return true;
}
} // namespace OtaImage
