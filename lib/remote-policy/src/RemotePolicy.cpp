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

WifiAction wifiSupervisorAction(bool apMode, bool hasCredentials,
                                bool staConnected, uint32_t nowMs,
                                uint32_t lastAttemptMs) {
  if (staConnected) {
    return WifiAction::None;
  }
  const uint32_t elapsed = static_cast<uint32_t>(nowMs - lastAttemptMs);
  if (!apMode) {
    return elapsed >= kStaReconnectMs ? WifiAction::Reconnect : WifiAction::None;
  }
  if (hasCredentials && elapsed >= kApRetryMs) {
    return WifiAction::RetryFromAp;
  }
  return WifiAction::None;
}

bool cutHeatForLinkLoss(bool printerSourceActive, bool heatingFromPrinter,
                        bool linkHealthy, uint32_t nowMs,
                        uint32_t linkDownSinceMs, uint32_t graceMs) {
  if (!printerSourceActive || !heatingFromPrinter || linkHealthy) {
    return false;
  }
  return static_cast<uint32_t>(nowMs - linkDownSinceMs) >= graceMs;
}

bool moonrakerStatusIsReset(const char *printerState) {
  return printerState == nullptr || printerState[0] == '\0';
}

bool validTxPower(int quarterDbm) {
  switch (quarterDbm) {
  case 34:
  case 44:
  case 52:
  case 60:
  case 68:
  case 78:
    return true;
  default:
    return false;
  }
}

}  // namespace iheaterlink::policy
