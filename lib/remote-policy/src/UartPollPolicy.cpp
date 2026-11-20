#include "UartPollPolicy.h"

namespace iheaterlink::policy {

void UartPollPolicy::recomputeDesired() {
  if (!desiredKnown_) return;
  if (hasLimits_) {
    RangeT10 r{};
    rangeEmpty_ = !effectiveRangeT10(setup_, limits_.minTargetT10, limits_.maxAirTargetT10, r);
    desiredT10_ =
        uartTargetT10(desiredC_, setup_, limits_.minTargetT10, limits_.maxAirTargetT10);
  } else {
    // Provisional until the settings are read; nothing but 0 is written before then.
    rangeEmpty_ = false;
    desiredT10_ = uartTargetT10(desiredC_, setup_, INT16_MIN, INT16_MAX);
  }
}

void UartPollPolicy::setDesired(float targetC, TargetRange setup) {
  desiredKnown_ = true;
  desiredC_ = targetC;
  setup_ = setup;
  recomputeDesired();
}

PollRequest UartPollPolicy::cycleRead() const {
  return {PollKind::ReadInputs, kInputBase, kInputCount};
}

PollRequest UartPollPolicy::followUp(uint32_t) const {
  const PollRequest none{PollKind::None, 0, 0};
  if (!hasSnapshot_ || !protocolSupported() || !lastInputsOk_) return none;
  const bool faulted = snapshot_.state == IheaterState::Fault;

  // 1. Off is always safe, even before the limits are known.
  if (desiredKnown_ && desiredT10_ == 0 && snapshot_.airTargetT10 != 0 && !faulted) {
    return {PollKind::WriteTarget, kRegTarget, 0};
  }
  // 2. Settings.
  if (!hasLimits_) return {PollKind::ReadSettings, kSettingsBase, kSettingsCount};
  // 3. Automatic clear: added in a later task.
  // 4. Target write.
  if (desiredKnown_ && !faulted && desiredT10_ != static_cast<uint16_t>(snapshot_.airTargetT10)) {
    const bool refused =
        refusalKnown_ && refusedT10_ == desiredT10_ && refusedState_ == snapshot_.state;
    if (!refused) return {PollKind::WriteTarget, kRegTarget, desiredT10_};
  }
  return none;
}

void UartPollPolicy::onReply(const PollRequest &req, const Reply &reply, uint32_t nowMs) {
  if (reply.kind == ReplyKind::Ok) {
    // A reply after the link had gone quiet means the iHeater may have restarted.
    const LinkStatus before = link(nowMs);
    if (before == LinkStatus::NoReply || before == LinkStatus::Restarting) {
      hasLimits_ = false;
      refusalKnown_ = false;
    }
    haveOk_ = true;
    lastOkMs_ = nowMs;
  }

  switch (req.kind) {
    case PollKind::ReadInputs: {
      IheaterSnapshot s{};
      if (reply.kind == ReplyKind::Ok && decodeInputs(reply.regs, reply.count, s)) {
        if (hasSnapshot_ && s.uptimeS < snapshot_.uptimeS) {
          hasLimits_ = false;
          refusalKnown_ = false;
        }
        snapshot_ = s;
        hasSnapshot_ = true;
        lastInputsOk_ = true;
      } else {
        lastInputsOk_ = false;
      }
      break;
    }
    case PollKind::ReadSettings: {
      IheaterLimits l{};
      if (reply.kind == ReplyKind::Ok && decodeSettings(reply.regs, reply.count, l)) {
        limits_ = l;
        hasLimits_ = true;
      }
      break;
    }
    case PollKind::WriteTarget:
      if (reply.kind == ReplyKind::Exception) {
        refusalKnown_ = true;
        refusedT10_ = req.value;
        refusedState_ = snapshot_.state;
      }
      break;
    default:
      break;
  }
  recomputeDesired();
}

void UartPollPolicy::onNoReply(const PollRequest &req, uint32_t) {
  if (req.kind == PollKind::ReadInputs) lastInputsOk_ = false;
}

LinkStatus UartPollPolicy::link(uint32_t nowMs) const {
  if (!haveOk_) return LinkStatus::Waiting;
  if (!intervalElapsed(nowMs, lastOkMs_, kNoReplyAfterMs)) return LinkStatus::Up;
  if (clearPhase_ == ClearPhase::AwaitRestart &&
      !intervalElapsed(nowMs, clearSentMs_, kRestartWindowMs)) {
    return LinkStatus::Restarting;
  }
  return LinkStatus::NoReply;
}

}  // namespace iheaterlink::policy
