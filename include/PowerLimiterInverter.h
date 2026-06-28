// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Configuration.h"
#include <Hoymiles.h>
#include <cstdint>
#include <optional>
#include <memory>
#include <vector>

class PowerLimiterInverter {
public:
    enum class TargetDispatchKind : uint8_t {
        PowerLimit,
        PowerState,
    };

    struct TargetDispatchEvent {
        TargetDispatchKind kind = TargetDispatchKind::PowerLimit;
        uint16_t targetSetpointWatts = 0;
        uint16_t expectedOutputAcWatts = 0;
        uint32_t sentMillis = 0;
        bool powerState = false;
        bool completed = true;
    };

    struct RuntimeState {
        uint64_t Serial = 0;
        uint16_t OutputAcWatts = 0;
        uint16_t ExpectedOutputAcWatts = 0;
        uint16_t PowerLimitWatts = 0;
        uint16_t MaxPowerWatts = 0;
        float GridVoltage = 0.0f;
        bool Reachable = false;
        bool Producing = false;
    };

    static constexpr uint32_t TargetEffectAssumptionMillis = 2 * 1000;
    static constexpr uint32_t RestoredStateAssumptionMillis = 3 * 60 * 1000;

    static std::unique_ptr<PowerLimiterInverter> create(PowerLimiterInverterConfig const& config);

    // send command(s) to inverter to reach desired target state (limit and
    // production). return true if an update is pending, i.e., if the target
    // state is NOT yet reached, false otherwise.
    bool update(bool allowNewDispatch = true);
    std::vector<TargetDispatchEvent> consumeTargetDispatchEvents();

    // retire an inverter from the DPL. the inverter will have it's standby()
    // function (different outcome for different types of inverters) called
    // once. afterwards this method returns true as long as the target state
    // is pending.
    bool retire(bool allowNewDispatch = true);

    // returns the timestamp of the oldest stats received for this inverter
    // *after* its last command completed. return std::nullopt if new stats
    // are pending after the last command completed.
    std::optional<uint32_t> getLatestStatsMillis() const;

    // returns the timestamp of the latest raw inverter statistics frame.
    uint32_t getCurrentStatsMillis() const;

    // returns the timestamp at which the output value used by DPL is expected
    // to be reflected by the power meter. This is either the latest measured
    // inverter stats timestamp, or the settle timestamp of a successfully sent
    // command while newer settled stats are unavailable.
    std::optional<uint32_t> getOutputReferenceMillis() const;

    // the amount of times an update command issued to the inverter timed out
    uint8_t getUpdateTimeouts() const { return _updateTimeouts; }

    // Re-asserts the last known DPL limit after the refresh interval elapsed.
    bool refreshStaleLimit();

    // Brings the inverter's known limit back inside the configured DPL bounds.
    bool applyConfiguredLimitBounds();

    // maximum amount of AC power the inverter is able to produce
    // (not regarding the configured upper power limit)
    uint16_t getInverterMaxPowerWatts() const;

    // maximum amount of AC power the inverter is allowed to produce as per
    // upper power limit (additionally restricted by inverter's absolute max)
    uint16_t getConfiguredMaxPowerWatts() const;
    uint16_t getConfiguredLowerPowerLimitWatts() const { return _config.LowerPowerLimit; }

    // current AC output used by DPL. After a successful command this may be the
    // assumed target output until inverter stats newer than the settle delay arrive.
    uint16_t getCurrentOutputAcWatts() const;
    std::optional<float> getTemperatureCelsius() const;
    uint16_t getCurrentLimitWattsForTrace() const
    {
        return (isProducing() || isUsingAssumedOutput()) ? getCurrentLimitWatts() : 0;
    }
    uint16_t getExpectedLimitWattsForTrace() const
    {
        if (_oTargetPowerState && !*_oTargetPowerState) { return 0; }
        if (getExpectedOutputAcWatts() == 0) { return 0; }
        return getExpectedLimitWatts();
    }
    bool hasPendingTargetForTrace() const { return hasPendingTarget(); }
    bool isUsingAssumedOutputForTrace() const { return isUsingAssumedOutput() || isUsingRestoredState(); }
    bool isUsingRestoredStateForTrace() const { return isUsingRestoredState(); }
    bool hasFreshRuntimeStateForPersistence() const;
    RuntimeState getRuntimeStateForPersistence() const;
    void restoreRuntimeState(RuntimeState const& state, uint32_t restoreMillis);

    // this differs from current output power if new limit was assigned
    virtual uint16_t getExpectedOutputAcWatts() const;

    // the maximum reduction of power output the inverter
    // can achieve with or withouth going into standby.
    virtual uint16_t getMaxReductionWatts(bool allowStandby) const = 0;

    // the maximum increase of power output the inverter can achieve
    // (is expected to achieve), possibly coming out of standby.
    virtual uint16_t getMaxIncreaseWatts() const = 0;

    // Maximum increase that may be used for a paired zero-sum exchange without
    // starting an availability probe on the target inverter.
    virtual uint16_t getMaxNonProbingIncreaseWatts() const { return getMaxIncreaseWatts(); }

    // change the target limit such that the requested change becomes effective
    // on the expected AC power output. returns the change in the range
    // [0..reduction] that will become effective (once update() returns false).
    virtual uint16_t applyReduction(uint16_t reduction, bool allowStandby) = 0;
    virtual uint16_t applyIncrease(uint16_t increase) = 0;

    // Cap a production-limited inverter's power limit without increasing the
    // output that the DPL expects from it.
    virtual bool capOutputLimit(uint16_t) { return false; }

    // stop producing AC power. returns the change in power output
    // that will become effective (once update() returns false).
    virtual uint16_t standby() = 0;
    void reassertTargetPowerState();

    // wake the inverter from standby and set it to produce
    // as much power as permissible by its upper power limit.
    void setMaxOutput(bool fastStart = false);

    bool restart();

    float getGridVoltage() const;
    float getDcVoltage(uint8_t input);
    bool isSendingCommandsEnabled() const { return _spInverter->getEnableCommands(); }
    bool isReachable() const { return isUsingRestoredState() ? _restoredRuntimeState.Reachable : _spInverter->isReachable(); }
    bool isProducing() const { return isUsingRestoredState() ? _restoredRuntimeState.Producing : _spInverter->isProducing(); }
    uint32_t getRadioQueueSize() const;

    uint64_t getSerial() const { return _config.Serial; }
    char const* getSerialStr() const { return _serialStr; }
    bool isBehindPowerMeter() const { return _config.IsBehindPowerMeter; }

    bool isBatteryPowered() const { return _config.PowerSource == PowerLimiterInverterConfig::InverterPowerSource::Battery; }
    bool isSolarPowered() const { return _config.PowerSource == PowerLimiterInverterConfig::InverterPowerSource::Solar; }
    bool isSmartBufferPowered() const { return _config.PowerSource == PowerLimiterInverterConfig::InverterPowerSource::SmartBuffer; }

    void debug() const;

    enum class Eligibility : unsigned {
        Unreachable,
        SendingCommandsDisabled,
        MaxOutputUnknown,
        CurrentLimitUnknown,
        CommandBackoff,
        GridDisconnected,
        Eligible,
        Nighttime
    };

    // only returns true if the inverter can participate
    // in achieving the requested change in power output
    bool isEligible() const;

protected:
    explicit PowerLimiterInverter(PowerLimiterInverterConfig const& config);

    uint16_t getCurrentLimitWatts() const;
    uint16_t getExpectedLimitWatts() const
    {
        if (_oTargetPowerLimitWatts) { return *_oTargetPowerLimitWatts; }
        if (_oLastTargetPowerLimitWatts) { return *_oLastTargetPowerLimitWatts; }
        return getCurrentLimitWatts();
    }
    bool hasPendingTarget() const { return _oTargetPowerLimitWatts || _oTargetPowerState; }
    bool isUsingAssumedOutput() const;
    uint16_t getMeasuredOutputAcWatts() const;

    void setTargetPowerLimitWatts(uint16_t power)
    {
        _oTargetPowerLimitWatts = power;
        _oLastTargetPowerLimitWatts = power;
    }
    void setTargetPowerState(bool enable) { _oTargetPowerState = enable; }
    void setExpectedOutputAcWatts(uint16_t power) { _expectedOutputAcWatts = power; }

    static char mpptName(MpptNum_t mppt);

    // copied to avoid races with web UI
    PowerLimiterInverterConfig _config;

    // Hoymiles lib inverter instance
    std::shared_ptr<InverterAbstract> _spInverter = nullptr;

    char _logPrefix[32];

private:
    static constexpr uint32_t _failedUpdateRetryBackoffMillis = 3 * 1000;
    static constexpr uint32_t _limitRefreshIntervalMillis = 60 * 1000;

    // returns the detailed eligibility status of the inverter
    Eligibility getEligibility() const;

    virtual void setAcOutput(uint16_t expectedOutputWatts) = 0;

    bool hasStatsAtOrAfter(uint32_t timestamp) const;
    bool hasLimitFeedbackAtOrAfter(uint32_t timestamp) const;
    bool hasRestoredLimitPendingFeedback() const;
    bool isUsingRestoredLimit() const;
    bool isUsingRestoredState() const;
    void recordSuccessfulTargetCommand(
            uint32_t commandMillis,
            uint16_t assumedOutputAcWatts,
            bool keepAssumedOutput = false);
    void recordTargetDispatchEvent(TargetDispatchEvent event);
    uint16_t clampLimitToConfiguredBounds(uint16_t power) const;
    uint16_t getLimitResolutionWatts() const;

    bool _retired = false; // true if to be abandoned by DPL

    char _serialStr[16];

    // track the number of times an update command
    // issued to the inverter timed out *or* failed
    uint8_t _updateTimeouts = 0;

    // track (target) state
    std::optional<uint32_t> _oUpdateStartMillis = std::nullopt;
    std::optional<uint32_t> _oNextUpdateAttemptMillis = std::nullopt;
    std::optional<uint16_t> _oTargetPowerLimitWatts = std::nullopt;
    std::optional<uint16_t> _oLastTargetPowerLimitWatts = std::nullopt;
    std::optional<bool> _oTargetPowerState = std::nullopt;
    bool _forcePowerStateCommand = false;
    bool _allowParallelStartupCommands = false;
    std::optional<uint16_t> _oInFlightPowerLimitWatts = std::nullopt;
    std::optional<uint16_t> _oInFlightPowerLimitExpectedOutputAcWatts = std::nullopt;
    std::optional<bool> _oInFlightPowerState = std::nullopt;
    std::optional<uint16_t> _oInFlightPowerStateExpectedOutputAcWatts = std::nullopt;
    std::vector<TargetDispatchEvent> _targetDispatchEvents;
    mutable std::optional<uint32_t> _oStatsMillis = std::nullopt;
    std::optional<uint16_t> _oAssumedOutputAcWatts = std::nullopt;
    std::optional<uint32_t> _oAssumedOutputValidAfterMillis = std::nullopt;
    bool _keepAssumedOutput = false;
    RuntimeState _restoredRuntimeState;
    std::optional<uint32_t> _oRestoredStateMillis = std::nullopt;
    std::optional<uint32_t> _oRestoredLimitMillis = std::nullopt;

    // the expected AC output (possibly is different from the target limit)
    uint16_t _expectedOutputAcWatts = 0;
};
