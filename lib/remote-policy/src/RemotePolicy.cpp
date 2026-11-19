#include "RemotePolicy.h"

namespace iheaterlink::policy {

bool validRange(int minC, int maxC) {
  return minC >= kRangeFloorC && minC <= maxC && maxC <= kRangeCeilC;
}

float clampTargetC(float requestedC, TargetRange r) {
  if (requestedC <= 0.0f) {
    return 0.0f;
  }
  if (requestedC < r.minC) {
    return r.minC;
  }
  if (requestedC > r.maxC) {
    return r.maxC;
  }
  return requestedC;
}

float resolveMoonrakerTargetC(const MoonrakerInputs &in, float autoTargetC,
                              uint8_t bedThresholdC, TargetRange r) {
  if (in.virtualChamberAvailable) {
    return clampTargetC(in.chamberTargetC, r);
  }
  if (in.bedTargetC >= bedThresholdC) {
    return clampTargetC(autoTargetC, r);
  }
  return 0.0f;
}

bool moonrakerStale(uint32_t nowMs, uint32_t lastUpdateMs, uint32_t timeoutMs) {
  return static_cast<uint32_t>(nowMs - lastUpdateMs) >= timeoutMs;
}

}  // namespace iheaterlink::policy
