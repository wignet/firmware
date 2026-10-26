#include "ota_image.h"
#include <assert.h>
#include <vector>
int main() {
  std::vector<uint8_t> image(OtaImage::PREFIX_SIZE, 0);
  image[0] = 0xe9;
  image[1] = 1;
  image[28] = 0x50; image[29] = 1; // 256 + 80 bytes
  memcpy(image.data() + OtaImage::DESCRIPTOR_OFFSET, &OtaImage::EXPECTED, 80);
  assert(OtaImage::compatible(image.data(), image.size()));
  for (size_t n = 0; n < image.size(); ++n)
    assert(!OtaImage::compatible(image.data(), n));
  for (size_t n = OtaImage::DESCRIPTOR_OFFSET; n < image.size(); ++n) {
    image[n] ^= 1;
    assert(!OtaImage::compatible(image.data(), image.size()));
    image[n] ^= 1;
  }
  image[12] = 9; // same descriptor, wrong ESP chip
  assert(!OtaImage::compatible(image.data(), image.size()));
  image[12] = 0;
  image[29] = 0; // descriptor must be inside the first segment
  assert(!OtaImage::compatible(image.data(), image.size()));
  uint8_t digest[32];
  assert(OtaImage::parseSha256("0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789abcdef", digest));
  assert(digest[0] == 1 && digest[7] == 0xef && digest[15] == 0xef);
  assert(!OtaImage::parseSha256(nullptr, digest));
  assert(!OtaImage::parseSha256("0", digest));
  assert(!OtaImage::parseSha256("g123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", digest));
}
