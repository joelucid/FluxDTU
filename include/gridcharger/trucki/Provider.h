// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <gridcharger/Provider.h>
#include <gridcharger/trucki/Stats.h>
#include <HttpGetter.h>
#include <Utils.h>

namespace GridChargers::Trucki {

class Provider : public ::GridChargers::Provider {
public:
    bool init() final;
    void deinit() final;
    void loop() final;
    std::shared_ptr<::GridChargers::Stats> getStats() const final { return _stats; }
    int16_t getAutoPowerTargetPowerConsumption() const final;
    bool isAutoPowerTargetPowerConsumptionZeroHoldActive() const final;
    bool isAutoPowerLimitedByAvailablePower() const final { return _autoPowerLimitedByAvailablePower; }
    bool supportsPowerLimiterControl() const final { return true; }
    std::optional<uint32_t> getPowerLimiterOutputReferenceMillis() const final;
    uint16_t getPowerLimiterCurrentInputPowerWatts() const final;
    uint16_t getPowerLimiterExpectedInputPowerWatts() const final;
    uint16_t getPowerLimiterMaxInputPowerWatts() const final;
    uint16_t applyPowerLimiterInputPowerIncrease(uint16_t increase) final;
    uint16_t applyPowerLimiterInputPowerReduction(uint16_t reduction) final;
    void setPowerLimiterLimitedByAvailablePower(bool limited) final { _autoPowerLimitedByAvailablePower = limited; }

    bool getAutoPowerStatus() const final {
        return _requestedPowerAc > 0
            || _dataCurrent.get<DataPointLabel::AcPowerSetpoint>().value_or(0) > 0.0f
            || _dataCurrent.get<DataPointLabel::DcPower>().value_or(0) > 0.0f;
    }

private:
    template<DataPointLabel L>
    void addStringToDataPoints(const JsonDocument& doc, const char* key)
    {
        if (doc[key].is<std::string>()) {
            _dataCurrent.add<L>(doc[key].as<std::string>());
            return;
        }

        if (doc[key].is<const char*>()) {
            _dataCurrent.add<L>(std::string{doc[key].as<const char*>()});
        }
    }

    template<DataPointLabel L>
    void addFloatToDataPoints(const JsonDocument& doc, const char* key)
    {
        std::optional<float> value;

        if (doc[key].is<float>()) {
            value = doc[key].as<float>();
        }

        if (doc[key].is<char const*>()) {
            value = Utils::getFromString<float>(doc[key].as<char const*>());
        }

        if (!value.has_value()) { return; }
        _dataCurrent.add<L>(*value);
    }

    void powerControlLoop();

    static void dataPollingLoopHelper(void* context);
    void dataPollingLoop();
    void pollData();

    TaskHandle_t _dataPollingTaskHandle = nullptr;
    std::atomic<bool> _dataPollingTaskDone = false;
    bool _stopPollingData = false;
    mutable std::mutex _dataPollingMutex;
    std::condition_variable _dataPollingCv;
    uint32_t _lastDataPoll = 0;

    std::unique_ptr<HttpRequestConfig> _httpRequestConfig;
    std::unique_ptr<HttpGetter> _httpGetter;

    static constexpr int DATA_POLLING_INTERVAL_MS = 3000; // 3 seconds
    static constexpr int HTTP_REQUEST_TIMEOUT_MS = 500; // 500ms

    void setRequestedPowerAc(float power);
    float _requestedPowerAc = 0;
    std::optional<uint16_t> _oPowerLimiterTargetInputPowerWatts = std::nullopt;
    uint32_t _powerLimiterCommandMillis = 0;

    void sendControlCommandRequest();
    void parseControlCommandResponse();
    uint32_t _lastControlCommandRequestMillis = 0;
    static constexpr int CONTROL_COMMAND_INTERVAL_MS = 500; // 500ms

    std::shared_ptr<Stats> _stats = std::make_shared<Stats>();

    DataPointContainer _dataCurrent;

    uint32_t _lastPowerMeterUpdateReceivedMillis = 0; // Timestamp of last seen power meter value
    uint32_t _autoModeBlockedTillMillis = 0;      // Timestamp to block running auto mode for some time
    uint32_t _autoPowerTargetPowerConsumptionZeroHoldTillMillis = 0;

    void holdAutoPowerTargetPowerConsumptionAtZero();
    float getPowerLimiterMaxInputPowerWattsFloat() const;

    bool _autoPowerEnabled = false;
    bool _autoPowerLimitedByAvailablePower = false;
    bool _batteryEmergencyCharging = false;
};

} // namespace GridChargers::Trucki
