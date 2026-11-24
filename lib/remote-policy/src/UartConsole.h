#pragma once

#include <stddef.h>
#include <stdint.h>

// Record ring and one-line decoder behind the live Modbus RTU console.
// Pure, no Arduino dependency, no heap: the ring uses caller-owned storage.
namespace iheaterlink::policy {

enum class ConsoleResult : uint8_t { Ok, Exception, Timeout, BadReply };

constexpr size_t kConsoleReqBytes = 32, kConsoleReplyBytes = 64, kConsoleLines = 100;

struct ConsoleRecord {
  uint32_t seq = 0, ms = 0;
  ConsoleResult result = ConsoleResult::Ok;
  uint8_t code = 0;                   // exception code for Exception
  uint16_t reqLen = 0, replyLen = 0;  // full lengths; the bytes below are cut
  uint8_t req[kConsoleReqBytes] = {};
  uint8_t reply[kConsoleReplyBytes] = {};
};

// Fills a record from one exchange (copies at most 32 / 64 bytes).
void makeConsoleRecord(ConsoleRecord &r, const uint8_t *req, size_t reqLen, const uint8_t *reply,
                       size_t replyLen, ConsoleResult result, uint8_t code, uint32_t ms);

// Ring over caller-owned storage. One writer, one reader; not thread-safe
// (the firmware holds its spinlock around each call).
class ConsoleRing {
 public:
  ConsoleRing(ConsoleRecord *storage, size_t capacity);
  void push(const ConsoleRecord &r);  // assigns seq (from 1)
  // Copies up to `max` records the reader has not seen, oldest first.
  // `dropped` = records overwritten before the reader saw them (0 if none).
  size_t drain(ConsoleRecord *out, size_t max, uint32_t &dropped);

 private:
  ConsoleRecord *storage_;
  size_t capacity_;
  uint32_t written_ = 0;  // records pushed so far; also the newest seq
  uint32_t read_ = 0;     // newest seq the reader has taken
};

// "<request> → <reply>", NUL-terminated, cut to outLen - 1 bytes.
void describeExchange(const ConsoleRecord &r, char *out, size_t outLen);

}  // namespace iheaterlink::policy
