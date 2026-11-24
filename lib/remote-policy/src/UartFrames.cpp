#include "UartFrames.h"

#include <string.h>

extern "C" {
#include "modbus_rtu.h"
}

namespace iheaterlink::policy {

namespace {

void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)(v & 0xFF);
}

uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

void finish(uint8_t out[kRequestLen]) {
  uint16_t crc = modbus_crc16(out, kRequestLen - 2);
  out[6] = (uint8_t)(crc & 0xFF);  // CRC low byte first
  out[7] = (uint8_t)(crc >> 8);
}

Reply invalid() {
  Reply r;
  r.kind = ReplyKind::Invalid;
  return r;
}

}  // namespace

size_t buildReadRequest(uint8_t fc, uint16_t addr, uint16_t count, uint8_t out[kRequestLen]) {
  if (fc != kFcReadHolding && fc != kFcReadInput) return 0;
  if (count == 0 || count > kMaxReadRegs) return 0;
  out[0] = kSlaveAddr;
  out[1] = fc;
  put16(out + 2, addr);
  put16(out + 4, count);
  finish(out);
  return kRequestLen;
}

size_t buildWriteSingle(uint16_t addr, uint16_t value, uint8_t out[kRequestLen]) {
  out[0] = kSlaveAddr;
  out[1] = kFcWriteSingle;
  put16(out + 2, addr);
  put16(out + 4, value);
  finish(out);
  return kRequestLen;
}

size_t buildWriteMultiple(uint16_t addr, const uint16_t *values, uint8_t count, uint8_t *out,
                          size_t cap) {
  if (count == 0 || count > kMaxWriteRegs) return 0;
  const size_t len = 9 + 2 * (size_t)count;
  if (len > cap) return 0;
  out[0] = kSlaveAddr;
  out[1] = kFcWriteMultiple;
  put16(out + 2, addr);
  put16(out + 4, count);
  out[6] = (uint8_t)(2 * count);
  for (uint8_t i = 0; i < count; i++) put16(out + 7 + 2 * i, values[i]);
  const uint16_t crc = modbus_crc16(out, len - 2);
  out[len - 2] = (uint8_t)(crc & 0xFF);
  out[len - 1] = (uint8_t)(crc >> 8);
  return len;
}

Reply parseReply(const uint8_t req[kRequestLen], const uint8_t *buf, size_t len) {
  const uint8_t fc = req[1];
  const bool isRead = (fc == kFcReadHolding || fc == kFcReadInput);
  const uint16_t count = isRead ? get16(req + 4) : 0;

  size_t want;
  if (len >= 2 && buf[1] == (uint8_t)(fc | 0x80)) {
    want = 5;
  } else if (isRead) {
    want = 5 + 2 * (size_t)count;
  } else {
    want = kRequestLen;
  }
  if (len < want) return Reply{};
  if (len > want) return invalid();

  if (buf[0] != kSlaveAddr) return invalid();
  const uint16_t crc = modbus_crc16(buf, want - 2);
  if (buf[want - 2] != (uint8_t)(crc & 0xFF) || buf[want - 1] != (uint8_t)(crc >> 8)) {
    return invalid();
  }

  Reply r;
  if (buf[1] == (uint8_t)(fc | 0x80)) {
    r.kind = ReplyKind::Exception;
    r.exception = buf[2];
    return r;
  }
  if (buf[1] != fc) return invalid();

  if (isRead) {
    if (count > kMaxReadRegs || buf[2] != 2 * count) return invalid();
    for (uint16_t i = 0; i < count; i++) r.regs[i] = get16(buf + 3 + 2 * i);
    r.count = (uint8_t)count;
    r.kind = ReplyKind::Ok;
    return r;
  }
  // FC06 echoes the whole request; FC10 echoes only slave, FC, start and count.
  if (memcmp(buf, req, fc == kFcWriteMultiple ? 6 : kRequestLen) != 0) return invalid();
  r.kind = ReplyKind::Ok;
  return r;
}

}  // namespace iheaterlink::policy
