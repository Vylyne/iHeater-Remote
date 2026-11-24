#pragma once

#include <stddef.h>
#include <stdint.h>

#include "UartFrames.h"
#include "UartPollPolicy.h"
#include "UartRegisters.h"

// Flash job for the iHeater: a pure, synchronous sequencer over an abstract
// FlashLink. It updates the iHeater's app through the iHeater's own link
// bootloader, in Modbus RTU frames only (115200 8N1, slave 1), following the
// flasher contract in iHeater-Standalone-Firmware
// docs/design/2026-11-24-link-bootloader-design.md and that repo's
// tools/linkboot_flash.py. It reports through FlashLink::publish. No Arduino
// dependency.
namespace iheaterlink::policy {

enum class FlashPhase : uint8_t { Idle, Cooling, Entering, Erasing, Writing, Starting, Restarting, Finished };
enum class FlashOutcome : uint8_t {
  None, Updated, ImageRejected, NoReply, UnexpectedState, NoLinkBootloader, Cancelled, HeatRequested,
  CoolTimeout, BadReading, NoReplyCooling, EntryRefused, EntryNotConfirmed, EraseRefused, WriteRefused,
  BootRefused, NoAppAfterUpdate
};

// Cooling: the air target goes to 0 and the heater must read 50.0 C or less.
constexpr uint32_t kCoolPollMs = 5000, kCoolTimeoutMs = 30u * 60u * 1000u;
constexpr int16_t kSafeHeaterT10 = 500;
constexpr uint8_t kCoolMissLimit = 3;

// Every exchange is sent up to kFlashTries times, the identical frame each
// time, while the reply is missing or damaged. A Modbus exception is never
// resent.
constexpr uint8_t kFlashTries = 5;
constexpr uint32_t kJobReplyMs = 500;    // reply wait for every frame but these two:
constexpr uint32_t kLongReplyMs = 2000;  // erase and boot
constexpr uint32_t kEntryPollMs = 100;   // input 0x00 poll period, after 0xB007 and after boot
constexpr uint32_t kEntryWaitMs = 3000;  // for 0x8001 after 0xB007
// For 1 after the boot command. The app is silent for about 5 s while it
// boots (reset_LEDs(10), init), then flushes every request that came in.
constexpr uint32_t kAppWaitMs = 15000;
constexpr uint8_t kRestarts = 1;         // shared by not-blank and boot-refused

struct FlashStatus {
  FlashPhase phase = FlashPhase::Idle;
  FlashOutcome outcome = FlashOutcome::None;  // None while idle or running
  FlashPhase failedPhase = FlashPhase::Idle;
  uint16_t progressPermille = 0;   // chunks written / total chunks
  int16_t heaterT10 = kNoReading;  // last cool-down reading
  bool entryUnconfirmed = false;   // no try of 0xB007 got a usable reply
  uint16_t retries = 0;            // frames resent after no usable reply
  uint16_t lastError = 0;          // the bootloader's last_error, when read
  uint16_t appCheck = 0;           // the bootloader's app_check, when read
  char detail[48] = "";            // e.g. "chunk 12: no reply"
  // The job found the bootloader at its start, with this why and timeout_s
  // (inputs 0x03 and 0x06; both 0 when that read failed).
  bool startedInBootloader = false;
  uint16_t startWhy = 0, startTimeoutS = 0;
};

class FlashLink {
 public:
  virtual ~FlashLink() = default;
  // Sends one whole frame, then judges what comes back with
  // parseReply(frame, ...). False: no complete reply within timeoutMs.
  virtual bool exchange(const uint8_t *frame, size_t len, Reply &reply, uint32_t timeoutMs) = 0;
  virtual void sleepMs(uint32_t ms) = 0;
  virtual uint32_t nowMs() = 0;
  virtual bool heatWanted() = 0;
  virtual bool cancelRequested() = 0;
  virtual void publish(const FlashStatus &status) = 0;
};

// Runs one job and returns the final status (phase Finished), which it also
// publishes. Input 0x00 decides where it starts: 1 (the app) cools and enters
// the bootloader first; 0x8001 (the bootloader) reads its why and timeout_s,
// then goes straight to the erase. A cancel is honoured until the first erase
// is sent.
FlashStatus runFlashJob(FlashLink &link, const uint8_t *image, size_t len);

// Heat is wanted when the source holds a positive target.
bool heatWanted(bool targetMode, float targetC);

struct FlashStartInputs {
  bool uartOutput = false, imageOk = false, heatWanted = false, jobRunning = false,
       restartPending = false;  // a Remote restart is already scheduled
  LinkStatus link = LinkStatus::Waiting;
  bool inBootloader = false;     // the poll last read 0x8001 at input 0x00
  uint16_t bootloaderWhy = 0;    // its input 0x03: 1 asked, 2 no valid app
  uint16_t bootloaderTimeoutS = 0;  // its input 0x06: seconds until it returns to the app
  FlashOutcome lastOutcome = FlashOutcome::None;
};
enum class FlashRefusal : uint8_t {
  None, NotUart, NoImage, HeatWanted, JobRunning, LinkDown, BootloaderLeaving, NothingToRetry, RestartPending
};
// RestartPending is checked first, for every start: a restart already
// scheduled would cut a job off mid-erase. The rest are checked in the order
// NotUart, JobRunning, NoImage, HeatWanted. Then a normal start needs the
// link Up, or a bootloader waiting for firmware: why 2, or why 1 with its
// timer stopped (timeout 0, after an erase). Why 1 with time left gives
// BootloaderLeaving, anything else LinkDown. A retry ignores the link and
// needs a last outcome that retryAllowed() accepts (else NothingToRetry).
FlashRefusal decideFlashStart(const FlashStartInputs &in, bool retry);
const char *flashRefusalMessage(FlashRefusal refusal);

// False for None, Updated, Cancelled, HeatRequested, ImageRejected and
// NoLinkBootloader: a rerun would end the same way, or nothing failed.
bool retryAllowed(FlashOutcome outcome);
// True when the job updated the app, or stopped before sending the erase
// with a valid app still there: false for any job that started in a
// bootloader that doesn't return to its app (see bootloaderReturnsToApp).
bool applicationIntact(const FlashStatus &status);

// True when a bootloader with this why (input 0x03) and timeout_s (0x06)
// goes back to a valid app by itself: only one entered with 0xB007 (why 1)
// over a valid app, whose timer still runs. An erase stops the timer for
// good, and why 2 never had one; both wait for firmware.
bool bootloaderReturnsToApp(uint16_t why, uint16_t timeoutS);

const char *flashPhaseName(FlashPhase phase);
const char *flashOutcomeName(FlashOutcome outcome);
const char *flashOutcomeMessage(FlashOutcome outcome);
// The finished job's message: flashOutcomeMessage, except for a cancel that
// leaves the iHeater in a bootloader with no valid app.
const char *flashStatusMessage(const FlashStatus &status);

}  // namespace iheaterlink::policy
