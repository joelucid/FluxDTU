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
constexpr float SocPlanningBatteryPowerControlGain = 0.5f;
constexpr float SocPlanningBatteryPowerMaxStepWatts = 50.0f;
constexpr float SocPlanningBatteryPowerAntiWindupMarginWatts = 5.0f;
constexpr float SocPlanningBatteryPowerChargerFeedbackMarginWatts = 2.0f;
constexpr float SocPlanningCatchUpDeadbandPercent = 0.5f;
constexpr float SocPlanningCatchUpReferenceHorizonHours = 1.0f;
constexpr uint16_t SocPlanAvailablePowerLimitMinimumHeadroomWatts = 10;

uint8_t clampSoC(uint8_t soc)
{
    return std::min<uint8_t>(soc, Controller::MaxConfiguredSoCPercent);
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

float interpolateSoC(time_t sample, time_t start, time_t finish, float startSoC, float finishSoC)
{
    if (finish <= start || sample >= finish) { return finishSoC; }
    if (sample <= start) { return startSoC; }

    float const progress = static_cast<float>(sample - start)
        / static_cast<float>(finish - start);
    return startSoC + (finishSoC - startSoC) * progress;
}

float plannedSoCAt(time_t sample,
        time_t start,
        time_t intermediate,
        time_t finalRampStart,
        time_t finish,
        uint8_t dayMinSoC,
        uint8_t intermediateTargetSoC,
        uint8_t nightTargetSoC)
{
    if (sample < start) { return dayMinSoC; }
    if (sample >= finish) { return nightTargetSoC; }

    if (intermediate > start && sample < intermediate) {
        return interpolateSoC(sample, start, intermediate, dayMinSoC, intermediateTargetSoC);
    }

    time_t const holdStart = std::max(start, intermediate);
    if (finalRampStart > holdStart && sample < finalRampStart) {
        return intermediateTargetSoC;
    }

    time_t const secondStageStart = std::max(holdStart, finalRampStart);
    return interpolateSoC(sample, secondStageStart, finish, intermediateTargetSoC, nightTargetSoC);
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

bool Controller::isAutoPowerLimitedByAvailablePower() const
{
    {
        std::lock_guard<std::mutex> lock(_mutex);

        if (!_upProvider) { return false; }
        if (_upProvider->isAutoPowerLimitedByAvailablePower()) { return true; }
    }

    auto const plan = getAutoPowerSocPlan();
    if (!plan.active
            || !plan.powerLimitAvailable
            || plan.plannedBatteryChargePowerWatts <= 0.0f) {
        return false;
    }

    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return false; }

    auto const expectedInputPower = _upProvider->getPowerLimiterExpectedInputPowerWatts();
    auto const maxInputPower = _upProvider->getPowerLimiterMaxInputPowerWatts();
    if (maxInputPower <= expectedInputPower) { return false; }

    return (maxInputPower - expectedInputPower)
        >= SocPlanAvailablePowerLimitMinimumHeadroomWatts;
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

std::optional<uint16_t> Controller::getPowerLimiterTargetInputPowerWatts() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return std::nullopt; }

    return _upProvider->getPowerLimiterTargetInputPowerWatts();
}

uint16_t Controller::getPowerLimiterMaxInputPowerWatts() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return 0; }

    return _upProvider->getPowerLimiterMaxInputPowerWatts();
}

std::optional<PowerLimiterControlProposal> Controller::getPowerLimiterControlProposal(
        uint16_t targetInputPowerWatts) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return std::nullopt; }

    return _upProvider->getPowerLimiterControlProposal(targetInputPowerWatts);
}

std::vector<PowerLimiterTargetDispatchEvent> Controller::consumePowerLimiterTargetDispatchEvents()
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upProvider) { return {}; }

    return _upProvider->consumePowerLimiterTargetDispatchEvents();
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
    auto const intermediateTargetSoC = clampSoC(config.GridCharger.AutoPowerSocPlanningIntermediateTargetSoC);
    auto const nightTargetSoC = clampSoC(config.GridCharger.AutoPowerSocPlanningNightTargetSoC);
    auto const startAfterSunrise = config.GridCharger.AutoPowerSocPlanningStartAfterSunrise;
    auto const intermediateBeforeSunset = config.GridCharger.AutoPowerSocPlanningIntermediateBeforeSunset;
    auto const finalRampStartBeforeSunset = config.GridCharger.AutoPowerSocPlanningFinalRampStartBeforeSunset;
    auto const finishBeforeSunset = config.GridCharger.AutoPowerSocPlanningFinishBeforeSunset;
    if (dayMinSoC > intermediateTargetSoC
            || intermediateTargetSoC > nightTargetSoC
            || intermediateBeforeSunset < finalRampStartBeforeSunset
            || finalRampStartBeforeSunset < finishBeforeSunset
            || (finalRampStartBeforeSunset == finishBeforeSunset
                && intermediateTargetSoC != nightTargetSoC)
            || timestamp < MinimumValidEpoch) {
        return std::nullopt;
    }

    auto const dayStart = localDayStart(timestamp);
    std::lock_guard<std::mutex> lock(_mutex);
    const bool cacheMatches = _autoPowerPlannedSocCache.valid
        && _autoPowerPlannedSocCache.dayStart == dayStart
        && _autoPowerPlannedSocCache.staticStopSoC == staticStopSoC
        && _autoPowerPlannedSocCache.dayMinSoC == dayMinSoC
        && _autoPowerPlannedSocCache.intermediateTargetSoC == intermediateTargetSoC
        && _autoPowerPlannedSocCache.nightTargetSoC == nightTargetSoC
        && _autoPowerPlannedSocCache.startAfterSunrise == startAfterSunrise
        && _autoPowerPlannedSocCache.intermediateBeforeSunset == intermediateBeforeSunset
        && _autoPowerPlannedSocCache.finalRampStartBeforeSunset == finalRampStartBeforeSunset
        && _autoPowerPlannedSocCache.finishBeforeSunset == finishBeforeSunset;

    if (!cacheMatches) {
        _autoPowerPlannedSocCache = AutoPowerPlannedSocCache();
        _autoPowerPlannedSocCache.valid = true;
        _autoPowerPlannedSocCache.dayStart = dayStart;
        _autoPowerPlannedSocCache.staticStopSoC = staticStopSoC;
        _autoPowerPlannedSocCache.dayMinSoC = dayMinSoC;
        _autoPowerPlannedSocCache.intermediateTargetSoC = intermediateTargetSoC;
        _autoPowerPlannedSocCache.nightTargetSoC = nightTargetSoC;
        _autoPowerPlannedSocCache.startAfterSunrise = startAfterSunrise;
        _autoPowerPlannedSocCache.intermediateBeforeSunset = intermediateBeforeSunset;
        _autoPowerPlannedSocCache.finalRampStartBeforeSunset = finalRampStartBeforeSunset;
        _autoPowerPlannedSocCache.finishBeforeSunset = finishBeforeSunset;

        time_t const sampleTime = static_cast<time_t>(timestamp);
        struct tm sunrise;
        struct tm sunset;
        if (SunPosition.sunTimes(&sunrise, &sunset, sampleTime)) {
            time_t start = mktime(&sunrise)
                + static_cast<time_t>(startAfterSunrise) * 60;
            time_t intermediate = mktime(&sunset)
                - static_cast<time_t>(intermediateBeforeSunset) * 60;
            time_t finalRampStart = mktime(&sunset)
                - static_cast<time_t>(finalRampStartBeforeSunset) * 60;
            time_t finish = mktime(&sunset)
                - static_cast<time_t>(finishBeforeSunset) * 60;
            if (finish > start && intermediate <= finalRampStart && finalRampStart <= finish) {
                _autoPowerPlannedSocCache.windowValid = true;
                _autoPowerPlannedSocCache.startTime = static_cast<uint32_t>(start);
                _autoPowerPlannedSocCache.intermediateTime = static_cast<uint32_t>(intermediate);
                _autoPowerPlannedSocCache.finalRampStartTime = static_cast<uint32_t>(finalRampStart);
                _autoPowerPlannedSocCache.finishTime = static_cast<uint32_t>(finish);
            }
        }
    }

    if (!_autoPowerPlannedSocCache.windowValid) {
        return std::nullopt;
    }

    time_t const sampleTime = static_cast<time_t>(timestamp);
    time_t const start = static_cast<time_t>(_autoPowerPlannedSocCache.startTime);
    time_t const intermediate = static_cast<time_t>(_autoPowerPlannedSocCache.intermediateTime);
    time_t const finalRampStart = static_cast<time_t>(_autoPowerPlannedSocCache.finalRampStartTime);
    time_t const finish = static_cast<time_t>(_autoPowerPlannedSocCache.finishTime);
    float plannedSoC = plannedSoCAt(
            sampleTime,
            start,
            intermediate,
            finalRampStart,
            finish,
            dayMinSoC,
            intermediateTargetSoC,
            nightTargetSoC);
    plannedSoC = std::clamp(
            plannedSoC,
            0.0f,
            static_cast<float>(Controller::MaxConfiguredSoCPercent));
    return std::min<float>(staticStopSoC, plannedSoC);
}

std::optional<float> Controller::getAutoPowerSocPlanningOutputPowerLimit(
        float currentOutputPower,
        std::optional<float> requestedOutputPower) const
{
    auto const plan = getAutoPowerSocPlan();
    if (!plan.powerLimitAvailable) {
        _autoPowerSocPlanningOutputPowerBiasWatts = 0.0f;
        _autoPowerSocPlanningLastBatteryPowerUpdateMillis = 0;
        return std::nullopt;
    }
    if (plan.plannedBatteryChargePowerWatts <= 0.0f) {
        _autoPowerSocPlanningOutputPowerBiasWatts = 0.0f;
        _autoPowerSocPlanningLastBatteryPowerUpdateMillis = 0;
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
        auto const antiWindupMargin = plan.actualBatteryChargePowerFromGridChargerOutput
            ? SocPlanningBatteryPowerChargerFeedbackMarginWatts
            : SocPlanningBatteryPowerAntiWindupMarginWatts;
        auto const batteryPowerError = plannedBatteryChargePower
            - plan.actualBatteryChargePowerWatts;

        auto const currentOutput = std::max(0.0f, currentOutputPower);
        auto const requestedOutput = requestedOutputPower
            ? std::max(0.0f, *requestedOutputPower)
            : currentOutput;
        // A charger can be command-limited before telemetry shows output. Treat
        // the requested output as binding so an undercharging plan can recover.
        auto const controlOutput = std::max(currentOutput, requestedOutput);
        bool const plannedLimitIsExceeded = controlOutput
            > plannedBatteryChargePower + antiWindupMargin;
        if (plannedLimitIsExceeded && _autoPowerSocPlanningOutputPowerBiasWatts < 0.0f) {
            _autoPowerSocPlanningOutputPowerBiasWatts = 0.0f;
            outputPowerLimit = plannedBatteryChargePower;
        }

        bool const outputLimitIsBinding = controlOutput
            >= outputPowerLimit - antiWindupMargin;
        bool const outputLimitIsObeyed = controlOutput
            <= outputPowerLimit + antiWindupMargin;
        bool const freshBatteryPower =
            plan.actualBatteryChargePowerUpdateMillis != 0
            && plan.actualBatteryChargePowerUpdateMillis
                != _autoPowerSocPlanningLastBatteryPowerUpdateMillis;
        bool const mayUpdateBias = batteryPowerError > 0.0f
            ? outputLimitIsBinding
            : outputLimitIsObeyed;
        if (freshBatteryPower && mayUpdateBias) {
            _autoPowerSocPlanningLastBatteryPowerUpdateMillis =
                plan.actualBatteryChargePowerUpdateMillis;
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

        // Delayed battery telemetry can drive the learned bias down while the
        // charger is already ramping down. Once the measured charge is below
        // plan again, reopen at least the planned cap so DPL can use surplus.
        if (batteryPowerError > 0.0f && outputPowerLimit < plannedBatteryChargePower) {
            _autoPowerSocPlanningOutputPowerBiasWatts = std::max(
                    _autoPowerSocPlanningOutputPowerBiasWatts,
                    0.0f);
            outputPowerLimit = plannedBatteryChargePower;
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
    plan.reenableSoC = std::min<uint8_t>(
            clampSoC(config.GridCharger.AutoPowerReenableBatterySoCThreshold),
            plan.staticStopSoC > 0 ? plan.staticStopSoC - 1 : 0);
    plan.effectiveStopSoC = plan.staticStopSoC;
    plan.dayMinSoC = clampSoC(config.GridCharger.AutoPowerSocPlanningDayMinSoC);
    plan.intermediateTargetSoC = clampSoC(config.GridCharger.AutoPowerSocPlanningIntermediateTargetSoC);
    plan.nightTargetSoC = clampSoC(config.GridCharger.AutoPowerSocPlanningNightTargetSoC);
    plan.chargeTargetSoC = plan.nightTargetSoC;
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

    if (plan.dayMinSoC > plan.intermediateTargetSoC
            || plan.intermediateTargetSoC > plan.nightTargetSoC) {
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
    time_t intermediate = mktime(&sunset)
        - static_cast<time_t>(config.GridCharger.AutoPowerSocPlanningIntermediateBeforeSunset) * 60;
    time_t finalRampStart = mktime(&sunset)
        - static_cast<time_t>(config.GridCharger.AutoPowerSocPlanningFinalRampStartBeforeSunset) * 60;
    time_t finish = mktime(&sunset)
        - static_cast<time_t>(config.GridCharger.AutoPowerSocPlanningFinishBeforeSunset) * 60;

    if (finish <= start
            || intermediate > finalRampStart
            || finalRampStart > finish
            || (finalRampStart == finish
                && plan.intermediateTargetSoC != plan.nightTargetSoC)) {
        plan.status = "invalidWindow";
        return plan;
    }

    plan.valid = true;
    plan.currentSoC = stats->getSoC();
    plan.startTime = static_cast<uint32_t>(start);
    plan.intermediateTime = static_cast<uint32_t>(intermediate);
    plan.finalRampStartTime = static_cast<uint32_t>(finalRampStart);
    plan.finishTime = static_cast<uint32_t>(finish);
    plan.secondsUntilStart = static_cast<int32_t>(start - now);
    plan.secondsUntilIntermediate = static_cast<int32_t>(intermediate - now);
    plan.secondsUntilFinalRampStart = static_cast<int32_t>(finalRampStart - now);
    plan.secondsUntilFinish = static_cast<int32_t>(finish - now);
    if (now < finalRampStart) {
        plan.chargeTargetSoC = plan.intermediateTargetSoC;
        plan.chargeTargetTime = static_cast<uint32_t>(
                now < intermediate ? intermediate : finalRampStart);
    } else {
        plan.chargeTargetSoC = plan.nightTargetSoC;
        plan.chargeTargetTime = static_cast<uint32_t>(finish);
    }
    plan.secondsUntilChargeTarget = static_cast<int32_t>(
            static_cast<time_t>(plan.chargeTargetTime) - now);

    float plannedStopSoC = plan.dayMinSoC;
    if (now < start) {
        plan.status = "beforeWindow";
    } else if (now >= finish) {
        plan.status = "afterWindow";
        plannedStopSoC = plan.nightTargetSoC;
    } else {
        plan.status = "active";
        plan.active = true;
        plannedStopSoC = plannedSoCAt(
                now,
                start,
                intermediate,
                finalRampStart,
                finish,
                plan.dayMinSoC,
                plan.intermediateTargetSoC,
                plan.nightTargetSoC);
    }

    plannedStopSoC = std::clamp(plannedStopSoC, 0.0f, static_cast<float>(Controller::MaxConfiguredSoCPercent));
    plan.plannedStopSoC = plannedStopSoC;
    plan.effectiveStopSoC = std::min<uint8_t>(
            plan.staticStopSoC,
            static_cast<uint8_t>(std::ceil(plannedStopSoC)));
    plan.reenableSoC = std::min<uint8_t>(
            plan.reenableSoC,
            plan.effectiveStopSoC > 0 ? plan.effectiveStopSoC - 1 : 0);

    if (config.GridCharger.AutoPowerIgnoreBmsCurrent && _upProvider) {
        auto const chargerStats = _upProvider->getStats();
        auto const chargerStatsMillis = chargerStats->getLastUpdate();
        auto const chargerOutputPower = chargerStats->getOutputPower();
        if (chargerOutputPower
                && chargerStatsMillis != 0
                && (millis() - chargerStatsMillis)
                    <= MaximumBatterySocAgeSeconds * 1000UL) {
            plan.actualBatteryChargePowerAvailable = true;
            plan.actualBatteryChargePowerWatts = std::max(0.0f, *chargerOutputPower);
            plan.actualBatteryChargePowerUpdateMillis = chargerStatsMillis;
            plan.actualBatteryChargePowerSource = "gridChargerOutput";
            plan.actualBatteryChargePowerFromGridChargerOutput = true;
        }
    }

    if (!plan.actualBatteryChargePowerAvailable
            && stats->isVoltageValid() && stats->isCurrentValid()
            && stats->getVoltageAgeSeconds() <= MaximumBatterySocAgeSeconds
            && stats->getChargeCurrentAgeSeconds() <= MaximumBatterySocAgeSeconds) {
        plan.actualBatteryChargePowerAvailable = true;
        plan.actualBatteryChargePowerWatts = std::max(
                0.0f,
                stats->getVoltage() * stats->getChargeCurrent());
        plan.actualBatteryChargePowerUpdateMillis = std::min(
                stats->getVoltageUpdateMillis(),
                stats->getChargeCurrentUpdateMillis());
        plan.actualBatteryChargePowerSource = "batteryBms";
    }

    auto const endTargetSoC = std::min<uint8_t>(plan.chargeTargetSoC, plan.staticStopSoC);
    if (config.GridCharger.AutoPowerSocPlanningBatteryCapacity > 0) {
        auto const missingSoC = std::max(0.0f, static_cast<float>(endTargetSoC) - plan.currentSoC);
        plan.missingEnergyWh = static_cast<float>(config.GridCharger.AutoPowerSocPlanningBatteryCapacity)
            * missingSoC / 100.0f;
        plan.missingEnergyAvailable = true;
    }

    if (!config.GridCharger.AutoPowerSocPlanningPowerLimitEnabled
            || config.GridCharger.AutoPowerSocPlanningBatteryCapacity == 0
            || now >= static_cast<time_t>(plan.chargeTargetTime)) {
        return plan;
    }

    auto const currentPlanTargetSoC = std::min<float>(
            plan.plannedStopSoC,
            static_cast<float>(plan.staticStopSoC));
    auto const socDeficit = currentPlanTargetSoC - plan.currentSoC;

    float plannedPowerLimit = 0.0f;

    if (plan.active && plan.missingEnergyWh > 0.0f) {
        float const remainingHours = static_cast<float>(
                static_cast<time_t>(plan.chargeTargetTime) - now) / 3600.0f;
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
    addLiveViewValue(root, "intermediateTargetSoC", plan.intermediateTargetSoC, "%", 0);
    addLiveViewValue(root, "nightTargetSoC", plan.nightTargetSoC, "%", 0);
    addLiveViewValue(root, "chargeTargetSoC", plan.chargeTargetSoC, "%", 0);
    addLiveViewValue(root, "staticStopSoC", plan.staticStopSoC, "%", 0);
    addLiveViewValue(root, "reenableSoC", plan.reenableSoC, "%", 0);

    if (plan.valid) {
        addLiveViewValue(root, "currentSoC", plan.currentSoC, "%", 1);
        addLiveViewValue(root, "secondsUntilStart", plan.secondsUntilStart, "s", 0);
        addLiveViewValue(root, "secondsUntilIntermediate", plan.secondsUntilIntermediate, "s", 0);
        addLiveViewValue(root, "secondsUntilFinalRampStart", plan.secondsUntilFinalRampStart, "s", 0);
        addLiveViewValue(root, "secondsUntilFinish", plan.secondsUntilFinish, "s", 0);
        addLiveViewValue(root, "secondsUntilChargeTarget", plan.secondsUntilChargeTarget, "s", 0);
    }

    if (plan.missingEnergyAvailable) {
        addLiveViewValue(root, "missingEnergy", plan.missingEnergyWh, "Wh", 0);
    }

    if (plan.powerLimitAvailable) {
        addLiveViewValue(root, "plannedBatteryChargePower", plan.plannedBatteryChargePowerWatts, "W", 0);
    }

    if (plan.actualBatteryChargePowerAvailable) {
        addLiveViewValue(root, "actualBatteryChargePower", plan.actualBatteryChargePowerWatts, "W", 0);
        addLiveViewText(root, "actualBatteryChargePowerSource", plan.actualBatteryChargePowerSource);
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
