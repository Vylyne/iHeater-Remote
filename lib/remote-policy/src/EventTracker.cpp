#include "EventTracker.h"

#include <stdio.h>
#include <string.h>

#include "UartRegisters.h"

namespace iheaterlink::policy {

namespace {

struct Sink {
  TrackedEvent *out;
  size_t max;
  size_t n = 0;

  void add(EventKind kind, const char *text) {
    if (n >= max || out == nullptr) return;
    out[n].kind = kind;
    copyEventText(out[n].text, text);
    n++;
  }
};

bool startsWith(const char *s, const char *prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

}  // namespace

size_t EventTracker::update(const TrackedState &now, TrackedEvent *out, size_t max) {
  const TrackedState &p = prev_;
  Sink sink{out, max};
  char buf[96];

  if (now.wifi != p.wifi) {
    if (now.wifi == WifiState::Joined) {
      sink.add(EventKind::Info, "Wi-Fi connected");
    } else if (now.wifi == WifiState::Lost && p.wifi == WifiState::Joined) {
      sink.add(EventKind::Warn, "Wi-Fi lost");
    } else if (now.wifi == WifiState::Ap) {
      sink.add(EventKind::Warn, "Hotspot started");
    }
  }

  if (now.moon != p.moon) {
    const bool wasUp = p.moon == MoonState::Ready || p.moon == MoonState::NotReady;
    if (now.moon == MoonState::Ready) {
      sink.add(EventKind::Info, "Moonraker connected, Klipper ready");
    } else if (now.moon == MoonState::NotReady) {
      sink.add(EventKind::Warn, "Klipper not ready");
    } else if (wasUp &&
               (now.moon == MoonState::Disconnected || now.moon == MoonState::Connecting)) {
      sink.add(EventKind::Warn, "Moonraker disconnected");
    }
  }

  if (now.paired && !p.paired) sink.add(EventKind::Info, "Paired with Klipper");

  if (now.uart) {
    if (now.link != p.link) {
      if (now.link == LinkStatus::Up) {
        sink.add(EventKind::Info, "iHeater link up");
      } else if (now.link == LinkStatus::NoReply) {
        sink.add(EventKind::Error, "iHeater not replying");
      } else if (now.link == LinkStatus::Updating) {
        sink.add(EventKind::Info, "iHeater in its bootloader");
      }
    }

    if (now.hasSnapshot) {
      if (!now.protocolSupported && p.protocolSupported) {
        snprintf(buf, sizeof buf, "iHeater link protocol %u not supported",
                 static_cast<unsigned>(now.proto));
        sink.add(EventKind::Error, buf);
      }
      if (now.fault != p.fault) {
        if (now.fault != 0) {
          snprintf(buf, sizeof buf, "iHeater fault: %s", faultName(now.fault));
          sink.add(EventKind::Fault, buf);
        } else {
          sink.add(EventKind::Info, "iHeater fault cleared");
        }
      }
      if (p.hasSnapshot && now.ihUptimeS < p.ihUptimeS) {
        snprintf(buf, sizeof buf, "iHeater restarted (reset cause 0x%04X)",
                 static_cast<unsigned>(now.resetCause));
        sink.add(EventKind::Warn, buf);
      }
    }

    if (now.clear != p.clear) {
      if (now.clear == ClearPhase::AwaitRestart && p.clear == ClearPhase::Idle) {
        sink.add(EventKind::Action, "Fault clear sent");
      } else if (now.clear == ClearPhase::Idle && p.clear == ClearPhase::AwaitRestart) {
        if (now.lastClearOk) {
          sink.add(EventKind::Info, "Fault clear worked");
        } else {
          sink.add(EventKind::Warn, "Fault clear did not work");
        }
      } else if (now.clear == ClearPhase::Blocked) {
        sink.add(EventKind::Error, "iHeater could not clear the fault: power-cycle it");
      }
    }
  }

  prev_ = now;
  return sink.n;
}

bool targetEventText(const char *source, float targetC, char out[kEventTextMax + 1]) {
  if (source == nullptr) source = "";
  if (strcmp(source, "boot") == 0) return false;

  const char *who = source;
  if (strcmp(source, "web") == 0 || strcmp(source, "button:click") == 0 ||
      strcmp(source, "button:long") == 0) {
    who = "manual press";
  } else if (strcmp(source, "klipper") == 0) {
    who = "Klipper";
  } else if (strcmp(source, "timer") == 0) {
    who = "timer ended";
  } else if (strcmp(source, "timer: back to klipper") == 0) {
    who = "timer ended, back to Klipper";
  } else if (strcmp(source, "back to klipper") == 0) {
    who = "back to Klipper";
  }

  char buf[160];
  if (targetC == 0.0f) {
    snprintf(buf, sizeof buf, "Heat off (%s)", who);
  } else {
    char t[24];
    snprintf(t, sizeof t, "%.1f", static_cast<double>(targetC));
    const size_t len = strlen(t);
    if (len >= 2 && strcmp(t + len - 2, ".0") == 0) t[len - 2] = '\0';
    snprintf(buf, sizeof buf, "Target %s \xC2\xB0" "C (%s)", t, who);
  }
  copyEventText(out, buf);
  return true;
}

const char *unpairReasonText(const char *error) {
  if (error == nullptr) error = "";
  if (startsWith(error, "not paired")) return "Not paired: no heater has this Remote's token";
  if (strstr(error, "more than one") != nullptr) return "Pairing token used by more than one heater";
  if (startsWith(error, "no heater_iheater_remote")) return "No heater_iheater_remote in Klipper";
  return "Unpaired from Klipper";
}

}  // namespace iheaterlink::policy
