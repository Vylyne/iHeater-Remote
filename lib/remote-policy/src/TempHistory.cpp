#include "TempHistory.h"

namespace iheaterlink::policy {

void TempHistory::push(const HistSample &s) {
  if (size_ < kCapacity) {
    ring_[(head_ + size_) % kCapacity] = s;
    size_++;
  } else {
    ring_[head_] = s;
    head_ = (head_ + 1) % kCapacity;
  }
}

size_t TempHistory::tick(uint32_t nowMs, const HistSample &current) {
  if (size_ == 0) {
    push(current);
    newestMs_ = nowMs;
    return 1;
  }
  const uint32_t k = (uint32_t)(nowMs - newestMs_) / kPeriodMs;
  if (k == 0) return 0;
  uint32_t gaps = k - 1;
  if (gaps > kCapacity) gaps = kCapacity;
  const HistSample none;
  for (uint32_t i = 0; i < gaps; i++) push(none);
  push(current);
  newestMs_ += k * kPeriodMs;
  return (size_t)gaps + 1;
}

const HistSample &TempHistory::at(size_t i) const {
  return ring_[(head_ + i) % kCapacity];
}

}  // namespace iheaterlink::policy
