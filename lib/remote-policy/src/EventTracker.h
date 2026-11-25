#pragma once

#include <stddef.h>
#include <stdint.h>

#include "EventLog.h"
#include "UartPollPolicy.h"

namespace iheaterlink::policy {

enum class WifiState : uint8_t { Connecting, Joined, Lost, Ap };
enum class MoonState : uint8_t { Off, Disconnected, Connecting, NotReady, Ready };

struct TrackedState {
  WifiState wifi = WifiState::Connecting;
  MoonState moon = MoonState::Off;  // Off while the source is Manual
  bool paired = false;              // MoonrakerLink::heaterObject() non-empty
  bool uart = false;                // the fields below apply only with UART output
  LinkStatus link = LinkStatus::Waiting;
  bool hasSnapshot = false;
  bool protocolSupported = true;
  uint16_t proto = 0;
  uint8_t fault = 0;  // snap.activeFault
  uint32_t ihUptimeS = 0;
  uint16_t resetCause = 0;
  ClearPhase clear = ClearPhase::Idle;
  bool lastClearOk = false;
};

struct TrackedEvent {
  EventKind kind;
  char text[kEventTextMax + 1];
};

class EventTracker {
 public:
  // Compares `now` with the previous call (the first call compares with a
  // default TrackedState) and writes up to `max` events, in the order of the
  // rules in the design. Returns how many were written.
  size_t update(const TrackedState &now, TrackedEvent *out, size_t max);

 private:
  TrackedState prev_;
};

// Text for a target change applied with the given source string (the `source`
// argument of applyMode / applyIntegrationTarget). False when the change is
// not worth an event ("boot").
bool targetEventText(const char *source, float targetC, char out[kEventTextMax + 1]);

// A fixed text for MoonrakerLink::lastError() at unpair time. Never copies the
// input, so no reason can carry the pairing token into the event log.
const char *unpairReasonText(const char *error);

}  // namespace iheaterlink::policy
