#include "UartRegisters.h"

#include <math.h>

namespace iheaterlink::policy {

bool decodeInputs(const uint16_t *regs, uint8_t count, IheaterSnapshot &out) {
  if (regs == nullptr || count != kInputCount) return false;
  out.proto = regs[0x00];
  out.fwVersion = regs[0x01];
  out.fwPatch = regs[0x02];
  out.state = static_cast<IheaterState>(regs[0x03] & 0xFF);
  out.activeFault = static_cast<uint8_t>(regs[0x04]);
  out.clearableNow = regs[0x05] != 0;
  out.clearAllowMask = regs[0x06];
  out.clearsLeft = static_cast<uint8_t>(regs[0x07]);
  out.lastClearResult = static_cast<uint8_t>(regs[0x08]);
  out.airT10 = static_cast<int16_t>(regs[0x09]);
  out.heaterT10 = static_cast<int16_t>(regs[0x0A]);
  out.auxT10 = static_cast<int16_t>(regs[0x0B]);
  out.airTargetT10 = static_cast<int16_t>(regs[0x0C]);
  out.heaterSetT10 = static_cast<int16_t>(regs[0x0D]);
  out.pwmPermille = regs[0x0E];
  out.fanOn = regs[0x0F] != 0;
  out.uptimeS = static_cast<uint32_t>(regs[0x10]) | (static_cast<uint32_t>(regs[0x11]) << 16);
  out.linkLossCount = regs[0x12];
  out.holdRemainingS = regs[0x13];
  out.resetCause = regs[0x14];
  out.lastFault = static_cast<uint8_t>(regs[0x15]);
  return true;
}

bool decodeSettings(const uint16_t *regs, uint8_t count, IheaterLimits &out) {
  if (regs == nullptr || count != kSettingsCount) return false;
  out.maxHeaterT10 = static_cast<int16_t>(regs[0]);
  out.maxAirT10 = static_cast<int16_t>(regs[1]);
  out.minTargetT10 = static_cast<int16_t>(regs[2]);
  out.maxAirTargetT10 = static_cast<int16_t>(regs[3]);
  out.linkHoldS = regs[4];
  out.deltaT10 = static_cast<int16_t>(regs[5]);
  out.fanOnT10 = static_cast<int16_t>(regs[6]);
  out.fanOffT10 = static_cast<int16_t>(regs[7]);
  out.fanOnActiveT10 = static_cast<int16_t>(regs[8]);
  out.clearMarginT10 = static_cast<int16_t>(regs[9]);
  return true;
}

const char *faultName(uint8_t code) {
  switch (code) {
    case 0x01: return "air did not reach target in time";
    case 0x02: return "heater not responding";
    case 0x03: return "manual shutdown";
    case 0x04: return "air thermistor open";
    case 0x05: return "heater thermistor open";
    case 0x06: return "heater over-temperature";
    case 0x07: return "air over-temperature";
    case 0x08: return "link lost";
    case 0xFF: return "unknown";
    default: return "unknown fault";
  }
}

bool effectiveRangeT10(TargetRange setup, int16_t ihMinT10, int16_t ihMaxT10, RangeT10 &out) {
  int lo = static_cast<int>(setup.minC) * 10;
  int hi = static_cast<int>(setup.maxC) * 10;
  if (ihMinT10 > lo) lo = ihMinT10;
  if (ihMaxT10 < hi) hi = ihMaxT10;
  if (lo > hi) return false;
  out.minT10 = static_cast<int16_t>(lo);
  out.maxT10 = static_cast<int16_t>(hi);
  return true;
}

uint16_t uartTargetT10(float requestedC, TargetRange setup, int16_t ihMinT10, int16_t ihMaxT10) {
  if (!(requestedC > 0.0f)) return 0;
  RangeT10 r;
  if (!effectiveRangeT10(setup, ihMinT10, ihMaxT10, r)) return 0;
  float t = roundf(requestedC * 10.0f);
  if (t < r.minT10) t = r.minT10;
  if (t > r.maxT10) t = r.maxT10;
  return static_cast<uint16_t>(t);
}

}  // namespace iheaterlink::policy
