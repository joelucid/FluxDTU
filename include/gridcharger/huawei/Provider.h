// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <gridcharger/Provider.h>
#include <gridcharger/huawei/HardwareInterface.h>
#include <gridcharger/huawei/DataPoints.h>
#include <gridcharger/huawei/Stats.h>
#include <espMqttClient.h>
#include <frozen/map.h>
#include <frozen/string.h>
#include <functional>
#include <mutex>

namespace GridChargers::Huawei {

// Modes of operation
#define HUAWEI_MODE_OFF 0
#define HUAWEI_MODE_ON 1
#define HUAWEI_MODE_AUTO_EXT 2
#define HUAWEI_MODE_AUTO_INT 3

class Provider : public ::GridChargers::Provider {
public:
    bool init() final;
    void deinit() final;
    void loop() final;

    std::shared_ptr<::GridChargers::Stats> getStats() const final { return _stats; }
    bool getAutoPowerStatus() const final { return _autoPowerEnabled; };
    int16_t getAutoPowerTargetPowerConsumption() const final { return getEffectiveAutoPowerTargetPowerConsumption(); }
    bool isAutoPowerLimitedByAvailablePower() const final { return _autoPowerLimitedByAvailablePower; }
    bool supportsPowerLimiterControl() const final { return true; }
    std::optional<uint32_t> getPowerLimiterOutputReferenceMillis() const final;
    uint16_t getPowerLimiterCurrentInputPowerWatts() const final;
    std::optional<uint16_t> getPowerLimiterTargetInputPowerWatts() const final;
    uint16_t getPowerLimiterExpectedInputPowerWatts() const final;
    uint16_t getPowerLimiterMaxInputPowerWatts() const final;
    std::optional<::GridChargers::PowerLimiterControlProposal>
        getPowerLimiterControlProposal(uint16_t targetInputPowerWatts) const final;
    std::vector<::GridChargers::PowerLimiterTargetDispatchEvent> consumePowerLimiterTargetDispatchEvents() final;
    uint16_t applyPowerLimiterInputPowerIncrease(uint16_t increase) final;
    uint16_t applyPowerLimiterInputPowerReduction(uint16_t reduction) final;
    void setPowerLimiterLimitedByAvailablePower(bool limited) final { _autoPowerLimitedByAvailablePower = limited; }

    void setProduction(bool enable);
    void setParameter(float val, HardwareInterface::Setting setting);

    // determined through trial and error (voltage limits, R4850G2)
    // and some educated guessing (current limits, no R4875 at hand)
    static constexpr float MIN_ONLINE_VOLTAGE = 41.0f;
    static constexpr float MAX_ONLINE_VOLTAGE = 58.6f;
    static constexpr float MIN_ONLINE_CURRENT = 0.0f;
    static constexpr float MAX_ONLINE_CURRENT = 84.0f;
    static constexpr float MIN_OFFLINE_VOLTAGE = 48.0f;
    static constexpr float MAX_OFFLINE_VOLTAGE = 58.4f;
    static constexpr float MIN_OFFLINE_CURRENT = 0.0f;
    static constexpr float MAX_OFFLINE_CURRENT = 84.0f;
    static constexpr float MIN_INPUT_CURRENT_LIMIT = 0.0f;
    static constexpr float MAX_INPUT_CURRENT_LIMIT = 40.0f;
    static constexpr float MIN_AUTO_POWER_BMS_CHARGE_CURRENT_MARGIN = 0.0f;
    static constexpr float MAX_AUTO_POWER_BMS_CHARGE_CURRENT_MARGIN = 5.0f;

private:
    void _setParameter(
            float val,
            HardwareInterface::Setting setting,
            bool pollFeedback = false,
            std::optional<uint16_t> powerLimiterTargetInputPowerWatts = std::nullopt,
            uint32_t powerLimiterTargetEffectAssumptionMillis =
                ::GridChargers::PowerLimiterNormalTargetEffectAssumptionMillis);
    void _setProduction(bool enable) const;

    void setFan(bool online, bool fullSpeed);
    void setMode(uint8_t mode);

    // these control the pin named "power", which in turn is supposed to control
    // a relay (or similar) to enable or disable the PSU using it's slot detect
    // pins.
    void enableOutput();
    void disableOutput();
    gpio_num_t _huaweiPower;

    Task _loopTask;
    std::unique_ptr<HardwareInterface> _upHardwareInterface;

    std::mutex _mutex;
    std::optional<bool> _oOutputEnabled;
    uint8_t _mode = HUAWEI_MODE_AUTO_EXT;

    DataPointContainer _dataPoints;
    std::shared_ptr<Stats> _stats = std::make_shared<Stats>();

    uint32_t _outputCurrentOnSinceMillis = 0;         // Timestamp since when the PSU was idle at zero amps
    uint32_t _nextAutoModePeriodicIntMillis = 0;      // When to set the next output voltage in automatic mode
    uint32_t _lastPowerMeterUpdateReceivedMillis = 0; // Timestamp of last seen power meter value
    uint32_t _lastBatteryUpdateReceivedMillis = 0;    // Timestamp of last processed BMS value
    uint32_t _autoModeBlockedTillMillis = 0;      // Timestamp to block running auto mode for some time

    uint32_t _lastDynamicAutoPowerTargetPowerMeterUpdate = 0;
    float _dynamicAutoPowerTargetMean = 0.0f;
    float _dynamicAutoPowerTargetVariance = 0.0f;
    bool _dynamicAutoPowerTargetInitialized = false;
    int16_t _autoPowerTargetPowerConsumption = 0;
    uint32_t _autoPowerLowerLimitHoldTillMillis = 0;
    uint32_t _autoPowerStartupQualificationSinceMillis = 0;
    float _lastRequestedOnlineCurrent = 0.0f;
    bool _autoPowerReachedLowerPowerLimit = false;

    bool isAutoPowerLowerLimitHoldActive() const;
    int16_t getEffectiveAutoPowerTargetPowerConsumption() const;
    void resetDynamicAutoPowerTargetState();
    void updateDynamicAutoPowerTarget(bool chargerControlActive);
    int16_t calcAutoPowerTargetPowerConsumption() const;
    float getEfficiency() const;
    std::optional<float> getCurrentInputPowerWatts() const;
    float getPowerLimiterMaxInputPowerWattsFloat() const;
    bool shouldBlockAutoPowerByBatteryState(
            bool batterySoCValid,
            float batterySoC,
            bool bmsChargeBlocked) const;
    void setPowerLimiterInputPowerWatts(float inputPower);

    uint8_t _autoPowerEnabledCounter = 0;
    bool _autoPowerEnabled = false;
    bool _autoPowerLimitedByAvailablePower = false;
    mutable bool _autoPowerBlockedByBatteryState = false;
    bool _batteryEmergencyCharging = false;
    std::optional<uint16_t> _oPowerLimiterTargetInputPowerWatts = std::nullopt;
    uint32_t _powerLimiterCommandMillis = 0;
    uint32_t _powerLimiterCommandEffectAssumptionMillis =
        ::GridChargers::PowerLimiterNormalTargetEffectAssumptionMillis;
    std::vector<::GridChargers::PowerLimiterTargetDispatchEvent> _powerLimiterTargetDispatchEvents;

    enum class Topic : unsigned {
        LimitOnlineVoltage,
        LimitOnlineCurrent,
        LimitOfflineVoltage,
        LimitOfflineCurrent,
        LimitInputCurrent,
        Mode,
        Production,
        FanOnlineFullSpeed,
        FanOfflineFullSpeed
    };

    void subscribeTopics();
    static void unsubscribeTopics();

    static constexpr frozen::string _cmdtopic = "huawei/cmd/";
    static constexpr frozen::map<frozen::string, Topic, 9> _subscriptions = {
        { "limit_online_voltage",   Topic::LimitOnlineVoltage },
        { "limit_online_current",   Topic::LimitOnlineCurrent },
        { "limit_offline_voltage",  Topic::LimitOfflineVoltage },
        { "limit_offline_current",  Topic::LimitOfflineCurrent },
        { "limit_input_current",    Topic::LimitInputCurrent },
        { "mode",                   Topic::Mode },
        { "production",             Topic::Production },
        { "fan_online_full_speed",  Topic::FanOnlineFullSpeed },
        { "fan_offline_full_speed", Topic::FanOfflineFullSpeed },
    };

    void onMqttMessage(Topic enumTopic,
            const espMqttClientTypes::MessageProperties& properties,
            const char* topic, const uint8_t* payload, size_t len);
};


} // namespace GridChargers::Huawei
