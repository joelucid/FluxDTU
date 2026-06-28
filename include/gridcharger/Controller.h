// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <ctime>
#include <TaskSchedulerDeclarations.h>
#include <gridcharger/Provider.h>

namespace GridChargers {

class Controller {
public:
    static constexpr uint8_t MaxConfiguredSoCPercent = 101;

    void init(Scheduler&);
    void updateSettings();

    std::shared_ptr<Stats const> getStats() const;
    bool getAutoPowerStatus() const;
    int16_t getAutoPowerTargetPowerConsumption() const;
    bool isAutoPowerLimitedByAvailablePower() const;
    bool supportsPowerLimiterControl() const;
    std::optional<uint32_t> getPowerLimiterOutputReferenceMillis() const;
    uint16_t getPowerLimiterCurrentInputPowerWatts() const;
    std::optional<uint16_t> getPowerLimiterTargetInputPowerWatts() const;
    uint16_t getPowerLimiterExpectedInputPowerWatts() const;
    uint16_t getPowerLimiterMaxInputPowerWatts() const;
    std::optional<PowerLimiterControlProposal> getPowerLimiterControlProposal(
            uint16_t targetInputPowerWatts) const;
    std::vector<PowerLimiterTargetDispatchEvent> consumePowerLimiterTargetDispatchEvents();
    uint16_t applyPowerLimiterInputPowerIncrease(uint16_t increase);
    uint16_t applyPowerLimiterInputPowerReduction(uint16_t reduction);
    void setPowerLimiterLimitedByAvailablePower(bool limited);
    uint8_t getAutoPowerStopBatterySoCThreshold() const;
    std::optional<float> getAutoPowerPlannedSoC(uint32_t timestamp) const;
    std::optional<float> getAutoPowerSocPlanningOutputPowerLimit(
            float currentOutputPower,
            std::optional<float> requestedOutputPower = std::nullopt) const;
    void getAutoPowerSocPlanLiveViewData(JsonVariant& root) const;

    struct AutoPowerSocPlan {
        bool configured = false;
        bool valid = false;
        bool active = false;
        char const* status = "disabled";
        uint8_t staticStopSoC = 0;
        uint8_t reenableSoC = 0;
        uint8_t effectiveStopSoC = 0;
        uint8_t dayMinSoC = 0;
        uint8_t intermediateTargetSoC = 0;
        uint8_t nightTargetSoC = 0;
        uint8_t chargeTargetSoC = 0;
        float plannedStopSoC = 0.0f;
        float currentSoC = 0.0f;
        uint32_t startTime = 0;
        uint32_t intermediateTime = 0;
        uint32_t finalRampStartTime = 0;
        uint32_t finishTime = 0;
        uint32_t chargeTargetTime = 0;
        int32_t secondsUntilStart = 0;
        int32_t secondsUntilIntermediate = 0;
        int32_t secondsUntilFinalRampStart = 0;
        int32_t secondsUntilFinish = 0;
        int32_t secondsUntilChargeTarget = 0;
        float missingEnergyWh = 0.0f;
        float plannedBatteryChargePowerWatts = 0.0f;
        float actualBatteryChargePowerWatts = 0.0f;
        uint32_t actualBatteryChargePowerUpdateMillis = 0;
        char const* actualBatteryChargePowerSource = "batteryBms";
        bool actualBatteryChargePowerFromGridChargerOutput = false;
        bool missingEnergyAvailable = false;
        bool powerLimitAvailable = false;
        bool actualBatteryChargePowerAvailable = false;
    };

    // Simple template method for provider-specific access
    template<typename T>
    T* getProvider() const;

private:
    void loop() const;
    AutoPowerSocPlan getAutoPowerSocPlan() const;

    struct AutoPowerPlannedSocCache {
        bool valid = false;
        bool windowValid = false;
        uint32_t dayStart = 0;
        uint32_t startTime = 0;
        uint32_t intermediateTime = 0;
        uint32_t finalRampStartTime = 0;
        uint32_t finishTime = 0;
        uint8_t staticStopSoC = 0;
        uint8_t dayMinSoC = 0;
        uint8_t intermediateTargetSoC = 0;
        uint8_t nightTargetSoC = 0;
        uint16_t startAfterSunrise = 0;
        uint16_t intermediateBeforeSunset = 0;
        uint16_t finalRampStartBeforeSunset = 0;
        uint16_t finishBeforeSunset = 0;
    };

    Task _loopTask;
    mutable std::mutex _mutex;
    std::unique_ptr<Provider> _upProvider = nullptr;
    mutable float _autoPowerSocPlanningOutputPowerBiasWatts = 0.0f;
    mutable uint32_t _autoPowerSocPlanningLastBatteryPowerUpdateMillis = 0;
    mutable AutoPowerPlannedSocCache _autoPowerPlannedSocCache;
};

} // namespace GridChargers

extern GridChargers::Controller GridCharger;
