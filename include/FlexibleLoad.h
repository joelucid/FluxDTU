// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Configuration.h"
#include <Arduino.h>
#include <TaskSchedulerDeclarations.h>
#include <optional>

class FlexibleLoadClass {
public:
    static constexpr size_t MaxLoadCount = POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT;

    enum class State : uint8_t {
        Disabled,
        Off,
        WaitingForStart,
        Starting,
        RunningMinTime,
        Running,
        Cooldown,
    };

    void init(Scheduler& scheduler);

    size_t getLoadCount() const { return MaxLoadCount; }
    State getState(size_t index) const;
    char const* getStateText(size_t index) const;
    char const* getStartReasonText(size_t index) const;
    char const* getStartBlockReasonText(size_t index) const;
    char const* getStopReason(size_t index) const;
    float getBatterySupportEnergyWh(size_t index) const;
    float getBatteryDischargePowerWatts() const;
    bool isRunning(size_t index) const;

private:
    enum class StartReason : uint8_t {
        None,
        BatterySoC,
        BmsChargeCurrentLimit,
    };

    enum class StartBlockReason : uint8_t {
        None,
        PowerLimiterDisabled,
        Disabled,
        MqttTopicMissing,
        BatteryDisabled,
        PowerMeterInvalid,
        MqttDisconnected,
        FeedInBelowStartLimit,
        StartConditionMissing,
        StartDelay,
        StartSlotBusy,
        Cooldown,
        EmergencyStop,
    };

    struct Runtime {
        State CurrentState = State::Disabled;
        uint32_t StateChangedMillis = 0;
        uint32_t StartCandidateSince = 0;
        uint32_t StopCandidateSince = 0;
        uint32_t GridChargerLimitStopCandidateSince = 0;
        uint32_t BmsChargeLimitStopCandidateSince = 0;
        uint32_t LastCommandPublish = 0;
        uint32_t LoadStartedMillis = 0;
        uint32_t LastBatteryEnergyUpdate = 0;
        uint32_t LastBatterySupportStatePersist = 0;
        float BatterySupportEnergyWh = 0.0f;
        StartReason StartCandidateReason = StartReason::None;
        StartReason CurrentStartReason = StartReason::None;
        StartBlockReason CurrentStartBlockReason = StartBlockReason::None;
        char const* StopReason = "none";
    };

    void loop();
    bool loopLoad(size_t index, Runtime& runtime, PowerLimiterConfig const& powerLimiterConfig,
            PowerLimiterFlexibleLoadConfig const& config, uint32_t now, bool canStart);
    StartReason getStartReason(PowerLimiterConfig const& powerLimiterConfig,
            PowerLimiterFlexibleLoadConfig const& config, Runtime& runtime);
    bool canTakeOverGridCharger(PowerLimiterFlexibleLoadConfig const& config) const;
    bool canUseBatteryForStart(PowerLimiterFlexibleLoadConfig const& config) const;
    bool canUseBatteryBuffer(PowerLimiterFlexibleLoadConfig const& config) const;
    uint16_t getGridChargerTakeoverPowerWatts(PowerLimiterFlexibleLoadConfig const& config) const;
    float getStartAvailablePowerWatts(PowerLimiterFlexibleLoadConfig const& config) const;
    float getStopGridPowerWatts(PowerLimiterFlexibleLoadConfig const& config) const;
    float getStopDeficitPowerWatts(PowerLimiterFlexibleLoadConfig const& config) const;
    bool hasGridPowerReachedStartLimit(PowerLimiterFlexibleLoadConfig const& config) const;
    bool hasBatteryReachedStartSoC(PowerLimiterFlexibleLoadConfig const& config) const;
    bool isBmsChargeCurrentLimited(PowerLimiterFlexibleLoadConfig const& config) const;
    bool shouldStopForGridPower(size_t index, PowerLimiterFlexibleLoadConfig const& config) const;
    bool shouldStopForGridChargerLimit(PowerLimiterFlexibleLoadConfig const& config) const;
    bool handleGridChargerLimitStop(size_t index, PowerLimiterFlexibleLoadConfig const& config, Runtime& runtime, uint32_t now);
    bool handleBmsChargeCurrentLimitStop(size_t index, PowerLimiterFlexibleLoadConfig const& config, Runtime& runtime, uint32_t now);
    bool hasBatterySupportBudgetExceeded(PowerLimiterFlexibleLoadConfig const& config, Runtime const& runtime) const;
    std::optional<float> getMeasuredLoadPowerWatts(size_t index) const;
    float getMeasuredRunningLoadPowerWatts() const;
    bool accountsBatteryDischargeForBuffer(PowerLimiterFlexibleLoadConfig const& config) const;
    float allocateSharedPowerToLoad(size_t index, float totalSharedPowerWatts) const;
    float getGridDeficitSupportPowerWatts(size_t index, PowerLimiterFlexibleLoadConfig const& config) const;
    float getBatteryDischargeSupportPowerWatts(size_t index, PowerLimiterFlexibleLoadConfig const& config) const;
    float getRequiredBatteryBufferPowerWatts(size_t index, PowerLimiterFlexibleLoadConfig const& config) const;
    float getBatterySupportPowerWatts(size_t index, PowerLimiterFlexibleLoadConfig const& config) const;
    void updateBatterySupportEnergy(size_t index, PowerLimiterFlexibleLoadConfig const& config, Runtime& runtime);
    bool publishCommand(char const* payload, PowerLimiterFlexibleLoadConfig const& config, Runtime& runtime);
    void publishPeriodicCommand(PowerLimiterFlexibleLoadConfig const& config, Runtime& runtime, uint32_t now);
    char const* getPeriodicCommandPayload(PowerLimiterFlexibleLoadConfig const& config, Runtime const& runtime) const;
    bool startLoad(size_t index, PowerLimiterFlexibleLoadConfig const& config, Runtime& runtime, StartReason reason);
    bool stopLoad(size_t index, PowerLimiterFlexibleLoadConfig const& config, Runtime& runtime, char const* reason);
    void restoreState();
    bool persistState() const;
    void persistBatterySupportStateIfDue(Runtime& runtime, uint32_t now);
    void setState(size_t index, PowerLimiterFlexibleLoadConfig const& config, Runtime& runtime, State state);
    void resetRunAccounting(Runtime& runtime);
    bool isRunning(Runtime const& runtime) const;
    bool hasStartingLoad() const;
    char const* getStateText(State state) const;
    char const* getStartReasonText(StartReason reason) const;
    char const* getStartBlockReasonText(StartBlockReason reason) const;

    Task _loopTask;
    Runtime _runtime[MaxLoadCount];
};

extern FlexibleLoadClass FlexibleLoad;
