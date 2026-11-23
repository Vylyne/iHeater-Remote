#include "RomProtocol.h"

#include "FirmwareImage.h"

namespace iheaterlink::policy {

uint8_t romXor(const uint8_t *data, size_t len) {
  uint8_t x = 0;
  for (size_t i = 0; i < len; i++) x ^= data[i];
  return x;
}

void buildRomCommand(uint8_t cmd, uint8_t out[2]) {
  out[0] = cmd;
  out[1] = (uint8_t)~cmd;
}

void buildRomAddress(uint32_t addr, uint8_t out[5]) {
  out[0] = (uint8_t)(addr >> 24);
  out[1] = (uint8_t)(addr >> 16);
  out[2] = (uint8_t)(addr >> 8);
  out[3] = (uint8_t)addr;
  out[4] = romXor(out, 4);
}

size_t buildEraseBody(uint16_t pages, bool extended, uint8_t *out, size_t cap) {
  if (pages == 0 || pages > (uint16_t)(kLastFlashPage + 1)) return 0;
  const size_t need = extended ? 2 + 2 * (size_t)pages + 1 : 1 + (size_t)pages + 1;
  if (cap < need) return 0;
  size_t n = 0;
  if (extended) {
    out[n++] = (uint8_t)((pages - 1) >> 8);
    out[n++] = (uint8_t)(pages - 1);
    for (uint16_t p = 0; p < pages; p++) {
      out[n++] = (uint8_t)(p >> 8);
      out[n++] = (uint8_t)p;
    }
  } else {
    out[n++] = (uint8_t)(pages - 1);
    for (uint16_t p = 0; p < pages; p++) out[n++] = (uint8_t)p;
  }
  out[n] = romXor(out, n);
  return n + 1;
}

size_t buildWriteBody(const uint8_t *data, size_t len, uint8_t *out, size_t cap) {
  if (len == 0 || len > kRomChunkBytes || len % 4 != 0) return 0;
  if (cap < len + 2) return 0;
  out[0] = (uint8_t)(len - 1);
  for (size_t i = 0; i < len; i++) out[1 + i] = data[i];
  out[len + 1] = romXor(out, len + 1);
  return len + 2;
}

void buildReadCount(size_t count, uint8_t out[2]) {
  out[0] = (uint8_t)(count - 1);
  out[1] = (uint8_t)~out[0];
}

}  // namespace iheaterlink::policy
