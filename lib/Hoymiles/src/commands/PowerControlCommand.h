// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "DevControlCommand.h"

class PowerControlCommand : public DevControlCommand {
public:
    explicit PowerControlCommand(InverterAbstract* inv, const uint64_t router_address = 0);

    virtual String getCommandName() const;
    virtual QueueInsertType getQueueInsertType() const;
    virtual bool isHighPriority() const { return _completeOnTxSuccess; }
    virtual bool isCompleteOnTxSuccess() const { return _completeOnTxSuccess; }
    virtual uint8_t getMaxResendCount() const;
    virtual uint8_t getHardwareRetryCount() const;
    virtual bool areSameParameter(CommandAbstract* other);

    virtual bool handleResponse(const fragment_t fragment[], const uint8_t max_fragment_id);
    virtual void handleTxResult(const bool success);
    virtual void gotTimeout();

    void setPowerOn(const bool state);
    void setRestart();
    void setCompleteOnTxSuccess(bool complete);

private:
    enum class Action : uint8_t {
        TurnOn,
        TurnOff,
        Restart
    };

    void applyPowerStateFromCommand();

    Action _action = Action::TurnOn;
    bool _completeOnTxSuccess = false;
};
