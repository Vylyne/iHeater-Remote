#pragma once

#include <stddef.h>
#include <stdint.h>

// The iHeater pulse frame, as RMT symbols. Pure, no Arduino dependency.
//
// A frame is a SYNC LOW, then one HIGH/LOW pair per count:
//   [SYNC LOW] -> [HIGH pulseHighUs | LOW pulseLowUs] x pulseCode
// The STM32 counts the rising edges after the SYNC (src/controller/RMT_PROTOCOL.md).
namespace iheaterlink::policy {

struct PulseTiming {
  uint32_t syncLowUs;
  uint32_t pulseHighUs;
  uint32_t pulseLowUs;
};

// One RMT symbol: two halves, each a level held for a duration in µs
// (one tick is 1 µs).
struct PulseSymbol {
  uint16_t duration0;
  uint8_t level0;
  uint16_t duration1;
  uint8_t level1;
};

// Each half has a 15-bit duration field, so it holds at most 2^15 - 1 ticks.
constexpr uint32_t kPulseFieldMaxUs = 32767;

// Fills out[] with the frame's symbols (no end marker). Returns the count,
// or 0 if the frame does not fit in capacity or a pulse width is 0 or over
// kPulseFieldMaxUs.
size_t buildPulseFrame(uint8_t pulseCode, const PulseTiming &timing,
                       PulseSymbol *out, size_t capacity);

}  // namespace iheaterlink::policy
