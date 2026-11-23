#pragma once

namespace iheaterlink::policy {

// Two targets closer than this count as the same target.
constexpr float kSameTargetToleranceC = 0.05f;

// A manual press while following Klipper overrides Klipper's target without
// unbinding. The override lasts until Klipper's target changes from the one
// it had when the override began, or until it is ended.
class KlipperOverride {
 public:
  // A web or button press. Starts an override when klipperSource is true,
  // keeping the baseline of one already running. Returns active().
  bool press(bool klipperSource);
  // A target from Moonraker. Always recorded as Klipper's latest. True
  // when it must be applied now: no override, or a target that ends one.
  bool target(float targetC);
  // Ends any override (Back to Klipper, timer expiry, link-loss cut).
  // Returns Klipper's latest target, or 0 when none has arrived.
  float end();
  // Forgets the override and Klipper's latest target (unpaired, or the
  // source or binding changed).
  void reset();
  bool active() const { return active_; }
  bool hasKlipperTarget() const { return hasTarget_; }
  float klipperTargetC() const { return hasTarget_ ? targetC_ : 0.0f; }

 private:
  bool active_ = false;
  bool hasTarget_ = false;
  float targetC_ = 0.0f;
  bool hasBaseline_ = false;
  float baselineC_ = 0.0f;
};

}  // namespace iheaterlink::policy
