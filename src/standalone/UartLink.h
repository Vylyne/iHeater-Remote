#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>

#include <FlashJob.h>
#include <RemotePolicy.h>
#include <UartConsole.h>
#include <UartFrames.h>
#include <UartPollPolicy.h>
#include <UartRegisters.h>

#include "controller/ControllerOutputCommand.h"
#include "controller/OutputPort.h"

#ifndef IHEATER_UART_TX_PIN
#define IHEATER_UART_TX_PIN -1
#endif
#ifndef IHEATER_UART_RX_PIN
#define IHEATER_UART_RX_PIN -1
#endif

namespace iheaterlink::standalone {

// One consistent copy of what the poll task knows, for the web UI and logs.
struct UartLinkView {
  policy::LinkStatus link = policy::LinkStatus::Waiting;
  bool hasSnapshot = false;
  policy::IheaterSnapshot snap{};
  bool hasLimits = false;
  policy::IheaterLimits limits{};
  bool protocolSupported = false, desiredKnown = false, rangeEmpty = false;
  policy::ClearPhase clear = policy::ClearPhase::Idle;
  uint8_t blockedFault = 0;
  bool lastClearOk = false;
  // Diagnostics: millis() at the first Ok reply since boot (0 = none yet),
  // requests that got no reply in time, and replies that did not parse.
  uint32_t firstOkMs = 0, timeouts = 0, badReplies = 0;
  // The running flash job's status, or the last job's once it has finished.
  policy::FlashStatus flash;
  bool flashRunning = false;
  // The last Ok input read came from the iHeater's link bootloader (input
  // 0x00 = 0x8001), and what it said about itself.
  bool inBootloader = false;
  policy::BootloaderInfo boot{};
};

// The Remote as Modbus RTU master to the iHeater on UART1. A FreeRTOS task
// polls every kPollPeriodMs using UartPollPolicy. The main loop never touches
// the UART: apply() and setSetupRange() only leave pending values for the
// task, and view() returns a copy of the task's latest results. One spinlock
// guards those hand-offs and is held only while copying small structs.
// startFlash() queues an iHeater flash job, which the task runs in place of
// polling until it finishes.
//
// The console tap: while a console ring is published (consoleStart() to
// consoleStop(), both on the main loop), every exchange the task makes, poll
// and flash job alike, is copied into the ring as one fixed-size record. The
// task only copies bytes: it never formats or allocates for the console.
class UartLink : public OutputPort {
 public:
  void begin() override;  // installs the UART1 driver and starts the task
  // Off becomes 0 C; a temperature becomes that target. Queued for the task.
  void apply(const ControllerOutputCommand &command) override;
  ControllerOutputCommand getLastCommand() const override;
  bool isEnabled() const override { return initialized_; }
  void setSetupRange(policy::TargetRange range);
  UartLinkView view() const;  // copy under the lock

  // Unused stack of the poll task, in bytes (0 before the task exists).
  uint32_t stackHighWaterBytes() const;

  // Main loop -> task. False when a job is already queued or running, or the
  // link is disabled. The caller keeps `image` alive and unchanged until
  // flashBusy() turns false.
  bool startFlash(const uint8_t *image, size_t len);
  void cancelFlash();      // only takes effect until the job sends the erase
  bool flashBusy() const;  // queued or running

  // The console, main loop only. consoleStart() allocates kConsoleLines
  // records and starts recording (true if already recording); false when
  // the heap check fails or the allocation does. consoleStop() stops and
  // frees the ring. consoleDrain() copies at most kConsoleDrainMax records the
  // caller has not seen, oldest first; `dropped` counts records overwritten
  // before they were drained. It returns 0 with dropped 0 for max 0, or
  // when no ring exists.
  static constexpr size_t kConsoleDrainMax = 8;
  bool consoleStart();
  void consoleStop();
  size_t consoleDrain(policy::ConsoleRecord *out, size_t max, uint32_t &dropped);

 private:
  // policy::FlashLink on UART1, for the flash job. Used only from the task.
  class FlashPort final : public policy::FlashLink {
   public:
    explicit FlashPort(UartLink &link) : link_(link) {}
    bool exchange(const uint8_t *frame, size_t len, policy::Reply &reply, uint32_t timeoutMs) override;
    void sleepMs(uint32_t ms) override;
    uint32_t nowMs() override;
    bool heatWanted() override;
    bool cancelRequested() override;
    void publish(const policy::FlashStatus &status) override;

   private:
    UartLink &link_;
  };

  static void taskEntry(void *arg);
  void taskLoop();
  void registerWatchdog();
  void feedWatchdog();
  // Sends one whole frame and judges the reply with parseReply(frame, ...).
  // True when a complete reply arrived (Ok, Exception or Invalid); false when
  // nothing usable arrived within timeoutMs. Feeds the watchdog while it waits.
  bool exchangeFrame(const uint8_t *frame, size_t len, policy::Reply &reply, uint32_t timeoutMs);
  // Copies one exchange into the console ring, if one is still published.
  // Kept out of line so its record sits on the task stack only during the
  // call, never in exchangeFrame's own frame.
  __attribute__((noinline)) void consoleCapture(const uint8_t *frame, size_t len,
                                                const uint8_t *reply, size_t replyLen,
                                                policy::ConsoleResult result, uint8_t code);
  void runRequest(const policy::PollRequest &req, bool &gotOk);
  // Runs a queued flash job, if there is one, to its end.
  void runQueuedFlash(FlashPort &port);
  // holdUpdating keeps link = Updating for the publish right after a job; the
  // next cycle's publish, after its poll, reports the real link again.
  void publishView(bool holdUpdating = false);

  // Written once by begin() before the task starts; read by anyone.
  bool initialized_ = false;
  TaskHandle_t task_ = nullptr;

  // Task-private after begin(): never touched outside taskLoop().
  policy::UartPollPolicy policy_;
  bool watchdogOn_ = false;
  uint32_t firstOkMs_ = 0, timeouts_ = 0, badReplies_ = 0;

  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  // Everything below is guarded by mux_.
  ControllerOutputCommand lastCommand_{};
  bool applied_ = false;  // apply() has been called at least once
  bool pendingDirty_ = false;
  float pendingTargetC_ = 0.0f;
  policy::TargetRange pendingRange_{policy::kDefaultMinC, policy::kDefaultMaxC};
  UartLinkView view_{};
  // The flash job hand-off. image/len are set with flashQueued_ and read by
  // the task when it takes the job.
  bool flashQueued_ = false, flashRunning_ = false, flashCancel_ = false;
  const uint8_t *flashImage_ = nullptr;
  size_t flashLen_ = 0;
  policy::FlashStatus flashStatus_{};  // the running job's, or the last one's
  // The console ring. Written only by the main loop, always under mux_; the
  // task reads console_ only under mux_. consoleRing_ is reset on each start.
  policy::ConsoleRing consoleRing_{nullptr, 0};
  policy::ConsoleRing *console_ = nullptr;  // &consoleRing_ while recording
  policy::ConsoleRecord *consoleStorage_ = nullptr;

  // A hint, read by the task without the lock, so that it skips the capture
  // when nobody is recording. The lock and console_ decide whether a record
  // is kept.
  volatile bool consoleOn_ = false;
};

}  // namespace iheaterlink::standalone
