#include "wgn6_format.h"

#include <limits.h>
#include <string.h>

namespace {

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

} // namespace

namespace Wgn6 {

bool decodeHeader(const uint8_t *bytes, size_t length, Header &header) {
  if (!bytes || length != HEADER_SIZE || memcmp(bytes, "WGN6", 4) != 0 ||
      bytes[4] != VERSION || bytes[5] != HEADER_SIZE ||
      bytes[6] != PROFILE_SPECTRA6 ||
      bytes[7] != PACKING_4BPP_HIGH_NIBBLE_FIRST) {
    return false;
  }

  header.width = readLe16(bytes + 8);
  header.height = readLe16(bytes + 10);
  header.payloadLength = readLe32(bytes + 12);
  header.payloadCrc32 = readLe32(bytes + 16);
  return header.width > 0 && header.height > 0;
}

bool expectedPayloadLength(const Header &header, size_t &length) {
  if (header.height == 0 ||
      static_cast<size_t>(header.width) > SIZE_MAX / header.height) {
    return false;
  }
  const size_t pixelCount =
      static_cast<size_t>(header.width) * static_cast<size_t>(header.height);
  length = pixelCount / 2U + (pixelCount & 1U);
  return length == header.payloadLength;
}

bool validatePalette(const Header &header, const uint8_t *payload,
                     size_t length) {
  size_t expectedLength = 0;
  if (!payload || !expectedPayloadLength(header, expectedLength) ||
      length != expectedLength) {
    return false;
  }

  const size_t pixelCount =
      static_cast<size_t>(header.width) * static_cast<size_t>(header.height);
  for (size_t byteIndex = 0; byteIndex < length; ++byteIndex) {
    const uint8_t packed = payload[byteIndex];
    if ((packed >> 4U) > 5U) {
      return false;
    }

    const bool isFinalPadding =
        (pixelCount & 1U) && (byteIndex == length - 1U);
    const uint8_t lowNibble = packed & 0x0FU;
    if (isFinalPadding ? (lowNibble != WHITE) : (lowNibble > 5U)) {
      return false;
    }
  }
  return true;
}

uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

} // namespace Wgn6
