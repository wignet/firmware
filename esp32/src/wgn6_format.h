#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Wgn6 {

// WGN6 v1 is deliberately small and fixed so old firmware can reject future
// versions safely. All multi-byte integers are little-endian.
//
// Offset  Size  Field
//   0       4   ASCII magic "WGN6"
//   4       1   version = 1
//   5       1   header size = 20
//   6       1   profile = 1 (Spectra 6)
//   7       1   packing = 1 (two 4-bit pixels per byte)
//   8       2   width
//  10       2   height
//  12       4   payload length
//  16       4   IEEE CRC-32 of the payload
//
// The payload is top-down, row-major. The left/even pixel is in the high
// nibble. Logical colors match GxEPD2's generic 7-color encoding:
//   0 black, 1 white, 2 green, 3 blue, 4 red, 5 yellow.
// Pixels are packed continuously across row boundaries. If width*height is
// odd, the single unused low nibble at the end of the payload must be white
// (1). Values 6..15 are invalid in v1.
constexpr size_t HEADER_SIZE = 20;
constexpr uint8_t VERSION = 1;
constexpr uint8_t PROFILE_SPECTRA6 = 1;
constexpr uint8_t PACKING_4BPP_HIGH_NIBBLE_FIRST = 1;
constexpr uint8_t WHITE = 1;

struct Header {
  uint16_t width;
  uint16_t height;
  uint32_t payloadLength;
  uint32_t payloadCrc32;
};

bool decodeHeader(const uint8_t *bytes, size_t length, Header &header);
bool expectedPayloadLength(const Header &header, size_t &length);
bool validatePalette(const Header &header, const uint8_t *payload,
                     size_t length);
uint32_t crc32(const uint8_t *data, size_t length);

} // namespace Wgn6
