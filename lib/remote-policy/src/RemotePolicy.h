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

// --- Wi-Fi supervisor -------------------------------------------------------
constexpr uint32_t kStaReconnectMs = 30000;
constexpr uint32_t kApRetryMs = 120000;

enum class WifiAction : uint8_t { None, Reconnect, RetryFromAp };

// Station mode: reconnect every kStaReconnectMs while down. AP fallback with
// saved credentials: retry the saved network every kApRetryMs.
WifiAction wifiSupervisorAction(bool apMode, bool hasCredentials,
                                bool staConnected, uint32_t nowMs,
                                uint32_t lastAttemptMs);

// --- Printer link loss ------------------------------------------------------
// Backstop for a printer that never comes back: heat already commanded by the
// printer is held through short outages and cut after this long.
constexpr uint32_t kLinkLossGraceMs = 600000;

bool cutHeatForLinkLoss(bool printerSourceActive, bool heatingFromPrinter,
                        bool linkHealthy, uint32_t nowMs,
                        uint32_t linkDownSinceMs, uint32_t graceMs);

// idryer-core reports an all-default status (empty print_stats state) when
// the Moonraker socket drops; a real update always carries a state.
bool moonrakerStatusIsReset(const char *printerState);

// --- Wi-Fi TX power ---------------------------------------------------------
// Arduino wifi_power_t values (quarter-dBm): 8.5, 11, 13, 15, 17, 19.5 dBm.
bool validTxPower(int quarterDbm);

}  // namespace iheaterlink::policy
