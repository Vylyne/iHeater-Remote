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

constexpr size_t kTicketLen = 16;
// Free heap that must remain after the console ring is allocated.
constexpr uint32_t kConsoleHeapFloor = 40960;

// One-time tickets that let the logged-in /diag page subscribe to the console
// over the socket. Tickets are secrets like session tokens: never log or
// return them. Not thread-safe; the caller serializes access.
class ConsoleTickets {
 public:
  static constexpr size_t kSlots = 4;
  static constexpr uint32_t kLifetimeMs = 60000;
  void issue(const uint8_t ticket[kTicketLen], uint32_t nowMs);   // a full table drops the oldest
  bool redeem(const uint8_t ticket[kTicketLen], uint32_t nowMs);  // once, within the lifetime

 private:
  struct Slot {
    bool used = false;
    uint32_t issuedMs = 0;
    uint32_t order = 0;  // issue counter, for dropping the oldest
    uint8_t ticket[kTicketLen] = {};
  };
  Slot slots_[kSlots];
  uint32_t counter_ = 0;
};

// Exactly 32 hex digits (either case) into 16 bytes.
bool parseTicketHex(const char *text, uint8_t out[kTicketLen]);

// True when freeHeap >= ringBytes + kConsoleHeapFloor.
bool consoleHeapOk(uint32_t freeHeap, size_t ringBytes);

}  // namespace iheaterlink::policy
