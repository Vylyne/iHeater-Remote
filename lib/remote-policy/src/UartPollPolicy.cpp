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
  // 3. Automatic clear, only when the iHeater reports it safe and within budget.
  // Blocked keeps this out until the active fault changes (see updateClearPhase).
  if (faulted && snapshot_.activeFault != 0 && snapshot_.clearableNow &&
      snapshot_.clearsLeft > 0 && clearPhase_ == ClearPhase::Idle) {
    return {PollKind::WriteClear, kRegClear, snapshot_.activeFault};
  }
  // 4. Target write.
  if (desiredKnown_ && !faulted && desiredT10_ != static_cast<uint16_t>(snapshot_.airTargetT10)) {
    const bool refused =
        refusalKnown_ && refusedT10_ == desiredT10_ && refusedState_ == snapshot_.state;
    if (!refused) return {PollKind::WriteTarget, kRegTarget, desiredT10_};
  }
  return none;
}

void UartPollPolicy::onReply(const PollRequest &req, const Reply &reply, uint32_t nowMs) {
  bool wasQuiet = false;
  if (reply.kind == ReplyKind::Ok) {
    // A reply after the link had gone quiet means the iHeater may have restarted.
    const LinkStatus before = link(nowMs);
    wasQuiet = (before == LinkStatus::NoReply || before == LinkStatus::Restarting);
    if (wasQuiet) {
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
        updateClearPhase(s, nowMs, wasQuiet);
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
    case PollKind::WriteClear:
      noteClearSent(req, nowMs);
      if (reply.kind == ReplyKind::Ok) {
        clearPhase_ = ClearPhase::AwaitRestart;
      } else if (reply.kind == ReplyKind::Exception && reply.exception == 0x03) {
        clearPhase_ = ClearPhase::Idle;  // refused; decide again from fresh registers
      } else if (reply.kind == ReplyKind::Exception) {
        clearPhase_ = ClearPhase::Blocked;  // 0x04: the erase failed
        blockedFault_ = clearSentCode_;
      } else {
        clearPhase_ = ClearPhase::AwaitRestart;  // unreadable reply: treat as lost
      }
      break;
    default:
      break;
  }
  recomputeDesired();
}

void UartPollPolicy::onNoReply(const PollRequest &req, uint32_t nowMs) {
  if (req.kind == PollKind::ReadInputs) lastInputsOk_ = false;
  if (req.kind == PollKind::WriteClear) {
    // The reply may have been lost after a successful clear; verify on return.
    noteClearSent(req, nowMs);
    clearPhase_ = ClearPhase::AwaitRestart;
  }
}

void UartPollPolicy::noteClearSent(const PollRequest &req, uint32_t nowMs) {
  clearSentCode_ = static_cast<uint8_t>(req.value);
  clearsLeftAtSend_ = snapshot_.clearsLeft;
  clearUptimeAtSend_ = snapshot_.uptimeS;
  clearSentMs_ = nowMs;
}

void UartPollPolicy::updateClearPhase(const IheaterSnapshot &s, uint32_t nowMs,
                                      bool wasQuiet) {
  if (clearPhase_ == ClearPhase::Blocked && s.activeFault != blockedFault_) {
    clearPhase_ = ClearPhase::Idle;
  } else if (clearPhase_ == ClearPhase::AwaitRestart) {
    // The iHeater is silent for ~5 s while it restarts and the clear goes out
    // after ~5 s of uptime, so the first reply often shows an uptime at or above
    // the one at send. A reply after the link went quiet is the restart too.
    if (s.uptimeS < clearUptimeAtSend_ || wasQuiet) {
      // Success needs the result code and exactly one clear spent.
      lastClearOk_ = (s.lastClearResult == 1 && s.clearsLeft + 1 == clearsLeftAtSend_);
      clearPhase_ = ClearPhase::Idle;
    } else if (s.clearsLeft < clearsLeftAtSend_ && s.state == IheaterState::Fault &&
               s.activeFault == clearSentCode_) {
      clearPhase_ = ClearPhase::Blocked;  // erase failed and the reply was lost
      blockedFault_ = clearSentCode_;
    } else if (intervalElapsed(nowMs, clearSentMs_, kRestartWindowMs)) {
      clearPhase_ = ClearPhase::Idle;  // the clear never landed
    }
  }
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
