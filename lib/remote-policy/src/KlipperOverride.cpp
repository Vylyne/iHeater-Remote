#include "KlipperOverride.h"

#include <math.h>

namespace iheaterlink::policy {

bool KlipperOverride::press(bool klipperSource) {
  if (!klipperSource || active_)
    return active_;
  active_ = true;
  hasBaseline_ = hasTarget_;
  baselineC_ = targetC_;
  return true;
}

bool KlipperOverride::target(float targetC) {
  targetC_ = targetC;
  hasTarget_ = true;
  if (!active_)
    return true;
  if (!hasBaseline_) {
    hasBaseline_ = true;
    baselineC_ = targetC;
    return false;
  }
  if (fabsf(targetC - baselineC_) < kSameTargetToleranceC)
    return false;
  active_ = false;
  hasBaseline_ = false;
  return true;
}

float KlipperOverride::end() {
  active_ = false;
  hasBaseline_ = false;
  return klipperTargetC();
}

void KlipperOverride::reset() {
  active_ = false;
  hasTarget_ = false;
  targetC_ = 0.0f;
  hasBaseline_ = false;
  baselineC_ = 0.0f;
}

}  // namespace iheaterlink::policy
