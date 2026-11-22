#pragma once

#include <Arduino.h>
#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ControllerOutputCommand.h"
#include "OutputPort.h"
#include "PulseFrame.h"

namespace iheaterlink
{

#ifndef IHEATER_TRIGGER_OUTPUT_PIN
#define IHEATER_TRIGGER_OUTPUT_PIN 0xFF
#endif

    struct RmtOutputConfig
    {
        uint8_t outputPin = IHEATER_TRIGGER_OUTPUT_PIN;
        // Two of the C3's 48-symbol RMT memory blocks; the driver refills
        // them while a longer frame goes out.
        size_t memBlockSymbols = 96;
        uint8_t offCode = 10;
        uint8_t minTempC = 45;
        uint8_t maxTempC = 80;
        uint8_t stepTempC = 1;
        uint32_t syncLowUs = 120000;
        uint32_t pulseHighUs = 2000;
        uint32_t pulseLowUs = 2000;
        uint32_t framePeriodMs = 1000;
    };

    class RmtOutputAdapter : public OutputPort
    {
    public:
        explicit RmtOutputAdapter(const RmtOutputConfig &config)
            : config_(config) {}

        void begin() override;
        void apply(const ControllerOutputCommand &command) override;
        ControllerOutputCommand getLastCommand() const override;

        uint8_t getLastPulseCode() const { return lastPulseCode_; }
        bool isEnabled() const override { return config_.outputPin != 0xFF && initialized_; }
        void forceFrame() override;
        const RmtOutputConfig &getConfig() const { return config_; }

    private:
        static constexpr size_t MAX_ITEMS = 128;

        uint8_t encode(const ControllerOutputCommand &command) const;
        void rebuildFrame(uint8_t pulseCode);
        void sendFrame();

        static void rmtTaskEntry(void *arg);
        void rmtTaskLoop();

        RmtOutputConfig config_;
        ControllerOutputCommand lastCommand_{};
        volatile uint8_t lastPulseCode_ = 10;
        uint8_t builtForCode_ = 0xFF;
        bool initialized_ = false;

        policy::PulseSymbol frame_[MAX_ITEMS]{};
        rmt_symbol_word_t symbols_[MAX_ITEMS]{};
        size_t itemCount_ = 0;


        rmt_channel_handle_t channel_ = nullptr;
        rmt_encoder_handle_t encoder_ = nullptr;
        TaskHandle_t rmtTask_ = nullptr;
    };

} // namespace iheaterlink
