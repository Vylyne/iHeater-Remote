#include "controller/RmtOutputAdapter.h"

#include <string.h>

namespace iheaterlink {

// Starts the RMT TX channel and the FreeRTOS task that sends a frame to
// the STM32 every framePeriodMs.
void RmtOutputAdapter::begin() {
  lastPulseCode_ = encode(lastCommand_);

  if (config_.outputPin == 0xFF) {
    return;
  }

  rmt_tx_channel_config_t chan = {};
  chan.gpio_num = static_cast<gpio_num_t>(config_.outputPin);
  chan.clk_src = RMT_CLK_SRC_DEFAULT;
  chan.resolution_hz = 1000000;  // 1 tick = 1 µs
  chan.mem_block_symbols = config_.memBlockSymbols;
  chan.trans_queue_depth = 1;
  chan.flags.init_level = 0;  // idle low until the first frame

  esp_err_t err = rmt_new_tx_channel(&chan, &channel_);
  if (err != ESP_OK) {
    Serial.printf("[RMT] rmt_new_tx_channel failed (err=%d)\n", (int)err);
    return;
  }
  const rmt_copy_encoder_config_t copy = {};
  err = rmt_new_copy_encoder(&copy, &encoder_);
  if (err != ESP_OK) {
    Serial.printf("[RMT] rmt_new_copy_encoder failed (err=%d)\n", (int)err);
    return;
  }
  err = rmt_enable(channel_);
  if (err != ESP_OK) {
    Serial.printf("[RMT] rmt_enable failed (err=%d)\n", (int)err);
    return;
  }

  initialized_ = true;
  rebuildFrame(lastPulseCode_);

  // A dedicated task keeps the frame period independent of the main loop,
  // which Wi-Fi and the web server can stall for seconds.
  xTaskCreate(&RmtOutputAdapter::rmtTaskEntry, "rmt_tx", 3072, this,
              tskIDLE_PRIORITY + 2, &rmtTask_);
}

// Stores the command and its pulse code for the next frame.
void RmtOutputAdapter::apply(const ControllerOutputCommand &command) {
  lastCommand_ = command;
  lastPulseCode_ = encode(command);
}

ControllerOutputCommand RmtOutputAdapter::getLastCommand() const {
  return lastCommand_;
}

// Encodes the command as a pulse count: offCode for Off, otherwise the
// target quantized into minTempC..maxTempC.
uint8_t RmtOutputAdapter::encode(const ControllerOutputCommand &command) const {
  if (command.mode == ControllerOutputMode::Off) {
    return config_.offCode;
  }

  int target = static_cast<int>(lroundf(command.targetTempC));
  if (target < config_.minTempC) {
    target = config_.minTempC;
  }
  if (target > config_.maxTempC) {
    target = config_.maxTempC;
  }

  const int base = config_.minTempC;
  const int step = config_.stepTempC > 0 ? config_.stepTempC : 5;
  const int quantized = base + (((target - base) + (step / 2)) / step) * step;
  return static_cast<uint8_t>(quantized);
}

// Builds the frame's symbols with policy::buildPulseFrame. The new driver
// is given the symbol count, so it needs no end marker. A frame that does
// not fit leaves itemCount_ at 0, and nothing is sent.
void RmtOutputAdapter::rebuildFrame(uint8_t pulseCode) {
  const policy::PulseTiming timing{config_.syncLowUs, config_.pulseHighUs,
                                   config_.pulseLowUs};
  const size_t n = policy::buildPulseFrame(pulseCode, timing, frame_, MAX_ITEMS);
  for (size_t i = 0; i < n; ++i) {
    symbols_[i].level0 = frame_[i].level0;
    symbols_[i].duration0 = frame_[i].duration0;
    symbols_[i].level1 = frame_[i].level1;
    symbols_[i].duration1 = frame_[i].duration1;
  }
  itemCount_ = n;
  builtForCode_ = pulseCode;
}

// Wakes the task to send a frame now rather than at the next period.
void RmtOutputAdapter::forceFrame() {
  if (!isEnabled() || !rmtTask_)
    return;
  xTaskNotifyGive(rmtTask_);
}

// FreeRTOS task entry point; runs rmtTaskLoop.
void RmtOutputAdapter::rmtTaskEntry(void *arg) {
  static_cast<RmtOutputAdapter *>(arg)->rmtTaskLoop();
}

// Sends a frame every framePeriodMs, or sooner when notified.
void RmtOutputAdapter::rmtTaskLoop() {
  const TickType_t period = pdMS_TO_TICKS(config_.framePeriodMs);
  for (;;) {
    sendFrame();
    ulTaskNotifyTake(pdTRUE, period);
  }
}

// Waits for the previous frame to finish, rebuilds the frame if the code
// changed, then queues it. Waiting first means the copy encoder never reads
// a buffer being rewritten, and a forced frame never cuts into the one
// going out.
void RmtOutputAdapter::sendFrame() {
  const esp_err_t done = rmt_tx_wait_all_done(channel_, 1000);
  if (done != ESP_OK) {
    Serial.printf("[RMT] previous frame still sending (err=%d)\n", (int)done);
    return;
  }

  if (builtForCode_ != lastPulseCode_) {
    rebuildFrame(lastPulseCode_);
  }
  if (itemCount_ == 0) {
    return;
  }

  const rmt_transmit_config_t tx = {};  // loop_count 0, eot_level 0 (low)
  rmt_transmit(channel_, encoder_, symbols_,
               itemCount_ * sizeof(rmt_symbol_word_t), &tx);
}

} // namespace iheaterlink
