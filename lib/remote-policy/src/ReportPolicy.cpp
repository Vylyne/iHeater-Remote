#include "ReportPolicy.h"

namespace iheaterlink::policy {

const char *pluginStateName(PluginState p) {
  switch (p) {
    case PluginState::Unknown: return "unknown";
    case PluginState::Found: return "found";
    case PluginState::Missing: return "missing";
  }
  return "unknown";
}

static bool isActive(const ReportInputs &in) {
  return in.targetT10 > 0 ||
         (in.hasSnapshot && (in.state == IheaterState::Heating || in.state == IheaterState::Hold));
}

uint32_t reportIntervalMs(const ReportInputs &in) {
  return isActive(in) ? kReportActiveMs : kReportIdleMs;
}

ReportValues reportValues(const ReportInputs &in) {
  ReportValues v{};
  const bool live = in.link == LinkStatus::Up && in.hasSnapshot;
  v.hasAir = live && in.airT10 != kNoReading;
  v.hasElement = live && in.heaterT10 != kNoReading;
  v.hasPower = live;
  v.airT10 = v.hasAir ? in.airT10 : 0;
  v.elementT10 = v.hasElement ? in.heaterT10 : 0;
  v.powerPermille = v.hasPower ? in.pwmPermille : 0;
  v.nextWithinS = reportIntervalMs(in) / 1000;
  return v;
}

static bool changed(const ReportInputs &a, const ReportValues &av, const ReportInputs &b,
                    const ReportValues &bv) {
  if (a.link != b.link || a.targetT10 != b.targetT10) return true;
  if (av.hasAir != bv.hasAir || av.hasElement != bv.hasElement || av.hasPower != bv.hasPower)
    return true;
  if (av.hasAir && av.airT10 != bv.airT10) return true;
  if (av.hasElement && av.elementT10 != bv.elementT10) return true;
  // Power is reported to the nearest whole percent, so only that step counts.
  if (av.hasPower && av.powerPermille / 10 != bv.powerPermille / 10) return true;
  if (a.hasSnapshot && b.hasSnapshot &&
      (a.state != b.state || a.fanOn != b.fanOn || a.fault != b.fault)) {
    return true;
  }
  return false;
}

bool ReportScheduler::due(const ReportInputs &in, uint32_t nowMs) const {
  if (inFlight_ || plugin_ == PluginState::Missing) return false;
  if (!haveSent_) return true;
  const bool gapOk = intervalElapsed(nowMs, lastSentMs_, kReportMinGapMs);
  if (force_) return gapOk;
  if (gapOk && changed(in, reportValues(in), lastIn_, lastValues_)) return true;
  return intervalElapsed(nowMs, lastSentMs_, lastValues_.nextWithinS * 1000u);
}

void ReportScheduler::onSent(const ReportInputs &in, uint32_t nowMs) {
  haveSent_ = true;
  force_ = false;
  inFlight_ = true;
  lastSentMs_ = nowMs;
  lastIn_ = in;
  lastValues_ = reportValues(in);
  sent_++;
}

void ReportScheduler::onAccepted() {
  plugin_ = PluginState::Found;
  inFlight_ = false;
}

void ReportScheduler::onRefused() {
  refused_++;
  inFlight_ = false;
}

void ReportScheduler::onMethodNotFound() {
  plugin_ = PluginState::Missing;
  inFlight_ = false;
}

void ReportScheduler::onFailed() {
  failed_++;
  inFlight_ = false;
}

void ReportScheduler::forceNext() {
  force_ = true;
  inFlight_ = false;
  if (plugin_ == PluginState::Missing) plugin_ = PluginState::Unknown;
}

void ReportScheduler::reset() {
  haveSent_ = false;
  force_ = false;
  inFlight_ = false;
  plugin_ = PluginState::Unknown;
}

}  // namespace iheaterlink::policy
