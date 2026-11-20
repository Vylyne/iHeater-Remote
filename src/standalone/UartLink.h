#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>

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
};

// The Remote as Modbus RTU master to the iHeater on UART1. A FreeRTOS task
// polls every kPollPeriodMs using UartPollPolicy. The main loop never touches
// the UART: apply() and setSetupRange() only leave pending values for the
// task, and view() returns a copy of the task's latest results. One spinlock
// guards those hand-offs and is held only while copying small structs.
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

 private:
  static void taskEntry(void *arg);
  void taskLoop();
  void registerWatchdog();
  void feedWatchdog();
  // One request/reply exchange. True when a complete reply arrived (Ok,
  // Exception or Invalid); false when nothing usable arrived in time.
  bool exchange(const policy::PollRequest &req, policy::Reply &reply);
  void runRequest(const policy::PollRequest &req, bool &gotOk);
  void publishView();

  // Written once by begin() before the task starts; read by anyone.
  bool initialized_ = false;
  TaskHandle_t task_ = nullptr;

  // Task-private after begin(): never touched outside taskLoop().
  policy::UartPollPolicy policy_;
  bool watchdogOn_ = false;

  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  // Everything below is guarded by mux_.
  ControllerOutputCommand lastCommand_{};
  bool applied_ = false;  // apply() has been called at least once
  bool pendingDirty_ = false;
  float pendingTargetC_ = 0.0f;
  policy::TargetRange pendingRange_{policy::kDefaultMinC, policy::kDefaultMaxC};
  UartLinkView view_{};
};

}  // namespace iheaterlink::standalone
