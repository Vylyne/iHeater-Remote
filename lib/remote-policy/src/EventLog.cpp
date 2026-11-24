#include "EventLog.h"

#include <string.h>

namespace iheaterlink::policy {

const char *eventKindName(EventKind k) {
  switch (k) {
    case EventKind::Info: return "info";
    case EventKind::Action: return "action";
    case EventKind::Warn: return "warn";
    case EventKind::Fault: return "fault";
    case EventKind::Error: return "error";
  }
  return "info";
}

void copyEventText(char out[kEventTextMax + 1], const char *src) {
  if (!src) src = "";
  size_t n = strnlen(src, kEventTextMax + 1);
  if (n > kEventTextMax) {
    n = kEventTextMax;
    // If the cut lands before a continuation byte (10xxxxxx), step back to
    // the lead byte and drop that whole sequence.
    if ((static_cast<uint8_t>(src[n]) & 0xC0) == 0x80) {
      while (n > 0 && (static_cast<uint8_t>(src[n]) & 0xC0) == 0x80) n--;
    }
  }
  memcpy(out, src, n);
  out[n] = '\0';
}

const Event &EventLog::at(size_t i) const {
  return ring_[(head_ + i) % kEventCapacity];
}

const Event &EventLog::add(EventKind kind, const char *text, uint32_t nowMs) {
  char cut[kEventTextMax + 1];
  copyEventText(cut, text);

  for (size_t i = size_; i > 0; i--) {
    Event &e = ring_[(head_ + i - 1) % kEventCapacity];
    if (e.kind == kind && strcmp(e.text, cut) == 0) {
      if (static_cast<uint32_t>(nowMs - e.ms) <= kEventCollapseMs) {
        if (e.count < UINT16_MAX) e.count++;
        e.ms = nowMs;
        return e;
      }
      break;  // newest match is too old; start a new entry
    }
  }

  size_t slot;
  if (size_ < kEventCapacity) {
    slot = (head_ + size_) % kEventCapacity;
    size_++;
  } else {
    slot = head_;
    head_ = (head_ + 1) % kEventCapacity;
  }
  Event &e = ring_[slot];
  e.seq = nextSeq_++;
  e.ms = nowMs;
  e.kind = kind;
  e.count = 1;
  memcpy(e.text, cut, sizeof(e.text));
  return e;
}

}  // namespace iheaterlink::policy
