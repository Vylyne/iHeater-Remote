#include "standalone/UartLink.h"

#include <driver/uart.h>
#include <esp_task_wdt.h>
#include <string.h>

#include <UartFrames.h>

namespace iheaterlink::standalone {

namespace {

constexpr uart_port_t kPort = UART_NUM_1;
constexpr int kBaud = 115200;
constexpr int kRxBufferBytes = 256;  // must exceed the 128-byte hardware FIFO
// The flash job holds a 258 B write body and a 256 B read buffer on the stack.
constexpr uint32_t kTaskStackBytes = 6144;
constexpr UBaseType_t kTaskPriority = 5;
// The longest reply is 5 + 2 * kMaxReadRegs bytes; leave a little headroom.
constexpr size_t kReplyBufBytes = 96;
// Used only if the task watchdog was not already running (see registerWatchdog).
constexpr uint32_t kFallbackWdtSeconds = 5;
// The flash job's waits are cut into slices of at most this, with the watchdog
// fed after each, so a 10 s erase or a 30-minute cool-down never starves it.
constexpr uint32_t kWaitSliceMs = 500;

// Builds the wire frame for a policy request. 0 when the request has no frame.
size_t buildFrame(const policy::PollRequest &req, uint8_t out[policy::kRequestLen]) {
  switch (req.kind) {
    case policy::PollKind::ReadInputs:
      return policy::buildReadRequest(policy::kFcReadInput, req.addr, req.value, out);
    case policy::PollKind::ReadSettings:
      return policy::buildReadRequest(policy::kFcReadHolding, req.addr, req.value, out);
    case policy::PollKind::WriteTarget:
    case policy::PollKind::WriteClear:
    case policy::PollKind::WriteBootloader:
      return policy::buildWriteSingle(req.addr, req.value, out);
    default:
      return 0;
  }
}

}  // namespace

void UartLink::begin() {
  if (initialized_)
    return;
  if (IHEATER_UART_TX_PIN < 0 || IHEATER_UART_RX_PIN < 0) {
    Serial.println("[UART] no UART pins configured; link disabled");
    return;
  }

  uart_config_t cfg = {};
  cfg.baud_rate = kBaud;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_APB;

  if (uart_driver_install(kPort, kRxBufferBytes, 0, 0, nullptr, 0) != ESP_OK) {
    Serial.println("[UART] driver install failed; link disabled");
    return;
  }
  if (uart_param_config(kPort, &cfg) != ESP_OK ||
      uart_set_pin(kPort, IHEATER_UART_TX_PIN, IHEATER_UART_RX_PIN,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    Serial.println("[UART] port setup failed; link disabled");
    uart_driver_delete(kPort);
    return;
  }

  if (xTaskCreate(&UartLink::taskEntry, "uart_link", kTaskStackBytes, this,
                  kTaskPriority, &task_) != pdPASS) {
    Serial.println("[UART] could not start the poll task; link disabled");
    task_ = nullptr;
    uart_driver_delete(kPort);
    return;
  }
  initialized_ = true;
  Serial.printf("[UART] polling the iHeater on UART1 tx=%d rx=%d\n",
                (int)IHEATER_UART_TX_PIN, (int)IHEATER_UART_RX_PIN);
}

void UartLink::apply(const ControllerOutputCommand &command) {
  // Off, or any target of 0 C or less, is target 0. The policy clamps the rest
  // into the Setup range and the iHeater's limits.
  const float targetC =
      command.mode == ControllerOutputMode::Off ? 0.0f : command.targetTempC;
  portENTER_CRITICAL(&mux_);
  lastCommand_ = command;
  pendingTargetC_ = targetC;
  applied_ = true;
  pendingDirty_ = true;
  portEXIT_CRITICAL(&mux_);
}

ControllerOutputCommand UartLink::getLastCommand() const {
  portENTER_CRITICAL(&mux_);
  const ControllerOutputCommand copy = lastCommand_;
  portEXIT_CRITICAL(&mux_);
  return copy;
}

void UartLink::setSetupRange(policy::TargetRange range) {
  portENTER_CRITICAL(&mux_);
  pendingRange_ = range;
  pendingDirty_ = true;  // the task re-derives the target once apply() has run
  portEXIT_CRITICAL(&mux_);
}

UartLinkView UartLink::view() const {
  portENTER_CRITICAL(&mux_);
  const UartLinkView copy = view_;
  portEXIT_CRITICAL(&mux_);
  return copy;
}

bool UartLink::startFlash(const uint8_t *image, size_t len, policy::FlashStart start) {
  if (!initialized_)
    return false;
  portENTER_CRITICAL(&mux_);
  const bool busy = flashQueued_ || flashRunning_;
  if (!busy) {
    flashQueued_ = true;
    flashImage_ = image;
    flashLen_ = len;
    flashStart_ = start;
    flashCancel_ = false;  // a stale cancel must not stop this job
  }
  portEXIT_CRITICAL(&mux_);
  return !busy;
}

void UartLink::cancelFlash() {
  portENTER_CRITICAL(&mux_);
  flashCancel_ = true;
  portEXIT_CRITICAL(&mux_);
}

bool UartLink::flashBusy() const {
  portENTER_CRITICAL(&mux_);
  const bool busy = flashQueued_ || flashRunning_;
  portEXIT_CRITICAL(&mux_);
  return busy;
}

uint32_t UartLink::stackHighWaterBytes() const {
  if (!task_)
    return 0;
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(task_)) * sizeof(StackType_t);
}

void UartLink::taskEntry(void *arg) {
  static_cast<UartLink *>(arg)->taskLoop();
}

// The Arduino-ESP32 3.x build sets CONFIG_ESP_TASK_WDT_INIT=y, so ESP-IDF
// starts the task watchdog before app_main and esp_task_wdt_add() normally
// succeeds.
// If it ever reports that the watchdog is not running, start it with the
// build's own settings and subscribe again. If that also fails the link keeps
// polling without a watchdog, and says so once.
void UartLink::registerWatchdog() {
  esp_err_t err = esp_task_wdt_add(nullptr);
  if (err == ESP_ERR_INVALID_STATE) {
    const esp_task_wdt_config_t cfg = {
        .timeout_ms = kFallbackWdtSeconds * 1000,
        .idle_core_mask = 0,  // watch only the tasks that subscribe
        .trigger_panic = true,
    };
    if (esp_task_wdt_init(&cfg) == ESP_OK)
      err = esp_task_wdt_add(nullptr);
  }
  watchdogOn_ = err == ESP_OK;
  if (!watchdogOn_)
    Serial.printf("[UART] task watchdog unavailable (err=%d); polling without it\n", (int)err);
}

void UartLink::feedWatchdog() {
  if (watchdogOn_)
    esp_task_wdt_reset();
}

bool UartLink::exchange(const policy::PollRequest &req, policy::Reply &reply) {
  uint8_t frame[policy::kRequestLen];
  if (buildFrame(req, frame) != policy::kRequestLen)
    return false;

  // Anything still in the buffer belongs to an earlier, abandoned request.
  uart_flush_input(kPort);
  // No gap is needed before the follow-up request. The iHeater turns its
  // receiver off while it transmits and back on at transmit-complete
  // (iHeater-Standalone-Firmware Core/Src/link_uart.c:157-168). This task
  // cannot act on a reply until the ESP-IDF RX timeout expires, about ten
  // symbol times after the last byte, so the next request always arrives
  // with the iHeater's receiver already on again.
  uart_write_bytes(kPort, reinterpret_cast<const char *>(frame), sizeof(frame));
  uart_wait_tx_done(kPort, pdMS_TO_TICKS(10));

  uint8_t buf[kReplyBufBytes];
  size_t len = 0;
  const uint32_t startMs = millis();
  for (;;) {
    const uint32_t elapsed = millis() - startMs;
    if (elapsed >= policy::kReplyTimeoutMs)
      return false;

    // Wait for the next byte, then take whatever else has already arrived.
    const int got = uart_read_bytes(kPort, buf + len, 1,
                                    pdMS_TO_TICKS(policy::kReplyTimeoutMs - elapsed));
    if (got <= 0)
      continue;
    len += static_cast<size_t>(got);

    size_t avail = 0;
    if (uart_get_buffered_data_len(kPort, &avail) == ESP_OK && avail > 0) {
      if (avail > sizeof(buf) - len)
        avail = sizeof(buf) - len;
      if (avail > 0) {
        const int more = uart_read_bytes(kPort, buf + len, avail, 0);
        if (more > 0)
          len += static_cast<size_t>(more);
      }
    }

    reply = policy::parseReply(frame, buf, len);
    if (reply.kind != policy::ReplyKind::Incomplete)
      return true;
    if (len >= sizeof(buf)) {
      // More bytes than any valid reply: not a reply to us.
      reply = policy::Reply{};
      reply.kind = policy::ReplyKind::Invalid;
      return true;
    }
  }
}

void UartLink::runRequest(const policy::PollRequest &req, bool &gotOk) {
  policy::Reply reply;
  const bool got = exchange(req, reply);
  const uint32_t now = millis();
  if (got) {
    policy_.onReply(req, reply, now);
    gotOk = reply.kind == policy::ReplyKind::Ok;
    if (gotOk) {
      if (firstOkMs_ == 0)
        firstOkMs_ = now ? now : 1;
      lastOkMs_ = now;
      romDetected_ = false;  // the application is answering
    }
    if (reply.kind == policy::ReplyKind::Invalid)
      ++badReplies_;
  } else {
    policy_.onNoReply(req, now);
    gotOk = false;
    ++timeouts_;
  }
}

void UartLink::publishView() {
  UartLinkView v;
  v.link = policy_.link(millis());
  v.hasSnapshot = policy_.hasSnapshot();
  if (v.hasSnapshot)
    v.snap = policy_.snapshot();
  v.hasLimits = policy_.hasLimits();
  if (v.hasLimits)
    v.limits = policy_.limits();
  v.protocolSupported = policy_.protocolSupported();
  v.desiredKnown = policy_.desiredKnown();
  v.rangeEmpty = policy_.rangeEmpty();
  v.clear = policy_.clearPhase();
  v.blockedFault = policy_.blockedFault();
  v.lastClearOk = policy_.lastClearOk();
  v.firstOkMs = firstOkMs_;
  v.timeouts = timeouts_;
  v.badReplies = badReplies_;
  v.romDetected = romDetected_;

  portENTER_CRITICAL(&mux_);
  v.flash = flashStatus_;
  v.flashRunning = flashRunning_;
  if (flashRunning_)
    v.link = policy::LinkStatus::Updating;
  view_ = v;
  portEXIT_CRITICAL(&mux_);
}

void UartLink::runQueuedFlash(FlashPort &port) {
  bool queued = false;
  const uint8_t *image = nullptr;
  size_t len = 0;
  policy::FlashStart start = policy::FlashStart::Normal;
  portENTER_CRITICAL(&mux_);
  if (flashQueued_) {
    queued = true;
    image = flashImage_;
    len = flashLen_;
    start = flashStart_;
    flashQueued_ = false;
    flashRunning_ = true;
    // Drop the last job's status now, so nobody reads its outcome as this one's.
    flashStatus_ = policy::FlashStatus{};
    view_.flash = flashStatus_;
    view_.flashRunning = true;
    view_.link = policy::LinkStatus::Updating;
  }
  portEXIT_CRITICAL(&mux_);
  if (!queued)
    return;

  // Every return path of the job leaves UART1 at 8N1 with its input flushed
  // (FlashPort::setParity). apply() calls meanwhile stay pending.
  const policy::FlashStatus result = policy::runFlashJob(port, image, len, start);
  feedWatchdog();
  if (result.outcome == policy::FlashOutcome::Updated)
    romDetected_ = false;  // the new application answered

  portENTER_CRITICAL(&mux_);
  flashStatus_ = result;
  flashRunning_ = false;
  flashImage_ = nullptr;
  flashLen_ = 0;
  portEXIT_CRITICAL(&mux_);
  publishView();
}

// One cycle every kPollPeriodMs, forever: the read of the input block, then at
// most one follow-up. A silent iHeater costs at most kReplyTimeoutMs per
// request; there is no back-off, so a recovering iHeater is noticed within a
// cycle.
void UartLink::taskLoop() {
  registerWatchdog();
  FlashPort port(*this);
  const TickType_t period = pdMS_TO_TICKS(policy::kPollPeriodMs);
  TickType_t lastWake = xTaskGetTickCount();

  for (;;) {
    feedWatchdog();

    // 0. A queued flash job runs to its end in place of polling.
    runQueuedFlash(port);

    // 1. Take the pending desired target and range (short copy under the lock).
    bool haveDesired = false;
    float targetC = 0.0f;
    policy::TargetRange range{policy::kDefaultMinC, policy::kDefaultMaxC};
    portENTER_CRITICAL(&mux_);
    if (applied_ && pendingDirty_) {
      haveDesired = true;
      targetC = pendingTargetC_;
      range = pendingRange_;
      pendingDirty_ = false;
    }
    portEXIT_CRITICAL(&mux_);
    if (haveDesired)
      policy_.setDesired(targetC, range);

    // 2. The cycle read, then 3. one follow-up if the read succeeded.
    bool gotOk = false;
    runRequest(policy_.cycleRead(), gotOk);
    feedWatchdog();
    if (gotOk) {
      const policy::PollRequest next = policy_.followUp(millis());
      if (next.kind != policy::PollKind::None) {
        bool followOk = false;
        runRequest(next, followOk);
        feedWatchdog();
      }
    }

    // 3b. With no Ok reply for a while, look for the iHeater's ROM bootloader,
    // but never while a job is queued (it would be taken next cycle anyway).
    if (!gotOk && !flashBusy() && policy::romProbeDue(millis(), lastOkMs_, lastProbeMs_)) {
      romDetected_ = policy::probeRom(port);
      lastProbeMs_ = millis();
      feedWatchdog();
    }

    // 4. Publish the results for the main loop.
    publishView();

    // 5. Wait out the rest of the period. If the cycle overran it (a silent
    // iHeater costs a full timeout), start the next one now and re-anchor
    // instead of bursting to catch up.
    const TickType_t now = xTaskGetTickCount();
    if (static_cast<TickType_t>(now - lastWake) >= period) {
      lastWake = now;
      vTaskDelay(1);
    } else {
      vTaskDelayUntil(&lastWake, period);
    }
  }
}

void UartLink::FlashPort::setParity(policy::LinkParity parity) {
  uart_wait_tx_done(kPort, pdMS_TO_TICKS(20));
  uart_set_parity(kPort, parity == policy::LinkParity::Even ? UART_PARITY_EVEN
                                                             : UART_PARITY_DISABLE);
  uart_flush_input(kPort);
}

void UartLink::FlashPort::send(const uint8_t *data, size_t len) {
  uart_write_bytes(kPort, reinterpret_cast<const char *>(data), len);
  uart_wait_tx_done(kPort, pdMS_TO_TICKS(100));
}

size_t UartLink::FlashPort::receive(uint8_t *buf, size_t len, uint32_t timeoutMs) {
  size_t got = 0;
  const uint32_t startMs = millis();
  while (got < len) {
    const uint32_t elapsed = millis() - startMs;
    if (elapsed >= timeoutMs)
      break;
    uint32_t slice = timeoutMs - elapsed;
    if (slice > kWaitSliceMs)
      slice = kWaitSliceMs;
    const int n = uart_read_bytes(kPort, buf + got, len - got, pdMS_TO_TICKS(slice));
    if (n > 0)
      got += static_cast<size_t>(n);
    link_.feedWatchdog();
  }
  return got;
}

void UartLink::FlashPort::flushInput() {
  uart_flush_input(kPort);
}

bool UartLink::FlashPort::modbus(const policy::PollRequest &req, policy::Reply &reply) {
  const bool got = link_.exchange(req, reply);
  link_.feedWatchdog();
  return got;
}

void UartLink::FlashPort::sleepMs(uint32_t ms) {
  while (ms > 0) {
    const uint32_t slice = ms < kWaitSliceMs ? ms : kWaitSliceMs;
    vTaskDelay(pdMS_TO_TICKS(slice));
    ms -= slice;
    link_.feedWatchdog();
  }
}

uint32_t UartLink::FlashPort::nowMs() {
  return millis();
}

bool UartLink::FlashPort::heatWanted() {
  portENTER_CRITICAL(&link_.mux_);
  const ControllerOutputCommand command = link_.lastCommand_;
  portEXIT_CRITICAL(&link_.mux_);
  return policy::heatWanted(command.mode == ControllerOutputMode::TargetTemperature,
                            command.targetTempC);
}

bool UartLink::FlashPort::cancelRequested() {
  portENTER_CRITICAL(&link_.mux_);
  const bool cancel = link_.flashCancel_;
  portEXIT_CRITICAL(&link_.mux_);
  return cancel;
}

void UartLink::FlashPort::publish(const policy::FlashStatus &status) {
  portENTER_CRITICAL(&link_.mux_);
  link_.flashStatus_ = status;
  link_.view_.flash = status;
  link_.view_.flashRunning = true;
  link_.view_.link = policy::LinkStatus::Updating;
  portEXIT_CRITICAL(&link_.mux_);
}

}  // namespace iheaterlink::standalone
