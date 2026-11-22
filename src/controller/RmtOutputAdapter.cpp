#include "controller/RmtOutputAdapter.h"

#include <string.h>

namespace iheaterlink {

// Запускает RMT-драйвер и FreeRTOS-таск периодической отправки фреймов на STM32.
void RmtOutputAdapter::begin() {
  lastPulseCode_ = encode(lastCommand_);

  if (config_.outputPin == 0xFF) {
    return;
  }

  rmt_config_t cfg = {};
  cfg.rmt_mode = RMT_MODE_TX;
  cfg.channel = config_.channel;
  cfg.gpio_num = static_cast<gpio_num_t>(config_.outputPin);
  cfg.clk_div = 80; // 80 МГц / 80 = 1 МГц, 1 тик = 1 мкс
  cfg.mem_block_num = config_.memBlockNum;
  cfg.tx_config.loop_en = false;
  cfg.tx_config.carrier_en = false;
  cfg.tx_config.idle_output_en = true;
  cfg.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;

  if (rmt_config(&cfg) != ESP_OK) {
    return;
  }
  if (rmt_driver_install(config_.channel, 0, 0) != ESP_OK) {
    return;
  }

  rmt_set_tx_thr_intr_en(config_.channel, false, 0);

  initialized_ = true;
  rebuildFrame(lastPulseCode_);

  // Выделенный FreeRTOS-таск: период RMT не зависит от блокировок main loop
  // (WiFi/MQTT/HA могут подвешивать loopTask на секунды — это убивало тайминг).
  xTaskCreate(&RmtOutputAdapter::rmtTaskEntry, "rmt_tx", 3072, this,
              tskIDLE_PRIORITY + 2, &rmtTask_);
}

// Сохраняет команду и пересчитывает пульс-код для следующего фрейма.
void RmtOutputAdapter::apply(const ControllerOutputCommand &command) {
  lastCommand_ = command;
  lastPulseCode_ = encode(command);
}

ControllerOutputCommand RmtOutputAdapter::getLastCommand() const {
  return lastCommand_;
}

// Кодирует команду в число импульсов: offCode для Off, иначе температура квантуется в min–max.
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

// Builds the frame with policy::buildPulseFrame, then appends the end
// marker the legacy driver needs: an item with val = 0 (both durations 0).
// A frame that does not fit leaves itemCount_ at 0, and nothing is sent.
void RmtOutputAdapter::rebuildFrame(uint8_t pulseCode) {
  const policy::PulseTiming timing{config_.syncLowUs, config_.pulseHighUs,
                                   config_.pulseLowUs};
  const size_t n =
      policy::buildPulseFrame(pulseCode, timing, frame_, MAX_ITEMS - 1);
  for (size_t i = 0; i < n; ++i) {
    items_[i].level0 = frame_[i].level0;
    items_[i].duration0 = frame_[i].duration0;
    items_[i].level1 = frame_[i].level1;
    items_[i].duration1 = frame_[i].duration1;
  }
  if (n > 0) {
    items_[n].val = 0;
  }
  itemCount_ = n > 0 ? n + 1 : 0;
  builtForCode_ = pulseCode;
}

// Будит FreeRTOS-таск и отправляет фрейм немедленно, не дожидаясь таймера.
void RmtOutputAdapter::forceFrame() {
  if (!isEnabled() || !rmtTask_)
    return;
  xTaskNotifyGive(rmtTask_);
}

// Точка входа FreeRTOS-таска (static) — делегирует в rmtTaskLoop.
void RmtOutputAdapter::rmtTaskEntry(void *arg) {
  static_cast<RmtOutputAdapter *>(arg)->rmtTaskLoop();
}

// Основной цикл таска: отправлять фрейм каждые framePeriodMs или по
// уведомлению.
void RmtOutputAdapter::rmtTaskLoop() {
  const TickType_t period = pdMS_TO_TICKS(config_.framePeriodMs);
  for (;;) {
    sendFrame();
    ulTaskNotifyTake(pdTRUE, period);
  }
}

// Перестроить фрейм если код изменился, затем отправить через RMT.
void RmtOutputAdapter::sendFrame() {
  if (builtForCode_ != lastPulseCode_) {
    rebuildFrame(lastPulseCode_);
  }

  const uint32_t now = millis();
  const uint32_t dt = lastSentMs_ ? (now - lastSentMs_) : 0;
  lastSentMs_ = now;
  ++seqNum_;
  // Serial.printf("[RMT] #%lu t=%lu dt=%lu ms code=%u items=%u\n",
  //               (unsigned long)seqNum_,
  //               (unsigned long)now, (unsigned long)dt,
  //               (unsigned)lastPulseCode_, (unsigned)itemCount_);

  if (itemCount_ == 0) {
    return;
  }
  rmt_write_items(config_.channel, items_, static_cast<int>(itemCount_), false);
}

} // namespace iheaterlink
