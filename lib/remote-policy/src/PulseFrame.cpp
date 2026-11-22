#include "PulseFrame.h"

namespace iheaterlink::policy {

namespace {
uint32_t takeField(uint32_t &remaining) {
  const uint32_t d = remaining > kPulseFieldMaxUs ? kPulseFieldMaxUs : remaining;
  remaining -= d;
  return d;
}
}  // namespace

size_t buildPulseFrame(uint8_t pulseCode, const PulseTiming &timing,
                       PulseSymbol *out, size_t capacity) {
  if (timing.pulseHighUs == 0 || timing.pulseHighUs > kPulseFieldMaxUs ||
      timing.pulseLowUs == 0 || timing.pulseLowUs > kPulseFieldMaxUs)
    return 0;

  // The SYNC is longer than one field (120 ms by default), so it spans
  // several symbols, each with both halves low and up to 2 x 32 767 µs.
  const uint32_t perSymbol = 2 * kPulseFieldMaxUs;
  const size_t syncSymbols = (timing.syncLowUs + perSymbol - 1) / perSymbol;
  // Refuse rather than truncate: fewer pulses would be read as a
  // different, valid temperature.
  if (out == nullptr || syncSymbols + pulseCode > capacity)
    return 0;

  size_t n = 0;
  uint32_t remaining = timing.syncLowUs;
  while (remaining > 0) {
    PulseSymbol &s = out[n++];
    s.level0 = 0;
    s.duration0 = static_cast<uint16_t>(takeField(remaining));
    s.level1 = 0;
    s.duration1 = static_cast<uint16_t>(takeField(remaining));
  }

  for (uint8_t i = 0; i < pulseCode; ++i) {
    PulseSymbol &s = out[n++];
    s.level0 = 1;
    s.duration0 = static_cast<uint16_t>(timing.pulseHighUs);
    s.level1 = 0;
    s.duration1 = static_cast<uint16_t>(timing.pulseLowUs);
  }
  return n;
}

}  // namespace iheaterlink::policy
