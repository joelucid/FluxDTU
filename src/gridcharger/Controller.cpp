// SPDX-License-Identifier: GPL-2.0-or-later

#include <gridcharger/Controller.h>
#include <gridcharger/DummyStats.h>
#include <gridcharger/huawei/Provider.h>
#include <gridcharger/trucki/Provider.h>
#include <battery/Controller.h>
#include <Configuration.h>
#include <MqttSettings.h>
#include <LogHelper.h>
#include <SunPosition.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <limits>

#undef TAG
static const char* TAG = "gridCharger";
static const char* SUBTAG = "Controller";

GridChargers::Controller GridCharger;

namespace GridChargers {

namespace {

constexpr time_t MinimumValidEpoch = 1600000000;
constexpr uint32_t MaximumBatterySocAgeSeconds = 60;
constexpr float SocPlanningBatteryPowerControlGain = 0.25f;
constexpr float SocPlanningBatteryPowerMaxStepWatts = 25.0f;
constexpr float SocPlanningBatteryPowerAntiWindupMarginWatts = 10.0f;
constexpr float SocPlanningCatchUpDeadbandPercent = 0.5f;
constexpr float SocPlanningCatchUpReferenceHorizonHours = 1.0f;

uint8_t clampSoC(uint8_t soc)
{
    return std::min<uint8_t>(soc, 100);
}

uint32_t localDayStart(uint32_t timestamp)
{
    time_t t = timestamp;
    struct tm timeinfo;
    localtime_r(&t, &timeinfo);
    timeinfo.tm_hour = 0;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    return static_cast<uint32_t>(mktime(&timeinfo));
}

void addLiveViewValue(JsonVariant& root,
        char const* name, float value, char const* unit, uint8_t precision)
{
    auto jsonValue = root["values"]["socPlan"][name];
    jsonValue["v"] = value;
    jsonValue["u"] = unit;
    jsonValue["d"] = precision;
}

void addLiveViewText(JsonVariant& root, char const* name, char const* value, bool translate = true)
{
    auto jsonValue = root["values"]["socPlan"][name];
    jsonValue["value"] = value;
    jsonValue["translate"] = translate;
}

} // namespace

void Controller::init(Scheduler& scheduler)
{
    scheduler.addTask(_loopTask);
    _loopTask.setCallback(std::bind(&Controller::loop, this));
    _loopTask.setIterations(TASK_FOREVER);
    _loopTask.enable();

    this->updateSettings();
}

void Controller::updateSettings()
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (_upProvider) {
        _upProvider->deinit();
        _upProvider = nullptr;
    }

    auto const& config = Configuration.get();
    if (!config.GridCharger.Enabled) { return; }

    switch (config.GridCharger.Provider) {
        case GridChargerProviderType::HUAWEI:
            _upProvider = std::make_unique<::GridChargers::Huawei::Provider>();
            break;
        case GridChargerProviderType::TRUCKI:
            _upProvider = std::make_unique<::GridChargers::Trucki::Provider>();
            break;
        default:
            DTU_LOGW("Unknown provider: %d\r\n", config.GridCharger.Provider);
            return;
    }

    if (!_upProvider->init()) { _upProvider = nullptr; }
}

void Controller::loop() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return; }

    _upProvider->loop();

    _upProvider->getStats()->mqttLoop();
}

template<typename T>
T* Controller::getProvider() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_upProvider) return nullptr;
    return static_cast<T*>(_upProvider.get());
}

bool Controller::getAutoPowerStatus() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return false; }

    return _upProvider->getAutoPowerStatus();
}

int16_t Controller::getAutoPowerTargetPowerConsumption() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (_upProvider) { return _upProvider->getAutoPowerTargetPowerConsumption(); }

    auto const target = std::round(Configuration.get().GridCharger.AutoPowerTargetPowerConsumption);
    return static_cast<int16_t>(std::clamp(
            target,
            static_cast<float>(std::numeric_limits<int16_t>::min()),
            static_cast<float>(std::numeric_limits<int16_t>::max())));
}

bool Controller::isAutoPowerTargetPowerConsumptionZeroHoldActive() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return false; }

    return _upProvider->isAutoPowerTargetPowerConsumptionZeroHoldActive();
}

bool Controller::isAutoPowerLimitedByAvailablePower() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return false; }

    return _upProvider->isAutoPowerLimitedByAvailablePower();
}

bool Controller::supportsPowerLimiterControl() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return false; }

    return _upProvider->supportsPowerLimiterControl();
}

std::optional<uint32_t> Controller::getPowerLimiterOutputReferenceMillis() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return 0; }

    return _upProvider->getPowerLimiterOutputReferenceMillis();
}

uint16_t Controller::getPowerLimiterCurrentInputPowerWatts() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return 0; }

    return _upProvider->getPowerLimiterCurrentInputPowerWatts();
}

uint16_t Controller::getPowerLimiterExpectedInputPowerWatts() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return 0; }

    return _upProvider->getPowerLimiterExpectedInputPowerWatts();
}

uint16_t Controller::getPowerLimiterMaxInputPowerWatts() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return 0; }

    return _upProvider->getPowerLimiterMaxInputPowerWatts();
}

uint16_t Controller::applyPowerLimiterInputPowerIncrease(uint16_t increase)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return 0; }

    return _upProvider->applyPowerLimiterInputPowerIncrease(increase);
}

uint16_t Controller::applyPowerLimiterInputPowerReduction(uint16_t reduction)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return 0; }

    return _upProvider->applyPowerLimiterInputPowerReduction(reduction);
}

void Controller::setPowerLimiterLimitedByAvailablePower(bool limited)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return; }

    _upProvider->setPowerLimiterLimitedByAvailablePower(limited);
}

uint8_t Controller::getAutoPowerStopBatterySoCThreshold() const
{
    auto const plan = getAutoPowerSocPlan();
    if (plan.powerLimitAvailable && plan.plannedBatteryChargePowerWatts > 0.0f) {
        return plan.staticStopSoC;
    }

    return plan.effectiveStopSoC;
}

std::optional<float> Controller::getAutoPowerPlannedSoC(uint32_t timestamp) const
{
    auto const& config = Configuration.get();
    if (!config.GridCharger.AutoPowerSocPlanningEnabled
            || !config.GridCharger.Enabled
            || !config.GridCharger.AutoPowerEnabled
            || !config.GridCharger.AutoPowerBatterySoCLimitsEnabled
            || !config.Battery.Enabled) {
        return std::nullopt;
    }

    auto const staticStopSoC = clampSoC(config.GridCharger.AutoPowerStopBatterySoCThreshold);
    auto const dayMinSoC = clampSoC(config.GridCharger.AutoPowerSocPlanningDayMinSoC);
    auto const nightTargetSoC = clampSoC(config.GridCharger.AutoPowerSocPlanningNightTargetSoC);
    auto const startAfterSunrise = config.GridCharger.AutoPowerSocPlanningStartAfterSunrise;
    auto const finishBeforeSunset = config.GridCharger.AutoPowerSocPlanningFinishBeforeSunset;
    if (dayMinSoC > nightTargetSoC || timestamp < MinimumValidEpoch) {
        return std::nullopt;
    }

    auto const dayStart = localDayStart(timestamp);
    std::lock_guard<std::mutex> lock(_mutex);
    const bool cacheMatches = _autoPowerPlannedSocCache.valid
        && _autoPowerPlannedSocCache.dayStart == dayStart
        && _autoPowerPlannedSocCache.staticStopSoC == staticStopSoC
        && _autoPowerPlannedSocCache.dayMinSoC == dayMinSoC
        && _autoPowerPlannedSocCache.nightTargetSoC == nightTargetSoC
        && _autoPowerPlannedSocCache.startAfterSunrise == startAfterSunrise
        && _autoPowerPlannedSocCache.finishBeforeSunset == finishBeforeSunset;

    if (!cacheMatches) {
        _autoPowerPlannedSocCache = AutoPowerPlannedSocCache();
        _autoPowerPlannedSocCache.valid = true;
        _autoPowerPlannedSocCache.dayStart = dayStart;
        _autoPowerPlannedSocCache.staticStopSoC = staticStopSoC;
        _autoPowerPlannedSocCache.dayMinSoC = dayMinSoC;
        _autoPowerPlannedSocCache.nightTargetSoC = nightTargetSoC;
        _autoPowerPlannedSocCache.startAfterSunrise = startAfterSunrise;
        _autoPowerPlannedSocCache.finishBeforeSunset = finishBeforeSunset;

        time_t const sampleTime = static_cast<time_t>(timestamp);
        struct tm sunrise;
        struct tm sunset;
        if (SunPosition.sunTimes(&sunrise, &sunset, sampleTime)) {
            time_t start = mktime(&sunrise)
                + static_cast<time_t>(startAfterSunrise) * 60;
            time_t finish = mktime(&sunset)
                - static_cast<time_t>(finishBeforeSunset) * 60;
            if (finish > start) {
                _autoPowerPlannedSocCache.windowValid = true;
                _autoPowerPlannedSocCache.startTime = static_cast<uint32_t>(start);
                _autoPowerPlannedSocCache.finishTime = static_cast<uint32_t>(finish);
            }
        }
    }

    if (!_autoPowerPlannedSocCache.windowValid) {
        return std::nullopt;
    }

    time_t const sampleTime = static_cast<time_t>(timestamp);
    time_t const start = static_cast<time_t>(_autoPowerPlannedSocCache.startTime);
    time_t const finish = static_cast<time_t>(_autoPowerPlannedSocCache.finishTime);
    float plannedSoC = dayMinSoC;
    if (sampleTime >= finish) {
        plannedSoC = nightTargetSoC;
    } else if (sampleTime >= start) {
        float const progress = static_cast<float>(sampleTime - start)
            / static_cast<float>(finish - start);
        plannedSoC = dayMinSoC
            + (static_cast<float>(nightTargetSoC) - dayMinSoC) * progress;
    }

    plannedSoC = std::clamp(plannedSoC, 0.0f, 100.0f);
    return std::min<float>(staticStopSoC, plannedSoC);
}

std::optional<float> Controller::getAutoPowerSocPlanningOutputPowerLimit(float currentOutputPower) const
{
    auto const plan = getAutoPowerSocPlan();
    if (!plan.powerLimitAvailable) {
        _autoPowerSocPlanningOutputPowerBiasWatts = 0.0f;
        return std::nullopt;
    }
    if (plan.plannedBatteryChargePowerWatts <= 0.0f) {
        _autoPowerSocPlanningOutputPowerBiasWatts = 0.0f;
        return 0.0f;
    }

    auto const& config = Configuration.get();
    auto const plannedBatteryChargePower = plan.plannedBatteryChargePowerWatts;
    auto const upperPowerLimit = static_cast<float>(config.GridCharger.AutoPowerUpperPowerLimit);

    _autoPowerSocPlanningOutputPowerBiasWatts = std::clamp(
            _autoPowerSocPlanningOutputPowerBiasWatts,
            -plannedBatteryChargePower,
            upperPowerLimit - plannedBatteryChargePower);

    float outputPowerLimit = plannedBatteryChargePower + _autoPowerSocPlanningOutputPowerBiasWatts;
    if (plan.actualBatteryChargePowerAvailable) {
        auto const batteryPowerError = plannedBatteryChargePower
            - plan.actualBatteryChargePowerWatts;

        auto const currentOutput = std::max(0.0f, currentOutputPower);
        bool const outputLimitIsBinding = currentOutput
            >= outputPowerLimit - SocPlanningBatteryPowerAntiWindupMarginWatts;
        if (batteryPowerError <= 0.0f || outputLimitIsBinding) {
            auto biasStep = batteryPowerError * SocPlanningBatteryPowerControlGain;
            biasStep = std::clamp(
                    biasStep,
                    -SocPlanningBatteryPowerMaxStepWatts,
                    SocPlanningBatteryPowerMaxStepWatts);

            _autoPowerSocPlanningOutputPowerBiasWatts += biasStep;
            _autoPowerSocPlanningOutputPowerBiasWatts = std::clamp(
                    _autoPowerSocPlanningOutputPowerBiasWatts,
                    -plannedBatteryChargePower,
                    upperPowerLimit - plannedBatteryChargePower);
            outputPowerLimit = plannedBatteryChargePower + _autoPowerSocPlanningOutputPowerBiasWatts;
        }

        auto const lowerPowerLimit = static_cast<float>(config.GridCharger.AutoPowerLowerPowerLimit);
        if (batteryPowerError > 0.0f && outputPowerLimit > 0.0f
                && outputPowerLimit < lowerPowerLimit) {
            outputPowerLimit = batteryPowerError >= lowerPowerLimit * 0.5f
                ? lowerPowerLimit
                : 0.0f;
        }
    }

    return std::clamp(
            outputPowerLimit,
            0.0f,
            upperPowerLimit);
}

Controller::AutoPowerSocPlan Controller::getAutoPowerSocPlan() const
{
    AutoPowerSocPlan plan;

    auto const& config = Configuration.get();
    plan.staticStopSoC = clampSoC(config.GridCharger.AutoPowerStopBatterySoCThreshold);
    plan.effectiveStopSoC = plan.staticStopSoC;
    plan.dayMinSoC = clampSoC(config.GridCharger.AutoPowerSocPlanningDayMinSoC);
    plan.nightTargetSoC = clampSoC(config.GridCharger.AutoPowerSocPlanningNightTargetSoC);
    plan.plannedStopSoC = plan.staticStopSoC;
    plan.configured = config.GridCharger.AutoPowerSocPlanningEnabled;

    if (!plan.configured) {
        return plan;
    }

    plan.status = "fallbackDisabled";

    if (!config.GridCharger.Enabled || !config.GridCharger.AutoPowerEnabled
            || !config.GridCharger.AutoPowerBatterySoCLimitsEnabled) {
        return plan;
    }

    if (plan.dayMinSoC > plan.nightTargetSoC) {
        plan.status = "invalidSocRange";
        return plan;
    }

    if (!config.Battery.Enabled) {
        plan.status = "batteryDisabled";
        return plan;
    }

    auto stats = Battery.getStats();
    if (!stats->isSoCValid() || stats->getSoCAgeSeconds() > MaximumBatterySocAgeSeconds) {
        plan.status = "socUnavailable";
        return plan;
    }

    time_t const now = time(nullptr);
    if (now < MinimumValidEpoch) {
        plan.status = "timeUnavailable";
        return plan;
    }

    struct tm sunrise;
    struct tm sunset;
    if (!SunPosition.sunriseTime(&sunrise) || !SunPosition.sunsetTime(&sunset)) {
        plan.status = "sunUnavailable";
        return plan;
    }

    time_t start = mktime(&sunrise)
        + static_cast<time_t>(config.GridCharger.AutoPowerSocPlanningStartAfterSunrise) * 60;
    time_t finish = mktime(&sunset)
        - static_cast<time_t>(config.GridCharger.AutoPowerSocPlanningFinishBeforeSunset) * 60;

    if (finish <= start) {
        plan.status = "invalidWindow";
        return plan;
    }

    plan.valid = true;
    plan.currentSoC = stats->getSoC();
    plan.startTime = static_cast<uint32_t>(start);
    plan.finishTime = static_cast<uint32_t>(finish);
    plan.secondsUntilStart = static_cast<int32_t>(start - now);
    plan.secondsUntilFinish = static_cast<int32_t>(finish - now);

    float plannedStopSoC = plan.dayMinSoC;
    if (now < start) {
        plan.status = "beforeWindow";
    } else if (now >= finish) {
        plan.status = "afterWindow";
        plannedStopSoC = plan.nightTargetSoC;
    } else {
        plan.status = "active";
        plan.active = true;
        float const progress = static_cast<float>(now - start)
            / static_cast<float>(finish - start);
        plannedStopSoC = plan.dayMinSoC
            + (static_cast<float>(plan.nightTargetSoC) - plan.dayMinSoC) * progress;
    }

    plannedStopSoC = std::clamp(plannedStopSoC, 0.0f, 100.0f);
    plan.plannedStopSoC = plannedStopSoC;
    plan.effectiveStopSoC = std::min<uint8_t>(
            plan.staticStopSoC,
            static_cast<uint8_t>(std::ceil(plannedStopSoC)));

    if (stats->isVoltageValid() && stats->isCurrentValid()
            && stats->getVoltageAgeSeconds() <= MaximumBatterySocAgeSeconds
            && stats->getChargeCurrentAgeSeconds() <= MaximumBatterySocAgeSeconds) {
        plan.actualBatteryChargePowerAvailable = true;
        plan.actualBatteryChargePowerWatts = std::max(
                0.0f,
                stats->getVoltage() * stats->getChargeCurrent());
    }

    auto const endTargetSoC = std::min<uint8_t>(plan.nightTargetSoC, plan.staticStopSoC);
    if (config.GridCharger.AutoPowerSocPlanningBatteryCapacity > 0) {
        auto const missingSoC = std::max(0.0f, static_cast<float>(endTargetSoC) - plan.currentSoC);
        plan.missingEnergyWh = static_cast<float>(config.GridCharger.AutoPowerSocPlanningBatteryCapacity)
            * missingSoC / 100.0f;
        plan.missingEnergyAvailable = true;
    }

    if (!config.GridCharger.AutoPowerSocPlanningPowerLimitEnabled
            || config.GridCharger.AutoPowerSocPlanningBatteryCapacity == 0
            || now >= finish) {
        return plan;
    }

    auto const currentPlanTargetSoC = std::min<float>(
            plan.plannedStopSoC,
            static_cast<float>(plan.staticStopSoC));
    auto const socDeficit = currentPlanTargetSoC - plan.currentSoC;

    float plannedPowerLimit = 0.0f;

    if (plan.active && plan.missingEnergyWh > 0.0f) {
        float const remainingHours = static_cast<float>(finish - now) / 3600.0f;
        if (remainingHours <= 0.0f) { return plan; }

        plannedPowerLimit = plan.missingEnergyWh / remainingHours;
    }

    if (socDeficit > SocPlanningCatchUpDeadbandPercent) {
        auto const catchUpSoC = socDeficit - SocPlanningCatchUpDeadbandPercent;
        auto const catchUpEnergyWh = static_cast<float>(config.GridCharger.AutoPowerSocPlanningBatteryCapacity)
            * catchUpSoC / 100.0f;
        auto const catchUpMultiplier = std::max(0.0f, config.GridCharger.AutoPowerSocPlanningCatchUpMultiplier);
        plannedPowerLimit += catchUpEnergyWh * catchUpMultiplier / SocPlanningCatchUpReferenceHorizonHours;
    }

    if (plannedPowerLimit > 0.0f && config.GridCharger.AutoPowerSocPlanningMinimumPowerLimit > 0) {
        plannedPowerLimit = std::max<float>(
                plannedPowerLimit,
                config.GridCharger.AutoPowerSocPlanningMinimumPowerLimit);
    }

    plan.powerLimitAvailable = true;
    plan.plannedBatteryChargePowerWatts = std::min<float>(
            plannedPowerLimit,
            config.GridCharger.AutoPowerUpperPowerLimit);
    return plan;
}

void Controller::getAutoPowerSocPlanLiveViewData(JsonVariant& root) const
{
    auto const plan = getAutoPowerSocPlan();
    if (!plan.configured) { return; }

    addLiveViewText(root, "status", plan.status);
    addLiveViewValue(root, "effectiveStopSoC", plan.effectiveStopSoC, "%", 0);
    addLiveViewValue(root, "plannedStopSoC", plan.plannedStopSoC, "%", 1);
    addLiveViewValue(root, "dayMinSoC", plan.dayMinSoC, "%", 0);
    addLiveViewValue(root, "nightTargetSoC", plan.nightTargetSoC, "%", 0);
    addLiveViewValue(root, "staticStopSoC", plan.staticStopSoC, "%", 0);

    if (plan.valid) {
        addLiveViewValue(root, "currentSoC", plan.currentSoC, "%", 1);
        addLiveViewValue(root, "secondsUntilStart", plan.secondsUntilStart, "s", 0);
        addLiveViewValue(root, "secondsUntilFinish", plan.secondsUntilFinish, "s", 0);
    }

    if (plan.missingEnergyAvailable) {
        addLiveViewValue(root, "missingEnergy", plan.missingEnergyWh, "Wh", 0);
    }

    if (plan.powerLimitAvailable) {
        addLiveViewValue(root, "plannedBatteryChargePower", plan.plannedBatteryChargePowerWatts, "W", 0);
    }

    if (plan.actualBatteryChargePowerAvailable) {
        addLiveViewValue(root, "actualBatteryChargePower", plan.actualBatteryChargePowerWatts, "W", 0);
    }
}

std::shared_ptr<Stats const> Controller::getStats() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) {
        static auto sspDummyStats = std::make_shared<DummyStats>();
        return sspDummyStats;
    }

    return _upProvider->getStats();
}

// Template instantiations
template GridChargers::Huawei::Provider* Controller::getProvider<GridChargers::Huawei::Provider>() const;

} // namespace GridChargers
