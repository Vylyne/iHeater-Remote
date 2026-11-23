#pragma once

#include <stddef.h>
#include <stdint.h>

#include "UartPollPolicy.h"
#include "UartRegisters.h"

// Flash job for the iHeater: a pure, synchronous sequencer over an abstract
// FlashLink. It drives the STM32 ROM bootloader (AN3155) and reports through
// FlashLink::publish. No Arduino dependency.
namespace iheaterlink::policy {

enum class LinkParity : uint8_t { None, Even };
enum class FlashStart : uint8_t { Normal, Recovery, Retry };
enum class FlashPhase : uint8_t {
  Idle, Cooling, Entering, Syncing, Erasing, Writing, Starting, Restarting, Finished
};
enum class FlashOutcome : uint8_t {
  None, Updated, PowerCycleNeeded, Cancelled, HeatRequested, CoolTimeout, BadReading,
  NoReplyCooling, EntryRefused, EntryNotConfirmed, ImageRejected, SyncFailed, RomError,
  VerifyFailed, NoReplyAfterUpdate
};

constexpr uint32_t kCoolPollMs = 5000, kCoolTimeoutMs = 30u * 60u * 1000u;
constexpr int16_t kSafeHeaterT10 = 500;
constexpr uint8_t kCoolMissLimit = 3;
constexpr uint32_t kSyncSettleMs = 100, kSyncGapMs = 100;
constexpr uint8_t kSyncTries = 20;
constexpr uint32_t kRestartWaitMs = 15000;
constexpr uint32_t kRomProbeAfterMs = 10000, kRomProbePeriodMs = 30000, kRomProbeReplyMs = 100;

struct FlashStatus {
  FlashPhase phase = FlashPhase::Idle;
  FlashOutcome outcome = FlashOutcome::None;  // None while idle or running
  FlashPhase failedPhase = FlashPhase::Idle;
  uint16_t progressPermille = 0;   // bytes verified / padded length
  int16_t heaterT10 = kNoReading;  // last cool-down reading
  bool entryUnconfirmed = false;   // 0xB007 got no usable reply
  bool extendedErase = false, unexpectedPid = false;
  uint16_t pid = 0;
  char detail[48] = "";  // e.g. "write at 0x08000100: NACK"
};

class FlashLink {
 public:
  virtual ~FlashLink() = default;
  virtual void setParity(LinkParity parity) = 0;  // also drops pending input
  virtual void send(const uint8_t *data, size_t len) = 0;  // returns once sent
  virtual size_t receive(uint8_t *buf, size_t len, uint32_t timeoutMs) = 0;
  virtual void flushInput() = 0;
  virtual bool modbus(const PollRequest &req, Reply &reply) = 0;  // false: nothing usable in time
  virtual void sleepMs(uint32_t ms) = 0;
  virtual uint32_t nowMs() = 0;
  virtual bool heatWanted() = 0;
  virtual bool cancelRequested() = 0;
  virtual void publish(const FlashStatus &status) = 0;
};

// Runs one job and returns the final status (phase Finished). Every return
// path leaves the link at 8N1 and publishes that final status.
FlashStatus runFlashJob(FlashLink &link, const uint8_t *image, size_t len, FlashStart start);

// A 0x7F at 8E1, and a second one if the first gets no reply (a synced ROM
// only answers the pair): any ACK or NACK means the ROM bootloader answered.
// Leaves the link at 8N1.
bool probeRom(FlashLink &link);

// True when the iHeater has not answered Modbus for kRomProbeAfterMs and the
// last probe (0 means none yet) was at least kRomProbePeriodMs ago.
bool romProbeDue(uint32_t nowMs, uint32_t lastOkMs, uint32_t lastProbeMs);

// Heat is wanted when the source holds a positive target.
bool heatWanted(bool targetMode, float targetC);

struct FlashStartInputs {
  bool uartOutput = false, imageOk = false, heatWanted = false, jobRunning = false, romDetected = false,
       restartPending = false;  // a Remote restart is already scheduled
  LinkStatus link = LinkStatus::Waiting;
  FlashOutcome lastOutcome = FlashOutcome::None;
};
enum class FlashRefusal : uint8_t { None, NotUart, NoImage, HeatWanted, JobRunning, LinkDown, NothingToRetry,
                                  RestartPending };
struct FlashStartDecision {
  FlashRefusal refusal;
  FlashStart start;  // meaningful only when refusal is None
};
// RestartPending is checked first, for every kind of start: a restart already
// scheduled would cut a job off mid-erase. The rest are checked in the order
// NotUart, JobRunning, NoImage, HeatWanted, then LinkDown (NothingToRetry for
// a retry, which ignores the link).
FlashStartDecision decideFlashStart(const FlashStartInputs &in, bool retry);
const char *flashRefusalMessage(FlashRefusal refusal);

bool retryAllowed(FlashOutcome outcome);
bool applicationIntact(FlashOutcome outcome);

const char *flashPhaseName(FlashPhase phase);
const char *flashOutcomeName(FlashOutcome outcome);
const char *flashOutcomeMessage(FlashOutcome outcome);

}  // namespace iheaterlink::policy
