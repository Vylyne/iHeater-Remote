#pragma once

#include <stddef.h>
#include <stdint.h>

#include "UartRegisters.h"

namespace iheaterlink::policy {

// One graph sample in tenths of a degree. kNoReading = no data.
struct HistSample {
  int16_t airT10 = kNoReading;
  int16_t heaterT10 = kNoReading;
  int16_t targetT10 = kNoReading;
};

// Fixed ring of the last kCapacity samples, one per kPeriodMs. No heap.
class TempHistory {
 public:
  static constexpr size_t kCapacity = 360;
  static constexpr uint32_t kPeriodMs = 10000;

  // The first call samples at once. Later calls sample when at least
  // kPeriodMs has passed since the newest sample; k whole periods passed
  // add k-1 no-data samples (at most kCapacity), then `current`. The newest
  // sample's time stays on the 10 s grid (newest + k * kPeriodMs).
  // Returns the number of samples added. Unsigned arithmetic throughout, so
  // it survives the millis() wrap.
  size_t tick(uint32_t nowMs, const HistSample &current);
  size_t size() const { return size_; }
  const HistSample &at(size_t i) const;  // 0 = oldest
  uint32_t newestMs() const { return newestMs_; }

 private:
  void push(const HistSample &s);

  HistSample ring_[kCapacity];
  size_t head_ = 0;  // slot of the oldest sample
  size_t size_ = 0;
  uint32_t newestMs_ = 0;
};

}  // namespace iheaterlink::policy
