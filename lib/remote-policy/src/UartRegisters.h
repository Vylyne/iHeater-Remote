#pragma once

#include <stddef.h>
#include <stdint.h>

#include "RemotePolicy.h"

// Register decode and target-range helpers for the iHeater UART link.
// Pure, no Arduino dependency.
namespace iheaterlink::policy {

constexpr uint16_t kInputBase = 0x0000;
constexpr uint8_t kInputCount = 22;
constexpr uint16_t kSettingsBase = 0x0200;
constexpr uint8_t kSettingsCount = 10;
constexpr uint16_t kRegTarget = 0x0100;
constexpr uint16_t kRegClear = 0x0101;
constexpr uint16_t kRegBootloader = 0x0102, kBootloaderMagic = 0xB007;
constexpr uint16_t kSupportedProtocol = 1;  // link protocol this Remote speaks
constexpr int16_t kNoReading = INT16_MIN;  // wire 0x8000

enum class IheaterState : uint8_t { Booting = 0, Idle = 1, Heating = 2, Hold = 3, Fault = 4 };

// "booting", "idle", "heating", "hold", "fault"; anything else "unknown".
const char *iheaterStateName(IheaterState s);

struct IheaterSnapshot {
  uint16_t proto, fwVersion, fwPatch;
  IheaterState state;
  uint8_t activeFault;
  bool clearableNow;
  uint16_t clearAllowMask;
  uint8_t clearsLeft, lastClearResult;
  int16_t airT10, heaterT10, auxT10, airTargetT10, heaterSetT10;
  uint16_t pwmPermille;
  bool fanOn;
  uint32_t uptimeS;
  uint16_t linkLossCount, holdRemainingS, resetCause;
  uint8_t lastFault;
};
bool decodeInputs(const uint16_t *regs, uint8_t count, IheaterSnapshot &out);  // false unless count == 22

struct IheaterLimits {
  int16_t maxHeaterT10, maxAirT10, minTargetT10, maxAirTargetT10;
  uint16_t linkHoldS;
  int16_t deltaT10, fanOnT10, fanOffT10, fanOnActiveT10, clearMarginT10;
};
bool decodeSettings(const uint16_t *regs, uint8_t count, IheaterLimits &out);  // false unless count == 10

// Text for the UI. Codes 0x01-0x08 and 0xFF per the contract's fault table;
// anything else "unknown fault".
const char *faultName(uint8_t code);

struct RangeT10 {
  int16_t minT10;
  int16_t maxT10;
};
// Setup range (whole degrees C) intersected with the iHeater's [min, max]. False when empty.
bool effectiveRangeT10(TargetRange setup, int16_t ihMinT10, int16_t ihMaxT10, RangeT10 &out);
// <= 0 gives 0. Else round(c*10) clamped into the effective range; empty range gives 0.
uint16_t uartTargetT10(float requestedC, TargetRange setup, int16_t ihMinT10, int16_t ihMaxT10);

}  // namespace iheaterlink::policy
