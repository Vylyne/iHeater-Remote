#pragma once

#include "ControllerOutputCommand.h"

namespace iheaterlink
{

    // What the Remote needs from whatever carries its target to the iHeater:
    // the RMT pulse train, or (later) the UART link.
    class OutputPort
    {
    public:
        virtual ~OutputPort() = default;
        virtual void begin() = 0;
        virtual void apply(const ControllerOutputCommand &command) = 0;
        virtual ControllerOutputCommand getLastCommand() const = 0;
        virtual bool isEnabled() const = 0;
        virtual void forceFrame() {}
    };

} // namespace iheaterlink
