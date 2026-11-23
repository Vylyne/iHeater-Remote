#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>

#include <FlashJob.h>
#include <RemotePolicy.h>
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
  // The last ROM probe got an answer and no Modbus reply has come since.
  bool romDetected = false;
};

// The Remote as Modbus RTU master to the iHeater on UART1. A FreeRTOS task
// polls every kPollPeriodMs using UartPollPolicy. The main loop never touches
// the UART: apply() and setSetupRange() only leave pending values for the
// task, and view() returns a copy of the task's latest results. One spinlock
// guards those hand-offs and is held only while copying small structs.
// startFlash() queues an iHeater flash job, which the task runs in place of
// polling until it finishes.
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
  bool startFlash(const uint8_t *image, size_t len, policy::FlashStart start);
  void cancelFlash();      // only takes effect while cooling
  bool flashBusy() const;  // queued or running

 private:
  // policy::FlashLink on UART1, for the flash job and the ROM probe. Used only
  // from the task.
  class FlashPort final : public policy::FlashLink {
   public:
    explicit FlashPort(UartLink &link) : link_(link) {}
    void setParity(policy::LinkParity parity) override;
    void send(const uint8_t *data, size_t len) override;
    size_t receive(uint8_t *buf, size_t len, uint32_t timeoutMs) override;
    void flushInput() override;
    bool modbus(const policy::PollRequest &req, policy::Reply &reply) override;
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
  // One request/reply exchange. True when a complete reply arrived (Ok,
  // Exception or Invalid); false when nothing usable arrived in time.
  bool exchange(const policy::PollRequest &req, policy::Reply &reply);
  void runRequest(const policy::PollRequest &req, bool &gotOk);
  // Runs a queued flash job, if there is one, to its end.
  void runQueuedFlash(FlashPort &port);
  void publishView();

  // Written once by begin() before the task starts; read by anyone.
  bool initialized_ = false;
  TaskHandle_t task_ = nullptr;

  // Task-private after begin(): never touched outside taskLoop().
  policy::UartPollPolicy policy_;
  bool watchdogOn_ = false;
  uint32_t firstOkMs_ = 0, timeouts_ = 0, badReplies_ = 0;
  // millis() at the last Ok reply and the last ROM probe (0 = none yet).
  uint32_t lastOkMs_ = 0, lastProbeMs_ = 0;
  bool romDetected_ = false;

  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  // Everything below is guarded by mux_.
  ControllerOutputCommand lastCommand_{};
  bool applied_ = false;  // apply() has been called at least once
  bool pendingDirty_ = false;
  float pendingTargetC_ = 0.0f;
  policy::TargetRange pendingRange_{policy::kDefaultMinC, policy::kDefaultMaxC};
  UartLinkView view_{};
  // The flash job hand-off. image/len/start are set with flashQueued_ and
  // read by the task when it takes the job.
  bool flashQueued_ = false, flashRunning_ = false, flashCancel_ = false;
  const uint8_t *flashImage_ = nullptr;
  size_t flashLen_ = 0;
  policy::FlashStart flashStart_ = policy::FlashStart::Normal;
  policy::FlashStatus flashStatus_{};  // the running job's, or the last one's
};

}  // namespace iheaterlink::standalone
