#pragma once

// Shared test fixture: a synthetic iHeater image with a build record.
// Header-only; included by suites as "../image_fixture.h".

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "FirmwareImage.h"

namespace imagefixture {

inline void put16(std::vector<uint8_t> &b, size_t at, uint32_t v) {
  if (at + 2 > b.size()) return;
  b[at] = v & 0xFF;
  b[at + 1] = (v >> 8) & 0xFF;
}

inline void put32(std::vector<uint8_t> &b, size_t at, uint32_t v) {
  if (at + 4 > b.size()) return;
  for (int i = 0; i < 4; i++) b[at + i] = (v >> (8 * i)) & 0xFF;
}

// Vector table plus build record where they fit; filler bytes elsewhere.
inline std::vector<uint8_t> makeImage(size_t size) {
  std::vector<uint8_t> b(size);
  for (size_t i = 0; i < size; i++) b[i] = (uint8_t)((i * 7 + 3) & 0xFF);
  put32(b, 0, 0x20001800);
  put32(b, 4, 0x08000901);
  for (size_t v = 2; v < 48; v++) put32(b, v * 4, v == 43 ? 0x08000A15 : 0x080065F1);
  const size_t m = iheaterlink::policy::kMarkerOffset;
  put32(b, m + 0x00, iheaterlink::policy::kMarkerMagic);
  put16(b, m + 0x04, 1);   // record version
  put16(b, m + 0x06, 32);  // record size
  put16(b, m + 0x08, 2);   // build type: app
  put16(b, m + 0x0A, 1);   // protocol
  put16(b, m + 0x0C, 5);   // firmware version
  put16(b, m + 0x0E, 0);   // patch
  put32(b, m + 0x10, 0);   // length
  put32(b, m + 0x14, 0);   // CRC
  put32(b, m + 0x18, 0);   // git SHA
  put16(b, m + 0x1C, 0);   // flags
  put16(b, m + 0x1E, 0);   // reserved
  return b;
}

// Sets length, SHA and flags, then the CRC over the image with the CRC field 0.
inline void patchImage(std::vector<uint8_t> &b, size_t size, uint32_t sha, uint16_t flags) {
  const size_t m = iheaterlink::policy::kMarkerOffset;
  put32(b, m + 0x10, (uint32_t)size);
  put32(b, m + 0x18, sha);
  put16(b, m + 0x1C, flags);
  put32(b, m + 0x14, 0);
  put32(b, m + 0x14, iheaterlink::policy::crc32(b.data(), size));
}

}  // namespace imagefixture
