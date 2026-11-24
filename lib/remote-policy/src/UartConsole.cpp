#include "UartConsole.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include "modbus_rtu.h"
}

#include "UartFrames.h"
#include "UartRegisters.h"

namespace iheaterlink::policy {

namespace {

constexpr size_t kLineMax = 192;

// UTF-8 pieces, kept as escapes so the source stays plain ASCII.
#define ARROW "\xE2\x86\x92"  // U+2192
#define ENDASH "\xE2\x80\x93"  // U+2013
#define EMDASH "\xE2\x80\x94"  // U+2014
#define TIMES "\xC3\x97"       // U+00D7
#define DEGREE "\xC2\xB0"      // U+00B0

uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

// Appends formatted text, never past the end of buf.
void appendf(char *buf, size_t cap, size_t &pos, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void appendf(char *buf, size_t cap, size_t &pos, const char *fmt, ...) {
  if (pos >= cap - 1) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + pos, cap - pos, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  pos += (size_t)n;
  if (pos > cap - 1) pos = cap - 1;
}

void formatTemp(char *dst, size_t cap, int16_t t10) {
  if (t10 == kNoReading) {
    snprintf(dst, cap, EMDASH);
    return;
  }
  int v = t10 < 0 ? -t10 : t10;
  snprintf(dst, cap, "%s%d.%d " DEGREE "C", t10 < 0 ? "-" : "", v / 10, v % 10);
}

void describeRequest(const ConsoleRecord &r, char *buf, size_t cap, size_t &pos) {
  const uint8_t *q = r.req;
  size_t n = r.reqLen < kConsoleReqBytes ? r.reqLen : kConsoleReqBytes;
  if (n < 6) {
    appendf(buf, cap, pos, "request of %u bytes", (unsigned)r.reqLen);
    return;
  }
  uint8_t fc = q[1];
  uint16_t addr = be16(q + 2), val = be16(q + 4);
  switch (fc) {
    case kFcReadInput:
    case kFcReadHolding:
      appendf(buf, cap, pos, "read %s 0x%04X" ENDASH "0x%04X",
              fc == kFcReadInput ? "inputs" : "holding", (unsigned)addr,
              (unsigned)(uint16_t)(addr + val - 1));
      return;
    case kFcWriteSingle:
      if (addr == kRegBootloader && val == kBootloaderMagic) {
        appendf(buf, cap, pos, "enter bootloader");
      } else if (addr == kRegCommand && val == kCmdErase) {
        appendf(buf, cap, pos, "erase");
      } else if (addr == kRegCommand && val == kCmdBoot) {
        appendf(buf, cap, pos, "boot");
      } else {
        appendf(buf, cap, pos, "write 0x%04X = %u", (unsigned)addr, (unsigned)val);
      }
      return;
    case kFcWriteMultiple:
      if (addr == kRegChunk && n >= 9) {
        appendf(buf, cap, pos, "chunk %u", (unsigned)be16(q + 7));
      } else {
        appendf(buf, cap, pos, "write 0x%04X " TIMES "%u", (unsigned)addr, (unsigned)val);
      }
      return;
    default:
      appendf(buf, cap, pos, "FC 0x%02X at 0x%04X", (unsigned)fc, (unsigned)addr);
      return;
  }
}

void describeReply(const ConsoleRecord &r, char *buf, size_t cap, size_t &pos) {
  switch (r.result) {
    case ConsoleResult::Timeout:
      appendf(buf, cap, pos, "no reply");
      return;
    case ConsoleResult::Exception:
      appendf(buf, cap, pos, "exception 0x%02X", (unsigned)r.code);
      return;
    case ConsoleResult::BadReply:
      if (r.replyLen > kConsoleReplyBytes) {
        appendf(buf, cap, pos, "reply too long");
      } else if (r.replyLen >= 4 &&
                 modbus_crc16(r.reply, r.replyLen - 2) !=
                     (uint16_t)(r.reply[r.replyLen - 2] | (r.reply[r.replyLen - 1] << 8))) {
        appendf(buf, cap, pos, "bad CRC");
      } else {
        appendf(buf, cap, pos, "reply does not match");
      }
      return;
    case ConsoleResult::Ok:
      break;
  }
  bool isRead = r.reqLen >= 6 && (r.req[1] == kFcReadInput || r.req[1] == kFcReadHolding);
  if (!isRead) {
    appendf(buf, cap, pos, "ok");
    return;
  }
  uint16_t qty = be16(r.req + 4);
  if (r.req[1] == kFcReadInput && be16(r.req + 2) == kInputBase && qty == kInputCount &&
      r.reqLen >= kRequestLen && r.replyLen <= kConsoleReplyBytes) {
    Reply p = parseReply(r.req, r.reply, r.replyLen);
    IheaterSnapshot s;
    if (p.kind == ReplyKind::Ok && decodeInputs(p.regs, p.count, s)) {
      char air[24], heater[24];
      formatTemp(air, sizeof air, s.airT10);
      formatTemp(heater, sizeof heater, s.heaterT10);
      appendf(buf, cap, pos, "proto %u, air %s, heater %s, PWM %u%%, fault %s", (unsigned)s.proto,
              air, heater, (unsigned)(s.pwmPermille / 10),
              s.activeFault == 0 ? "none" : faultName(s.activeFault));
      return;
    }
  }
  appendf(buf, cap, pos, "ok, %u registers", (unsigned)qty);
}

}  // namespace

void makeConsoleRecord(ConsoleRecord &r, const uint8_t *req, size_t reqLen, const uint8_t *reply,
                       size_t replyLen, ConsoleResult result, uint8_t code, uint32_t ms) {
  r = ConsoleRecord();
  r.ms = ms;
  r.result = result;
  r.code = code;
  r.reqLen = (uint16_t)(reqLen > 0xFFFF ? 0xFFFF : reqLen);
  r.replyLen = (uint16_t)(replyLen > 0xFFFF ? 0xFFFF : replyLen);
  size_t nq = reqLen < kConsoleReqBytes ? reqLen : kConsoleReqBytes;
  size_t nr = replyLen < kConsoleReplyBytes ? replyLen : kConsoleReplyBytes;
  if (req && nq) memcpy(r.req, req, nq);
  if (reply && nr) memcpy(r.reply, reply, nr);
}

ConsoleRing::ConsoleRing(ConsoleRecord *storage, size_t capacity)
    : storage_(storage), capacity_(capacity) {}

void ConsoleRing::push(const ConsoleRecord &r) {
  if (!storage_ || capacity_ == 0) return;
  written_++;
  ConsoleRecord &slot = storage_[(written_ - 1) % capacity_];
  slot = r;
  slot.seq = written_;
}

size_t ConsoleRing::drain(ConsoleRecord *out, size_t max, uint32_t &dropped) {
  dropped = 0;
  if (!storage_ || capacity_ == 0) return 0;
  uint32_t oldest = written_ > capacity_ ? written_ - (uint32_t)capacity_ + 1 : 1;
  if (read_ + 1 < oldest) {
    dropped = oldest - (read_ + 1);
    read_ = oldest - 1;
  }
  size_t n = 0;
  while (n < max && read_ < written_) {
    read_++;
    out[n++] = storage_[(read_ - 1) % capacity_];
  }
  return n;
}

void describeExchange(const ConsoleRecord &r, char *out, size_t outLen) {
  if (!out || outLen == 0) return;
  char buf[kLineMax];
  size_t pos = 0;
  buf[0] = 0;
  describeRequest(r, buf, sizeof buf, pos);
  appendf(buf, sizeof buf, pos, " " ARROW " ");
  describeReply(r, buf, sizeof buf, pos);
  size_t n = pos < outLen - 1 ? pos : outLen - 1;
  memcpy(out, buf, n);
  out[n] = 0;
}

}  // namespace iheaterlink::policy
