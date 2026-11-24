#pragma once

#include <stddef.h>
#include <stdint.h>

namespace iheaterlink::policy {

enum class EventKind : uint8_t { Info, Action, Warn, Fault, Error };

// Lower-case name of a kind: "info", "action", "warn", "fault" or "error".
const char *eventKindName(EventKind k);

constexpr size_t kEventTextMax = 64;  // bytes, excluding the NUL
constexpr size_t kEventCapacity = 64;
constexpr uint32_t kEventCollapseMs = 60000;

struct Event {
  uint32_t seq = 0;  // from 1; 0 = none
  uint32_t ms = 0;   // millis() of the latest occurrence
  EventKind kind = EventKind::Info;
  uint16_t count = 1;
  char text[kEventTextMax + 1] = {};
};

// Copies src into out (kEventTextMax + 1 bytes), cutting at kEventTextMax
// bytes without splitting a UTF-8 sequence. Always NUL-terminates.
void copyEventText(char out[kEventTextMax + 1], const char *src);

// Fixed ring of the last kEventCapacity events. No heap.
class EventLog {
 public:
  // Adds an event, or collapses it into the newest entry with the same kind
  // and text when nowMs - entry.ms <= kEventCollapseMs (unsigned). Returns
  // the stored entry (new, or the bumped one with its original seq).
  const Event &add(EventKind kind, const char *text, uint32_t nowMs);
  size_t size() const { return size_; }
  const Event &at(size_t i) const;  // 0 = oldest

 private:
  Event ring_[kEventCapacity];
  size_t head_ = 0;  // slot of the oldest entry
  size_t size_ = 0;
  uint32_t nextSeq_ = 1;
};

}  // namespace iheaterlink::policy
