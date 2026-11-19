#pragma once

#include <stdint.h>

// Pure target-selection policy for iHeater Remote. No Arduino dependency.
namespace iheaterlink::policy {

constexpr uint8_t kRangeFloorC = 11;  // pulse codes 0 and 10 mean off
constexpr uint8_t kRangeCeilC = 90;   // stock iHeater MAX_AIR_TEMP
constexpr uint8_t kDefaultMinC = 45;  // stock iHeater PULSE_MIN_TARGET
constexpr uint8_t kDefaultMaxC = 85;
constexpr uint32_t kMoonrakerStaleMs = 30000;

struct TargetRange {
  uint8_t minC;
  uint8_t maxC;
};

// kRangeFloorC <= minC <= maxC <= kRangeCeilC
bool validRange(int minC, int maxC);

// <= 0 means off (returns 0); otherwise clamped to [r.minC, r.maxC].
float clampTargetC(float requestedC, TargetRange r);

struct MoonrakerInputs {
  bool virtualChamberAvailable;
  float chamberTargetC;
  float bedTargetC;
};

// Klipper's VIRTUAL_CHAMBER target wins when available (0 = off); otherwise
// the bed-setpoint rule applies.
float resolveMoonrakerTargetC(const MoonrakerInputs &in, float autoTargetC,
                              uint8_t bedThresholdC, TargetRange r);

// Wrap-safe: true once timeoutMs has elapsed since lastUpdateMs.
bool moonrakerStale(uint32_t nowMs, uint32_t lastUpdateMs, uint32_t timeoutMs);

}  // namespace iheaterlink::policy
