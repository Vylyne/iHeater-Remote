#pragma once

#include <stdint.h>

#include "UartPollPolicy.h"

// Send schedule for the Remote's readings reports to Klipper. Pure decisions,
// no Arduino dependency. The caller builds ReportInputs from the poll policy
// and its follow target, asks due(), sends, then feeds the outcome back.
namespace iheaterlink::policy {

constexpr uint32_t kReportMinGapMs = 1000;
constexpr uint32_t kReportActiveMs = 5000;
constexpr uint32_t kReportIdleMs = 15000;

enum class PluginState : uint8_t { Unknown, Found, Missing };
const char *pluginStateName(PluginState p);  // "unknown", "found", "missing"

struct ReportInputs {
  LinkStatus link;
  bool hasSnapshot;
  IheaterState state;
  int16_t airT10, heaterT10;  // may be kNoReading
  uint16_t pwmPermille;
  bool fanOn;
  uint8_t fault;
  uint16_t targetT10;  // the target the Remote follows; 0 = off
};

struct ReportValues {
  bool hasAir, hasElement, hasPower;
  int16_t airT10, elementT10;
  uint16_t powerPermille;
  uint32_t nextWithinS;  // 5 or 15
};

ReportValues reportValues(const ReportInputs &in);
uint32_t reportIntervalMs(const ReportInputs &in);  // kReportActiveMs or kReportIdleMs

class ReportScheduler {
 public:
  bool due(const ReportInputs &in, uint32_t nowMs) const;
  void onSent(const ReportInputs &in, uint32_t nowMs);  // sent++, in flight
  void onAccepted();                                    // plugin Found, not in flight
  void onRefused();                                     // refused++, not in flight
  void onMethodNotFound();                              // plugin Missing, not in flight
  void onFailed();                                      // failed++, not in flight
  // The next due() ignores change and heartbeat, but not the minimum gap, in
  // flight or Missing.
  void forceNext();
  // New connection: forget the last send, in flight and Missing; keep the counters.
  void reset();

  PluginState plugin() const { return plugin_; }
  bool inFlight() const { return inFlight_; }
  uint32_t sent() const { return sent_; }
  uint32_t refused() const { return refused_; }
  uint32_t failed() const { return failed_; }

 private:
  bool haveSent_ = false;
  bool force_ = false;
  bool inFlight_ = false;
  PluginState plugin_ = PluginState::Unknown;
  uint32_t lastSentMs_ = 0;
  ReportInputs lastIn_{};
  ReportValues lastValues_{};
  uint32_t sent_ = 0, refused_ = 0, failed_ = 0;
};

}  // namespace iheaterlink::policy
