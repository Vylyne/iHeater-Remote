#pragma once

#include <stdint.h>

#include "RemotePolicy.h"
#include "UartFrames.h"
#include "UartRegisters.h"

// Poll policy for the Remote's Modbus master. Pure decisions, no Arduino
// dependency. Each cycle the caller sends cycleRead(), feeds the outcome to
// onReply()/onNoReply(), then asks followUp() for at most one more request.
namespace iheaterlink::policy {

constexpr uint32_t kPollPeriodMs = 200;
constexpr uint32_t kReplyTimeoutMs = 200;
constexpr uint32_t kNoReplyAfterMs = 1000;
constexpr uint32_t kRestartWindowMs = 10000;

enum class LinkStatus : uint8_t { Waiting, Up, Restarting, NoReply, Updating };
enum class ClearPhase : uint8_t { Idle, AwaitRestart, Blocked };
enum class PollKind : uint8_t { None, ReadInputs, ReadSettings, WriteTarget, WriteClear, WriteBootloader };
struct PollRequest {
  PollKind kind;
  uint16_t addr;
  uint16_t value;  // register count for reads
};

// "waiting", "up", "restarting", "no-reply".
const char *linkStatusName(LinkStatus s);

class UartPollPolicy {
 public:
  // The first call makes the desired target known.
  void setDesired(float targetC, TargetRange setup);
  PollRequest cycleRead() const;
  PollRequest followUp(uint32_t nowMs) const;
  void onReply(const PollRequest &req, const Reply &reply, uint32_t nowMs);
  void onNoReply(const PollRequest &req, uint32_t nowMs);
  LinkStatus link(uint32_t nowMs) const;

  bool hasSnapshot() const { return hasSnapshot_; }
  const IheaterSnapshot &snapshot() const { return snapshot_; }
  bool hasLimits() const { return hasLimits_; }
  const IheaterLimits &limits() const { return limits_; }
  bool protocolSupported() const { return hasSnapshot_ && snapshot_.proto == kSupportedProtocol; }
  bool desiredKnown() const { return desiredKnown_; }
  uint16_t desiredT10() const { return desiredT10_; }
  bool rangeEmpty() const { return rangeEmpty_; }

  // True while the last Ok input read had 0x00 = 0x8001 (the link bootloader).
  // Then there is no snapshot and no write is offered.
  bool inBootloader() const { return inBootloader_; }
  const BootloaderInfo &bootloader() const { return bootloader_; }

  // Automatic fault clearing. A clear is sent when the iHeater says it is safe
  // and its budget allows; success shows as a restart with last_clear_result 1.
  ClearPhase clearPhase() const { return clearPhase_; }
  uint8_t blockedFault() const { return blockedFault_; }
  bool lastClearOk() const { return lastClearOk_; }

 private:
  void recomputeDesired();
  void noteClearSent(const PollRequest &req, uint32_t nowMs);
  void updateClearPhase(const IheaterSnapshot &s, uint32_t nowMs, bool wasQuiet);

  IheaterSnapshot snapshot_{};
  IheaterLimits limits_{};
  bool hasSnapshot_ = false;
  bool hasLimits_ = false;
  bool lastInputsOk_ = false;

  bool desiredKnown_ = false;
  float desiredC_ = 0.0f;
  TargetRange setup_{0, 0};
  uint16_t desiredT10_ = 0;
  bool rangeEmpty_ = false;

  bool refusalKnown_ = false;
  uint16_t refusedT10_ = 0;
  IheaterState refusedState_ = IheaterState::Booting;

  bool inBootloader_ = false;
  BootloaderInfo bootloader_{};

  bool haveOk_ = false;
  uint32_t lastOkMs_ = 0;

  ClearPhase clearPhase_ = ClearPhase::Idle;
  uint32_t clearSentMs_ = 0;
  uint8_t clearSentCode_ = 0;
  uint8_t clearsLeftAtSend_ = 0;
  uint32_t clearUptimeAtSend_ = 0;
  uint8_t blockedFault_ = 0;
  bool lastClearOk_ = false;
};

}  // namespace iheaterlink::policy
