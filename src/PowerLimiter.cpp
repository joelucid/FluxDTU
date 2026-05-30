// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Thomas Basler and others
 */

#include <battery/Controller.h>
#include <battery/Stats.h>
#include <powermeter/Controller.h>
#include "PowerLimiter.h"
#include "Configuration.h"
#include "MqttSettings.h"
#include "NetworkSettings.h"
#include <gridcharger/Controller.h>
#include <solarcharger/Controller.h>
#include <algorithm>
#include <ctime>
#include <cmath>
#include <limits>
#include <vector>
#include <frozen/map.h>
#include "SunPosition.h"
#include <LogHelper.h>

#undef TAG
static const char* TAG = "dynamicPowerLimiter";
static const char* SUBTAG = "Controller";

static auto sBatteryPoweredFilter = [](PowerLimiterInverter const& inv) {
    return inv.isBatteryPowered();
};

static const char sBatteryPoweredExpression[] = "battery-powered";

static auto sSolarPoweredFilter = [](PowerLimiterInverter const& inv) {
    return inv.isSolarPowered();
};

static const char sSolarPoweredExpression[] = "solar-powered";

static auto sSmartBufferPoweredFilter = [](PowerLimiterInverter const& inv) {
    return inv.isSmartBufferPowered();
};

static const char sSmartBufferPoweredExpression[] = "smart-buffer-powered";

static bool millisAtOrAfter(uint32_t timestamp, uint32_t reference)
{
    auto constexpr halfOfAllMillis = std::numeric_limits<uint32_t>::max() / 2;
    return (timestamp - reference) < halfOfAllMillis;
}

static float calcExponentialSmoothingAlpha(uint32_t elapsedMillis, uint16_t timeConstantSeconds)
{
    if (elapsedMillis == 0 || timeConstantSeconds == 0) { return 0.0f; }

    auto const elapsedSeconds = static_cast<float>(elapsedMillis) / 1000.0f;
    auto const timeConstant = static_cast<float>(timeConstantSeconds);
    return std::clamp(1.0f - std::exp(-elapsedSeconds / timeConstant), 0.0f, 1.0f);
}

static uint32_t latestMillis(uint32_t a, uint32_t b)
{
    return millisAtOrAfter(a, b) ? a : b;
}

static uint16_t ceilPositiveWattsToUint16(float watts)
{
    if (watts <= 0.0f) { return 0; }
    return static_cast<uint16_t>(std::min<float>(
            std::ceil(watts),
            std::numeric_limits<uint16_t>::max()));
}

struct DistributedChange {
    PowerLimiterInverter* inverter;
    uint16_t capacity;
    uint16_t amount = 0;
    uint16_t weight = 1000;
};

enum class ThermalPreference {
    Cooler,
    Hotter,
};

static constexpr uint16_t sDistributionWeightBase = 1000;
static constexpr uint16_t sDistributionWeightBonus = 4000;
static constexpr float sThermalControlDeadbandCelsius = 0.0f;
static constexpr float sThermalWeightFullBiasSpreadCelsius = 20.0f;
static constexpr float sThermalRebalanceWattsPerCelsius = 20.0f;
static constexpr uint16_t sThermalRebalanceMaxStepWatts = 120;
static constexpr float sThermalDerivativeLeadMinutes = 3.0f;
static constexpr float sThermalDerivativeFilterTimeConstantMinutes = 4.0f;
static constexpr float sThermalDerivativeMaxCelsius = 6.0f;

static float thermalEffectiveDelta(float deltaCelsius)
{
    return std::max(0.0f, deltaCelsius - sThermalControlDeadbandCelsius);
}

static uint16_t thermalDistributionWeight(float preferredDeltaCelsius)
{
    float bonusRatio = std::min(1.0f,
            thermalEffectiveDelta(preferredDeltaCelsius) / sThermalWeightFullBiasSpreadCelsius);
    return sDistributionWeightBase
        + static_cast<uint16_t>((bonusRatio * sDistributionWeightBonus) + 0.5f);
}

struct ThermalProjection {
    PowerLimiterInverter* inverter;
    float temperature;
    uint32_t statsMillis;
    float averageTemperature = 0.0f;
    float pError = 0.0f;
    float rawSlopeCelsiusPerMinute = 0.0f;
    float slopeCelsiusPerMinute = 0.0f;
    float dError = 0.0f;
    float pdError = 0.0f;
    float projectedTemperature = 0.0f;
    float preferredDelta = 0.0f;
    float effectiveDelta = 0.0f;
    uint16_t weight = sDistributionWeightBase;
    uint16_t capacity = 0;
    bool derivativeValid = false;
};

struct ThermalDebugItem {
    std::string name;
    std::string serial;
    float temperature;
    float averageTemperature;
    float pError;
    float rawSlopeCelsiusPerMinute;
    float slopeCelsiusPerMinute;
    float dError;
    float pdError;
    float projectedTemperature;
    float preferredDelta;
    float effectiveDelta;
    uint16_t weight;
    uint16_t capacity;
    uint32_t statsMillis;
    bool derivativeValid;
    std::string action;
};

struct ThermalTrendState {
    uint64_t serial;
    uint32_t statsMillis;
    float error;
    float rawSlopeCelsiusPerMinute = 0.0f;
    float slopeCelsiusPerMinute = 0.0f;
    float dError = 0.0f;
    bool derivativeValid = false;
};

static std::vector<ThermalTrendState> sThermalTrendStates;
static std::vector<ThermalDebugItem> sThermalDebugItems;
static String sThermalDebugSource;
static String sThermalDebugFilterExpression;
static uint32_t sThermalDebugMillis = 0;
static uint16_t sThermalDebugExchangeBudgetWatts = 0;

static char const* thermalPreferenceName(ThermalPreference preference)
{
    return preference == ThermalPreference::Cooler ? "cooler" : "hotter";
}

static ThermalProjection projectThermalTrend(
        PowerLimiterInverter* inverter,
        float temperature,
        float averageTemperature,
        uint32_t statsMillis)
{
    ThermalProjection projection {
        inverter,
        temperature,
        statsMillis,
        averageTemperature,
        temperature - averageTemperature,
    };

    auto iter = std::find_if(sThermalTrendStates.begin(), sThermalTrendStates.end(),
            [inverter](auto const& state) {
                return state.serial == inverter->getSerial();
            });

    if (iter == sThermalTrendStates.end()) {
        sThermalTrendStates.push_back({
            inverter->getSerial(),
            statsMillis,
            projection.pError,
        });
    } else if (iter->statsMillis == statsMillis) {
        projection.rawSlopeCelsiusPerMinute = iter->rawSlopeCelsiusPerMinute;
        projection.slopeCelsiusPerMinute = iter->slopeCelsiusPerMinute;
        projection.dError = iter->dError;
        projection.derivativeValid = iter->derivativeValid;
    } else {
        bool const timestampAdvanced = millisAtOrAfter(statsMillis, iter->statsMillis);
        float elapsedMinutes = timestampAdvanced
            ? static_cast<float>(statsMillis - iter->statsMillis) / 60000.0f
            : 0.0f;

        if (elapsedMinutes > 0.0f) {
            projection.rawSlopeCelsiusPerMinute =
                (projection.pError - iter->error) / elapsedMinutes;
            float const alpha = elapsedMinutes
                / (sThermalDerivativeFilterTimeConstantMinutes + elapsedMinutes);
            projection.slopeCelsiusPerMinute = iter->slopeCelsiusPerMinute
                + (alpha * (projection.rawSlopeCelsiusPerMinute - iter->slopeCelsiusPerMinute));
            projection.dError = std::clamp(
                    projection.slopeCelsiusPerMinute * sThermalDerivativeLeadMinutes,
                    -sThermalDerivativeMaxCelsius,
                    sThermalDerivativeMaxCelsius);
            projection.derivativeValid = true;
        }

        iter->statsMillis = statsMillis;
        iter->error = projection.pError;
        iter->rawSlopeCelsiusPerMinute = projection.rawSlopeCelsiusPerMinute;
        iter->slopeCelsiusPerMinute = projection.slopeCelsiusPerMinute;
        iter->dError = projection.dError;
        iter->derivativeValid = projection.derivativeValid;
    }

    projection.pdError = projection.pError + projection.dError;
    projection.projectedTemperature = projection.averageTemperature + projection.pdError;
    return projection;
}

static std::vector<ThermalProjection> collectThermalProjections(
        std::vector<PowerLimiterInverter*> const& inverters,
        float& averageTemperature)
{
    float temperatureSum = 0.0f;
    std::vector<ThermalProjection> projections;

    for (auto const inverter : inverters) {
        auto temperature = inverter->getTemperatureCelsius();
        if (!temperature) { continue; }

        auto statsMillis = inverter->getCurrentStatsMillis();
        if (statsMillis == 0) { continue; }

        temperatureSum += *temperature;
        projections.push_back({
            inverter,
            *temperature,
            statsMillis,
        });
    }

    if (projections.size() < 2) {
        projections.clear();
        return projections;
    }

    averageTemperature = temperatureSum / projections.size();
    for (auto& projection : projections) {
        projection = projectThermalTrend(
                projection.inverter,
                projection.temperature,
                averageTemperature,
                projection.statsMillis);
    }

    return projections;
}

static void publishThermalDebug(
        std::string const& source,
        std::string const& filterExpression,
        std::vector<ThermalProjection> const& projections,
        char const* action,
        uint16_t exchangeBudgetWatts = 0)
{
    sThermalDebugSource = source.c_str();
    sThermalDebugFilterExpression = filterExpression.c_str();
    sThermalDebugMillis = millis();
    sThermalDebugExchangeBudgetWatts = exchangeBudgetWatts;
    sThermalDebugItems.clear();

    for (auto const& projection : projections) {
        sThermalDebugItems.push_back({
            projection.inverter->getSerialStr(),
            projection.inverter->getSerialStr(),
            projection.temperature,
            projection.averageTemperature,
            projection.pError,
            projection.rawSlopeCelsiusPerMinute,
            projection.slopeCelsiusPerMinute,
            projection.dError,
            projection.pdError,
            projection.projectedTemperature,
            projection.preferredDelta,
            projection.effectiveDelta,
            projection.weight,
            projection.capacity,
            projection.statsMillis,
            projection.derivativeValid,
            action,
        });
    }
}

static void applyThermalWeights(std::vector<DistributedChange>& changes,
        std::vector<PowerLimiterInverter*> const& matchingInverters,
        ThermalPreference preference,
        std::string const& filterExpression)
{
    float averageTemperature = 0.0f;
    auto projections = collectThermalProjections(matchingInverters, averageTemperature);
    if (projections.empty()) { return; }

    float minProjectedTemperature = std::numeric_limits<float>::max();
    float maxProjectedTemperature = std::numeric_limits<float>::lowest();
    for (auto const& projection : projections) {
        minProjectedTemperature = std::min(minProjectedTemperature, projection.projectedTemperature);
        maxProjectedTemperature = std::max(maxProjectedTemperature, projection.projectedTemperature);
    }

    if (maxProjectedTemperature <= minProjectedTemperature) { return; }

    for (auto& projection : projections) {
        projection.preferredDelta = preference == ThermalPreference::Cooler
            ? maxProjectedTemperature - projection.projectedTemperature
            : projection.projectedTemperature - minProjectedTemperature;
        projection.effectiveDelta = thermalEffectiveDelta(projection.preferredDelta);
        projection.weight = thermalDistributionWeight(projection.preferredDelta);

        auto change = std::find_if(changes.begin(), changes.end(),
                [&projection](auto const& item) {
                    return item.inverter == projection.inverter;
                });
        if (change != changes.end()) {
            change->weight = projection.weight;
        }
    }

    publishThermalDebug("distribution-weight", filterExpression, projections,
            thermalPreferenceName(preference));
}

void PowerLimiterClass::addThermalDebugJson(JsonObject root) const
{
    root["source"] = sThermalDebugSource.c_str();
    root["filter"] = sThermalDebugFilterExpression.c_str();
    root["age_ms"] = millis() - sThermalDebugMillis;
    root["deadband_c"] = sThermalControlDeadbandCelsius;
    root["derivative_lead_min"] = sThermalDerivativeLeadMinutes;
    root["derivative_filter_tau_min"] = sThermalDerivativeFilterTimeConstantMinutes;
    root["derivative_max_c"] = sThermalDerivativeMaxCelsius;
    root["watts_per_c"] = sThermalRebalanceWattsPerCelsius;
    root["max_step_w"] = sThermalRebalanceMaxStepWatts;
    root["exchange_budget_w"] = sThermalDebugExchangeBudgetWatts;
    root["weight_base"] = sDistributionWeightBase;
    root["weight_bonus"] = sDistributionWeightBonus;

    auto items = root["inverters"].to<JsonArray>();
    for (auto const& item : sThermalDebugItems) {
        auto inverter = items.add<JsonObject>();
        inverter["name"] = item.name.c_str();
        inverter["serial"] = item.serial.c_str();
        inverter["action"] = item.action.c_str();
        inverter["temperature_c"] = item.temperature;
        inverter["average_temperature_c"] = item.averageTemperature;
        inverter["p_error_c"] = item.pError;
        inverter["raw_slope_c_per_min"] = item.rawSlopeCelsiusPerMinute;
        inverter["slope_c_per_min"] = item.slopeCelsiusPerMinute;
        inverter["d_error_c"] = item.dError;
        inverter["pd_error_c"] = item.pdError;
        inverter["projected_temperature_c"] = item.projectedTemperature;
        inverter["preferred_delta_c"] = item.preferredDelta;
        inverter["effective_delta_c"] = item.effectiveDelta;
        inverter["weight"] = item.weight;
        inverter["capacity_w"] = item.capacity;
        inverter["stats_millis"] = item.statsMillis;
        inverter["derivative_valid"] = item.derivativeValid;
    }
}

static uint16_t thermalRebalanceCapacity(float deltaCelsius, uint16_t maxChangeWatts)
{
    if (deltaCelsius <= 0.0f || maxChangeWatts == 0) { return 0; }

    float effectiveDelta = thermalEffectiveDelta(deltaCelsius);
    if (effectiveDelta <= 0.0f) { return 0; }

    auto requested = static_cast<uint16_t>(std::round(effectiveDelta * sThermalRebalanceWattsPerCelsius));
    requested = std::min<uint16_t>(requested, sThermalRebalanceMaxStepWatts);
    return std::min<uint16_t>(requested, maxChangeWatts);
}

static uint16_t getMeasuredThermalExchangeBudgetWatts(uint16_t hysteresis, float targetConsumption)
{
    if (!PowerMeter.isDataValid()) { return 0; }

    auto const meterValue = PowerMeter.getPowerTotal();
    auto const target = targetConsumption;
    auto const margin = static_cast<float>(std::max<uint16_t>(1, hysteresis));
    auto const surplus = target - meterValue - margin;

    if (surplus <= 0.0f) { return 0; }

    return static_cast<uint16_t>(
            std::round(std::min<float>(surplus, std::numeric_limits<uint16_t>::max())));
}

static uint16_t distributeByWeight(uint16_t requested, std::vector<DistributedChange>& changes)
{
    uint32_t totalCapacity = 0;
    for (auto const& change : changes) {
        totalCapacity += change.capacity;
    }

    uint32_t remaining = std::min<uint32_t>(requested, totalCapacity);
    uint32_t allocated = remaining;

    while (remaining > 0) {
        uint32_t totalWeight = 0;
        for (auto const& change : changes) {
            if (change.amount < change.capacity) {
                totalWeight += std::max<uint16_t>(1, change.weight);
            }
        }

        if (totalWeight == 0) { break; }

        uint32_t passRemaining = remaining;
        bool madeProgress = false;

        for (auto& change : changes) {
            if (remaining == 0) { break; }
            if (change.amount >= change.capacity) { continue; }

            uint32_t room = change.capacity - change.amount;
            uint32_t share = std::max<uint32_t>(
                    1,
                    (passRemaining * std::max<uint16_t>(1, change.weight)) / totalWeight);
            uint16_t delta = static_cast<uint16_t>(std::min<uint32_t>(room, share));
            if (delta == 0) { continue; }

            change.amount += delta;
            remaining -= delta;
            madeProgress = true;
        }

        if (!madeProgress) { break; }
    }

    return allocated - remaining;
}

static bool rebalanceThermally(std::vector<PowerLimiterInverter*>& matchingInverters,
        std::string const& filterExpression,
        uint16_t exchangeBudgetWatts = 0)
{
    float averageTemperature = 0.0f;
    auto projections = collectThermalProjections(matchingInverters, averageTemperature);
    if (projections.empty()) { return false; }

    std::vector<DistributedChange> reductions;
    std::vector<DistributedChange> increases;
    uint32_t totalReductionCapacity = 0;
    uint32_t totalIncreaseCapacity = 0;

    for (auto& projection : projections) {
        float const effectiveError = std::abs(projection.pdError);
        projection.preferredDelta = effectiveError;
        projection.effectiveDelta = thermalEffectiveDelta(effectiveError);
        projection.weight = thermalDistributionWeight(effectiveError);

        if (projection.pdError > 0.0f) {
            uint16_t maxReduction = projection.inverter->getMaxReductionWatts(false/*no standby*/);
            uint16_t capacity = thermalRebalanceCapacity(effectiveError, maxReduction);
            projection.capacity = capacity;
            if (capacity == 0) { continue; }

            reductions.push_back({
                projection.inverter,
                capacity,
                0,
                projection.weight,
            });
            totalReductionCapacity += capacity;
        } else if (projection.pdError < 0.0f) {
            uint16_t maxIncrease = projection.inverter->getMaxIncreaseWatts();
            uint16_t capacity = thermalRebalanceCapacity(effectiveError, maxIncrease);
            projection.capacity = capacity;
            if (capacity == 0) { continue; }

            increases.push_back({
                projection.inverter,
                capacity,
                0,
                projection.weight,
            });
            totalIncreaseCapacity += capacity;
        }
    }

    bool const allowExchange = exchangeBudgetWatts > 0;
    publishThermalDebug(allowExchange ? "rebalance" : "rebalance-blocked",
            filterExpression, projections, allowExchange ? "exchange" : "increase-needed",
            exchangeBudgetWatts);

    if (!allowExchange) { return false; }

    uint16_t exchange = static_cast<uint16_t>(
            std::min<uint32_t>({
                totalReductionCapacity,
                totalIncreaseCapacity,
                exchangeBudgetWatts,
            }));
    if (exchange == 0) { return false; }

    distributeByWeight(exchange, reductions);

    uint16_t appliedReduction = 0;
    size_t reducingInverters = 0;
    for (auto& reduction : reductions) {
        if (reduction.amount == 0) { continue; }

        appliedReduction += reduction.inverter->applyReduction(
                reduction.amount, false/*no standby*/);
        ++reducingInverters;
    }

    if (appliedReduction == 0) { return false; }

    distributeByWeight(appliedReduction, increases);

    uint16_t appliedIncrease = 0;
    size_t increasingInverters = 0;
    for (auto& increase : increases) {
        if (increase.amount == 0) { continue; }

        appliedIncrease += increase.inverter->applyIncrease(increase.amount);
        ++increasingInverters;
    }

    if (appliedIncrease == 0) { return true; }

    DTU_LOGD("thermal rebalance for %s inverters: shifting %u W from "
            "%u warmer inverter%s to %u cooler inverter%s around %.1f C",
            filterExpression.c_str(), appliedIncrease,
            static_cast<unsigned>(reducingInverters), (reducingInverters == 1?"":"s"),
            static_cast<unsigned>(increasingInverters), (increasingInverters == 1?"":"s"),
            averageTemperature);

    return true;
}

PowerLimiterClass PowerLimiter;

void PowerLimiterClass::init(Scheduler& scheduler)
{
    _batteryTargetPowerConsumption = Configuration.get().PowerLimiter.BatteryTargetPowerConsumption;

    scheduler.addTask(_loopTask);
    _loopTask.setCallback(std::bind(&PowerLimiterClass::loop, this));
    _loopTask.setIterations(TASK_FOREVER);
    _loopTask.enable();
}

frozen::string const& PowerLimiterClass::getStatusText(PowerLimiterClass::Status status) const
{
    static const frozen::string missing = "programmer error: missing status text";

    static const frozen::map<Status, frozen::string, 11> texts = {
        { Status::Initializing, "initializing (should not see me)" },
        { Status::DisabledByConfig, "disabled by configuration" },
        { Status::DisabledByMqtt, "disabled by MQTT" },
        { Status::WaitingForValidTimestamp, "waiting for valid date and time to be available" },
        { Status::PowerMeterPending, "waiting for sufficiently recent power meter reading" },
        { Status::InverterInvalid, "invalid inverter selection/configuration" },
        { Status::InverterCmdPending, "waiting for a start/stop/restart/limit command to complete" },
        { Status::ConfigReload, "reloading DPL configuration" },
        { Status::InverterStatsPending, "waiting for sufficiently recent inverter data" },
        { Status::UnconditionalSolarPassthrough, "unconditionally passing through all solar power (MQTT override)" },
        { Status::Stable, "the system is stable, the last power limit is still valid" },
    };

    auto iter = texts.find(status);
    if (iter == texts.end()) { return missing; }

    return iter->second;
}

void PowerLimiterClass::announceStatus(PowerLimiterClass::Status status)
{
    // this method is called with high frequency. print the status text if
    // the status changed since we last printed the text of another one.
    // otherwise repeat the info with a fixed interval.
    if (_lastStatus == status && millis() < _lastStatusPrinted + 10 * 1000) { return; }

    // after announcing once that the DPL is disabled by configuration, it
    // should just be silent while it is disabled.
    if (status == Status::DisabledByConfig && _lastStatus == status) { return; }

    DTU_LOGI("%s", getStatusText(status).data());

    _lastStatus = status;
    _lastStatusPrinted = millis();
}

void PowerLimiterClass::reloadConfig()
{
    auto const& config = Configuration.get();

    if (!config.PowerLimiter.Enabled || Mode::Disabled == _mode) {
        _retirees.insert(
            _retirees.end(),
            std::make_move_iterator(_inverters.begin()),
            std::make_move_iterator(_inverters.end())
        );

        _inverters.clear();

        _reloadConfigFlag = false;
        return;
    }

    auto iter = _inverters.begin();
    while (iter != _inverters.end()) {
        bool stillGoverned = false;

        for (size_t i = 0; i < INV_MAX_COUNT; ++i) {
            auto const& inv = config.PowerLimiter.Inverters[i];
            if (inv.Serial == 0ULL) { break; }
            stillGoverned = inv.Serial == (*iter)->getSerial() && inv.IsGoverned;
            if (stillGoverned) { break; }
        }

        if (!stillGoverned) {
            _retirees.push_back(std::move(*iter));
        }

        iter = _inverters.erase(iter);
    }

    for (size_t i = 0; i < INV_MAX_COUNT; ++i) {
        auto const& invConfig = config.PowerLimiter.Inverters[i];

        if (invConfig.Serial == 0ULL) { break; }

        if (!invConfig.IsGoverned) { continue; }

        auto upInv = PowerLimiterInverter::create(invConfig);
        if (upInv) { _inverters.push_back(std::move(upInv)); }
    }

    calcNextInverterRestart();
    resetDynamicBatteryTargetState();
    _batteryTargetPowerConsumption = config.PowerLimiter.BatteryTargetPowerConsumption;

    _reloadConfigFlag = false;
}

void PowerLimiterClass::loop()
{
    auto const& config = Configuration.get();

    // we know that the Hoymiles library refuses to send any message to any
    // inverter until the system has valid time information. until then we can
    // do nothing, not even shutdown the inverter.
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 5)) {
        return announceStatus(Status::WaitingForValidTimestamp);
    }

    // take care that the last requested power
    // limits and power states are actually reached
    if (updateInverters()) {
        return announceStatus(Status::InverterCmdPending);
    }

    if (_reloadConfigFlag) {
        reloadConfig();
        return announceStatus(Status::ConfigReload);
    }

    if (!config.PowerLimiter.Enabled) {
        return announceStatus(Status::DisabledByConfig);
    }

    if (Mode::Disabled == _mode) {
        return announceStatus(Status::DisabledByMqtt);
    }

    if (_inverters.empty()) {
        return announceStatus(Status::InverterInvalid);
    }

    uint32_t latestOutputReferenceMillis = 0;
    bool hasOutputReference = false;

    for (auto const& upInv : _inverters) {
        // in particular, we don't want to wait for stats from inverters that
        // are not eligible because they are (currently) unreachable. this is
        // fine as we ignore them throughout the DPL loop if they are not eligible.
        if (!upInv->isEligible()) { continue; }

        auto oOutputReferenceMillis = upInv->getOutputReferenceMillis();
        if (!oOutputReferenceMillis) {
            return announceStatus(Status::InverterStatsPending);
        }

        latestOutputReferenceMillis = hasOutputReference
            ? latestMillis(*oOutputReferenceMillis, latestOutputReferenceMillis)
            : *oOutputReferenceMillis;
        hasOutputReference = true;
    }

    if (isGridChargerManaged() && GridCharger.supportsPowerLimiterControl()) {
        auto oOutputReferenceMillis = GridCharger.getPowerLimiterOutputReferenceMillis();
        if (!oOutputReferenceMillis) {
            return announceStatus(Status::InverterStatsPending);
        }

        latestOutputReferenceMillis = hasOutputReference
            ? latestMillis(*oOutputReferenceMillis, latestOutputReferenceMillis)
            : *oOutputReferenceMillis;
        hasOutputReference = true;
    }

    // note that we can only perform unconditional full solar-passthrough or any
    // calculation at all after surviving the loop above, which ensures that each
    // eligible inverter has either usable stats or a successful command's
    // output settle timestamp.
    if (Mode::UnconditionalFullSolarPassthrough == _mode) {
        return unconditionalFullSolarPassthrough();
    }

    // if the power meter is being used, i.e., if its data is valid, we want to
    // wait for a new reading after adjusting the inverter limit. For a recent
    // successful command, the reference time is the fixed settle delay
    // instead of a post-command inverter statistics timestamp.
    if (PowerMeter.isDataValid()
            && hasOutputReference
            && !millisAtOrAfter(PowerMeter.getLastUpdate(), latestOutputReferenceMillis)) {
        return announceStatus(Status::PowerMeterPending);
    }

    updateDynamicBatteryTarget();

    // since _lastCalculation and _calculationBackoffMs are initialized to
    // zero, this test is passed the first time the condition is checked.
    if ((millis() - _lastCalculation) < _calculationBackoffMs) {
        return announceStatus(Status::Stable);
    }

    auto autoRestartInverters = [this]() -> bool {
        if (!_nextInverterRestart.first) { return false; } // no automatic restarts

        auto constexpr halfOfAllMillis = std::numeric_limits<uint32_t>::max() / 2;
        auto diff = _nextInverterRestart.second - millis();
        if (diff < halfOfAllMillis) { return false; }

        bool restartSent = false;
        for (auto& upInv : _inverters) {
            if (!upInv->isSolarPowered()) {
                DTU_LOGI("sending restart command to inverter %s", upInv->getSerialStr());
                restartSent |= upInv->restart();
            }
        }

        calcNextInverterRestart();
        return restartSent;
    };

    if (autoRestartInverters()) {
        return announceStatus(Status::InverterCmdPending);
    }

    auto getBatteryState = [this,&config]() -> BatteryState {

        // State machine for the battery
        // Conditions:          we use 'Below Stop Threshold', 'Above Start Threshold', 'Solar-Passthrough', 'Use Battery at night',
        //                      'Night/Day' and 'From which direction did we enter the stop-start zone' to determine the state.
        //
        // states               description
        // --------------------------------------------------------------------------------------------------------------------------------
        // STOP:                we must stop the inverter, because the battery is below the stop threshold
        // NO_DISCHARGE:        we can use the inverter, but we do not allow to discharge the battery, A requirement from 'Solar-Passthrough'
        // DISCHARGE_ALLOWED:   we can use the inverter and we allow discharging of the battery
        // DISCHARGE_NIGHT:     we can use the inverter and we allow discharging of a partial charged battery at night.
        //                      A requirement from 'Use Battery at night'
        //
        // Notes: The combination of 'Use Battery at night' and use of 'voltage thresholds' can leads to oscillation between the states
        // STOP and DISCHARGE_NIGHT. To avoid this problem, we allow only one transmission from STOP to DISCHARGE_NIGHT per night.
        // In case of restart or power-cycle, we accept that the inverter may start discharging at night once again.
        // Start-Up can be tricky, because data from the battery provider may not be available. As fallback we use the not very
        // accurate inverter voltage and this can lead to the wrong state.

        // check if we have a battery powered inverter
        if (!usesBatteryPoweredInverter()) { return BatteryState::STOP; }

        // check the stop condition
        auto day = SunPosition.isDayPeriod();
        if (isStopThresholdReached()) {
            _fromStart = false;
            _oneStopPerNightDone = day ? false : true;
            return BatteryState::STOP;
        }

        // check the start condition
        if (isStartThresholdReached()) {
            _fromStart = true;
            return BatteryState::DISCHARGE_ALLOWED;
        }

        // all of the following conditions mean that we are in the "stop-start zone",
        // and we must use the buffered information 'From which direction did we enter the stop-start zone'.

        // if we come from start we always allow discharging of the battery
        if (_fromStart) { return BatteryState::DISCHARGE_ALLOWED; }

        // if we reach this line we come from stop and have to consider the 'Solar-Passthrough' and the 'Use Battery at night' settings.
        auto solarPassThroughEnabled = isSolarPassThroughEnabled();
        auto isBatteryAlwaysUseAtNightEnabled = config.PowerLimiter.BatteryAlwaysUseAtNight;

        // When `Use Battery at night` is disabled or when its day, battery should not be discharged
        if (!isBatteryAlwaysUseAtNightEnabled || day) {
            _oneStopPerNightDone = false;

            // Only allow inverters to be active if we are in solar pass-through mode.
            // Otherwise we stop the battery inverters.
            if (solarPassThroughEnabled) { return BatteryState::NO_DISCHARGE; }
            return BatteryState::STOP;
         }

        // When `Use Battery at night` is enabled, and its night and we have already stopped the battery once per night, we keep the STOP state.
        // Otherwise we allow discharging of a partially charged battery.
        if (_oneStopPerNightDone) { return BatteryState::STOP; }
        return BatteryState::DISCHARGE_NIGHT;
    };

    auto getFullSolarPassthrough = [this,&config]() -> bool {
        // we only do full solar PT if general solar PT is enabled
        // and we are above the 'battery start threshold'
        if (!isSolarPassThroughEnabled() || !isStartThresholdReached()) { return false; }

        if (testThreshold(config.PowerLimiter.FullSolarPassThroughSoc,
                        config.PowerLimiter.FullSolarPassThroughStartVoltage,
                        [](float a, float b) -> bool { return a >= b; })) {
            return true;
        }

        if (testThreshold(config.PowerLimiter.FullSolarPassThroughSoc,
                        config.PowerLimiter.FullSolarPassThroughStopVoltage,
                        [](float a, float b) -> bool { return a < b; })) {
            return false;
        }

        return _fullSolarPassThroughActive;
    };

    auto getLoadCorrectedVoltage = [this,&config]() -> float {
        // TODO(schlimmchen): use the battery's data if available,
        // i.e., the current drawn from the battery as reported by the battery.
        float acPower = getBatteryInvertersOutputAcWatts();
        float dcVoltage = getBatteryVoltage();

        if (dcVoltage <= 0.0) { return 0.0; }

        return dcVoltage + (acPower * config.PowerLimiter.VoltageLoadCorrectionFactor);
    };

    _loadCorrectedVoltage = getLoadCorrectedVoltage();
    _batteryState = getBatteryState();
    _fullSolarPassThroughActive = getFullSolarPassthrough();

    DTU_LOGD("up %lu s, it is %s, next inverter restart at %d s (set to %d)",
            millis()/1000,
            (SunPosition.isDayPeriod()?"day":"night"),
            _nextInverterRestart.second/1000,
            config.PowerLimiter.RestartHour);

    if (usesBatteryPoweredInverter()) {
        DTU_LOGD("battery interface %sabled, SoC %.1f %% (%s), age %u s (%s)",
                (config.Battery.Enabled?"en":"dis"),
                Battery.getStats()->getSoC(),
                (config.PowerLimiter.IgnoreSoc?"ignored":"used"),
                Battery.getStats()->getSoCAgeSeconds(),
                (Battery.getStats()->isSoCValid()?"valid":"stale"));

        auto dcVoltage = getBatteryVoltage(true/*log voltages only once per DPL loop*/);
        DTU_LOGD("battery voltage %.2f V, load-corrected voltage %.2f V @ %.0f W, factor %.5f 1/A",
                dcVoltage, _loadCorrectedVoltage,
                getBatteryInvertersOutputAcWatts(),
                config.PowerLimiter.VoltageLoadCorrectionFactor);

        DTU_LOGD("battery discharge %s, start %.2f V or %u %%, stop %.2f V or %u %%",
                (((_batteryState == BatteryState::DISCHARGE_ALLOWED) || (_batteryState == BatteryState::DISCHARGE_NIGHT))?"allowed":
                (_batteryState == BatteryState::NO_DISCHARGE)?"restricted":"stopped"),
                config.PowerLimiter.VoltageStartThreshold,
                config.PowerLimiter.BatterySocStartThreshold,
                config.PowerLimiter.VoltageStopThreshold,
                config.PowerLimiter.BatterySocStopThreshold);

        if (isSolarPassThroughEnabled()) {
            DTU_LOGD("full solar-passthrough %s, start %.2f V or %u %%, stop %.2f V",
                    (isFullSolarPassthroughActive()?"active":"dormant"),
                    config.PowerLimiter.FullSolarPassThroughStartVoltage,
                    config.PowerLimiter.FullSolarPassThroughSoc,
                    config.PowerLimiter.FullSolarPassThroughStopVoltage);
        }

        DTU_LOGD("start %sreached, stop %sreached, solar-passthrough %sabled, use at night %sabled and %s",
                (isStartThresholdReached()?"":"NOT "),
                (isStopThresholdReached()?"":"NOT "),
                (isSolarPassThroughEnabled()?"en":"dis"),
                (config.PowerLimiter.BatteryAlwaysUseAtNight?"en":"dis"),
                ((_batteryState == BatteryState::DISCHARGE_NIGHT)?"active":"dormant"));

        DTU_LOGD("total max AC power is %u W, conduction losses are %u %%",
            config.PowerLimiter.TotalUpperPowerLimit,
            config.PowerLimiter.ConductionLosses);
    }

    bool gridChargerLimitUpdated = false;
    bool const gridChargerManaged = isGridChargerManaged()
        && GridCharger.supportsPowerLimiterControl();
    auto storageTargetPowerConsumption = getStorageTargetPowerConsumption();
    int32_t gridChargerMeterAdjustment = 0;
    auto powerMeterValue = []() -> float {
        return PowerMeter.getPowerTotal();
    };

    if (gridChargerManaged && PowerMeter.isDataValid()) {
        auto const meterValue = powerMeterValue();
        auto const gridChargerInput = GridCharger.getPowerLimiterExpectedInputPowerWatts();
        if (meterValue > storageTargetPowerConsumption && gridChargerInput > 0) {
            auto const requestedReduction = ceilPositiveWattsToUint16(
                    std::min<float>(
                        gridChargerInput,
                        meterValue - storageTargetPowerConsumption));
            uint16_t const appliedReduction =
                GridCharger.applyPowerLimiterInputPowerReduction(requestedReduction);
            if (appliedReduction > 0) {
                gridChargerMeterAdjustment -= appliedReduction;
                gridChargerLimitUpdated = true;
                DTU_LOGD("reducing grid charger input by %u W before requesting battery output "
                        "(meter %.1f W, storage target %.1f W)",
                        appliedReduction, meterValue,
                        storageTargetPowerConsumption);
            }
        }
    }

    auto const targetPowerConsumption = getTargetPowerConsumption();
    uint16_t inverterTotalPower = calcTargetOutput(
            targetPowerConsumption,
            gridChargerMeterAdjustment);

    auto totalAllowance = config.PowerLimiter.TotalUpperPowerLimit;
    inverterTotalPower = std::min(inverterTotalPower, totalAllowance);

    auto coveredBySolar = updateInverterLimits(inverterTotalPower, sSolarPoweredFilter, sSolarPoweredExpression);
    auto remainingAfterSolar = (inverterTotalPower >= coveredBySolar) ? inverterTotalPower - coveredBySolar : 0;
    auto coveredBySmartBuffer = updateInverterLimits(remainingAfterSolar, sSmartBufferPoweredFilter, sSmartBufferPoweredExpression);
    auto remainingAfterSmartBuffer = (remainingAfterSolar >= coveredBySmartBuffer) ? remainingAfterSolar - coveredBySmartBuffer : 0;
    auto batteryPowerRequest = remainingAfterSmartBuffer;
    std::optional<uint16_t> oBatteryRequestLimit = std::nullopt;
    bool allowBatteryStandby = true;
    uint16_t batteryMinOutputWithoutStandby = 0;
    if (usesBatteryPoweredInverter()) {
        auto const batteryControlActive = (_batteryState == BatteryState::DISCHARGE_ALLOWED
                || _batteryState == BatteryState::DISCHARGE_NIGHT)
            && (batteryPowerRequest > 0 || getBatteryInvertersOutputAcWatts() > 0.0f);

        auto batteryTargetPowerConsumption = storageTargetPowerConsumption;
        auto batteryTargetTotalPower = calcTargetOutput(
                batteryTargetPowerConsumption,
                gridChargerMeterAdjustment);
        batteryTargetTotalPower = std::min(batteryTargetTotalPower, totalAllowance);

        uint16_t coveredWithoutBattery = coveredBySolar + coveredBySmartBuffer;
        uint16_t batteryRequestLimit = (batteryTargetTotalPower > coveredWithoutBattery)
            ? batteryTargetTotalPower - coveredWithoutBattery
            : 0;

        if (batteryPowerRequest > batteryRequestLimit) {
            DTU_LOGD("limiting battery-powered inverter request from %u W to %u W "
                    "to keep battery grid target at %.1f W",
                    batteryPowerRequest, batteryRequestLimit, batteryTargetPowerConsumption);
            batteryPowerRequest = batteryRequestLimit;
        }

        oBatteryRequestLimit = batteryRequestLimit;

        batteryMinOutputWithoutStandby = getBatteryInvertersMinOutputAcWatts();
        if (batteryControlActive
                && batteryMinOutputWithoutStandby > 0
                && batteryPowerRequest < batteryMinOutputWithoutStandby) {
            uint32_t const requestLimitWithStandbyMargin =
                static_cast<uint32_t>(batteryRequestLimit)
                + config.PowerLimiter.BatteryStandbyPowerMargin;

            if (requestLimitWithStandbyMargin >= batteryMinOutputWithoutStandby) {
                DTU_LOGD("keeping battery-powered inverters at %u W instead of "
                        "allowing standby (request %u W, battery target limit "
                        "%u W, standby margin %u W)",
                        batteryMinOutputWithoutStandby, batteryPowerRequest,
                        batteryRequestLimit,
                        config.PowerLimiter.BatteryStandbyPowerMargin);
                batteryPowerRequest = batteryMinOutputWithoutStandby;
                allowBatteryStandby = false;
                oBatteryRequestLimit = static_cast<uint16_t>(std::min<uint32_t>(
                        requestLimitWithStandbyMargin,
                        std::numeric_limits<uint16_t>::max()));
            }
        }
    }

    auto powerBusUsage = calcPowerBusUsage(batteryPowerRequest);
    if (oBatteryRequestLimit && powerBusUsage > *oBatteryRequestLimit) {
        auto batteryTargetPowerConsumption = getBatteryTargetPowerConsumption();
        DTU_LOGD("limiting DC power bus usage from %u W to %u W "
                "to keep battery grid target at %.1f W",
                powerBusUsage, *oBatteryRequestLimit, batteryTargetPowerConsumption);
        powerBusUsage = *oBatteryRequestLimit;
    }
    if (!allowBatteryStandby && powerBusUsage < batteryMinOutputWithoutStandby) {
        DTU_LOGD("allowing battery-powered inverter standby because DC power bus "
                "allowance %u W is below the minimum online output %u W",
                powerBusUsage, batteryMinOutputWithoutStandby);
        allowBatteryStandby = true;
    }

    auto coveredByBattery = updateInverterLimits(powerBusUsage,
            sBatteryPoweredFilter, sBatteryPoweredExpression,
            allowBatteryStandby);

    if (gridChargerManaged && PowerMeter.isDataValid()) {
        auto const currentInverterOutput = getCurrentInvertersOutputAcWatts();
        auto const expectedInverterOutput =
            coveredBySolar + coveredBySmartBuffer + coveredByBattery;
        auto expectedMeterValue = powerMeterValue()
            + gridChargerMeterAdjustment
            - (static_cast<float>(expectedInverterOutput)
                    - static_cast<float>(currentInverterOutput));

        auto updateExpectedMeterAfterChargerChange = [&](int32_t delta) {
            gridChargerMeterAdjustment += delta;
            expectedMeterValue += delta;
        };

        if (coveredByBattery > 0) {
            auto const gridChargerInput = GridCharger.getPowerLimiterExpectedInputPowerWatts();
            uint16_t const appliedReduction =
                GridCharger.applyPowerLimiterInputPowerReduction(gridChargerInput);
            if (appliedReduction > 0) {
                updateExpectedMeterAfterChargerChange(-static_cast<int32_t>(appliedReduction));
                gridChargerLimitUpdated = true;
                DTU_LOGD("turning grid charger down by %u W because battery-powered "
                        "inverters are requested",
                        appliedReduction);
            }
        } else if (expectedMeterValue > storageTargetPowerConsumption) {
            auto const gridChargerInput = GridCharger.getPowerLimiterExpectedInputPowerWatts();
            if (gridChargerInput > 0) {
                auto const requestedReduction = ceilPositiveWattsToUint16(
                        std::min<float>(
                            gridChargerInput,
                            expectedMeterValue - storageTargetPowerConsumption));
                uint16_t const appliedReduction =
                    GridCharger.applyPowerLimiterInputPowerReduction(requestedReduction);
                if (appliedReduction > 0) {
                    updateExpectedMeterAfterChargerChange(-static_cast<int32_t>(appliedReduction));
                    gridChargerLimitUpdated = true;
                    DTU_LOGD("reducing grid charger input by %u W to keep storage grid target at %.1f W",
                            appliedReduction, storageTargetPowerConsumption);
                }
            }
        } else if (expectedMeterValue < storageTargetPowerConsumption) {
            auto const requestedIncrease = ceilPositiveWattsToUint16(
                    storageTargetPowerConsumption - expectedMeterValue);
            uint16_t const appliedIncrease =
                GridCharger.applyPowerLimiterInputPowerIncrease(requestedIncrease);
            if (appliedIncrease > 0) {
                updateExpectedMeterAfterChargerChange(appliedIncrease);
                gridChargerLimitUpdated = true;
                DTU_LOGD("increasing grid charger input by %u W after battery output reached zero "
                        "(expected meter %.1f W, storage target %.1f W)",
                        appliedIncrease, expectedMeterValue,
                        storageTargetPowerConsumption);
            }
        }

        auto const gridChargerMaxInput = GridCharger.getPowerLimiterMaxInputPowerWatts();
        auto const gridChargerExpectedInput = GridCharger.getPowerLimiterExpectedInputPowerWatts();
        GridCharger.setPowerLimiterLimitedByAvailablePower(
                gridChargerMaxInput > gridChargerExpectedInput);
    } else if (gridChargerManaged) {
        auto const gridChargerInput = GridCharger.getPowerLimiterExpectedInputPowerWatts();
        auto const appliedReduction =
            GridCharger.applyPowerLimiterInputPowerReduction(gridChargerInput);
        if (appliedReduction > 0) {
            gridChargerLimitUpdated = true;
            DTU_LOGD("turning grid charger down by %u W because power meter data is invalid",
                    appliedReduction);
        }
        GridCharger.setPowerLimiterLimitedByAvailablePower(false);
    }

    for (auto const &upInv : _inverters) { upInv->debug(); }

    _lastExpectedInverterOutput = coveredBySolar + coveredBySmartBuffer + coveredByBattery;

    bool limitUpdated = updateInverters() || gridChargerLimitUpdated;

    _lastCalculation = millis();

    if (!limitUpdated) {
        // increase polling backoff if system seems to be stable
        _calculationBackoffMs = std::min<uint32_t>(1024, _calculationBackoffMs * 2);
        return announceStatus(Status::Stable);
    }

    _calculationBackoffMs = _calculationBackoffMsDefault;
}

std::pair<float, char const*> PowerLimiterClass::getInverterDcVoltage() const
{
    auto const& config = Configuration.get();

    auto readDcVoltage = [&config](PowerLimiterInverter* inverter) -> float {
        if (!inverter->isReachable()) { return -1.0; }
        return inverter->getDcVoltage(config.PowerLimiter.InverterChannelIdForDcVoltage);
    };

    char const* configuredInverter = "<unknown>";
    for (auto const& upInv : _inverters) {
        if (upInv->getSerial() != config.PowerLimiter.InverterSerialForDcVoltage) { continue; }

        configuredInverter = upInv->getSerialStr();
        auto voltage = readDcVoltage(upInv.get());
        if (voltage > 0) { return { voltage, configuredInverter }; }
        break;
    }

    for (auto const& upInv : _inverters) {
        if (!upInv->isBatteryPowered()) { continue; }

        auto voltage = readDcVoltage(upInv.get());
        if (voltage > 0) { return { voltage, upInv->getSerialStr() }; }
    }

    return { -1.0, configuredInverter };
}

/**
 * determines the battery's voltage, trying multiple data providers. the most
 * accurate data is expected to be delivered by a BMS, if it's available. more
 * accurate and more recent than the inverter's voltage reading is the volage
 * at the charge controller's output, if it's available. only as a fallback
 * the voltage reported by the inverter is used.
 */
float PowerLimiterClass::getBatteryVoltage(bool log) const {
    auto const& config = Configuration.get();

    float res = 0;

    auto inverter = getInverterDcVoltage();
    if (inverter.first > 0) { res = inverter.first; }

    float chargeControllerVoltage = -1;

    auto chargerOutputVoltage = SolarCharger.getStats()->getOutputVoltage();
    if (chargerOutputVoltage) {
        res = chargeControllerVoltage = *chargerOutputVoltage;
    }

    float bmsVoltage = -1;
    auto stats = Battery.getStats();
    if (config.Battery.Enabled
            && stats->isVoltageValid()
            && stats->getVoltageAgeSeconds() < 60) {
        res = bmsVoltage = stats->getVoltage();
    }

    if (log) {
        DTU_LOGD("BMS: %.2f V, MPPT: %.2f V, inverter %s: %.2f",
                bmsVoltage, chargeControllerVoltage, inverter.second, inverter.first);
    }

    return res;
}

/**
 * calculate the AC output power (limit) to set, such that the inverter uses
 * the given power on its DC side, i.e., adjust the power for the inverter's
 * efficiency.
 */
uint16_t PowerLimiterClass::dcPowerBusToInverterAc(uint16_t dcPower) const
{
    // account for losses between power bus and inverter (cables, junctions...)
    auto const& config = Configuration.get();
    float lossesFactor = 1.00 - static_cast<float>(config.PowerLimiter.ConductionLosses)/100;

    // we cannot know the efficiency at the new limit. even if we could we
    // cannot know which inverter is assigned which limit. hence we use a
    // reasonable, conservative, fixed inverter efficiency.
    return 0.95 * lossesFactor * dcPower;
}

/**
 * implements the "uncoditional full solar passthrough" mode of operation. in this mode of
 * operation, the inverters shall behave as if they were connected to the solar
 * panels directly, i.e., all solar power (and only solar power) is converted
 * to AC power, independent from the power meter reading.
 */
void PowerLimiterClass::unconditionalFullSolarPassthrough()
{
    auto now = millis();
    if ((now - _lastCalculation) < _calculationBackoffMs) { return; }
    _lastCalculation = now;

    for (auto const& upInv : _inverters) {
        if (!upInv->isEligible()) { continue; }
        if (!upInv->isBatteryPowered()) { upInv->setMaxOutput(); }
    }

    uint16_t targetOutput = 0;

    auto solarChargerOutput = SolarCharger.getStats()->getOutputPowerWatts();
    if (solarChargerOutput) {
        targetOutput = static_cast<uint16_t>(std::max<int32_t>(0, *solarChargerOutput));
        targetOutput = dcPowerBusToInverterAc(targetOutput);
    }

    _calculationBackoffMs = 1 * 1000;
    updateInverterLimits(targetOutput, sBatteryPoweredFilter, sBatteryPoweredExpression);
    return announceStatus(Status::UnconditionalSolarPassthrough);
}

uint8_t PowerLimiterClass::getInverterUpdateTimeouts() const
{
    uint8_t res = 0;
    for (auto const& upInv : _inverters) {
        res += upInv->getUpdateTimeouts();
    }
    return res;
}

uint8_t PowerLimiterClass::getPowerLimiterState() const
{
    bool reachable = false;
    bool producing = false;
    for (auto const& upInv : _inverters) {
        reachable |= upInv->isReachable();
        producing |= upInv->isProducing();
    }

    if (!reachable) {
        return PL_UI_STATE_INACTIVE;
    }

    if (!producing) {
        return PL_UI_STATE_CHARGING;
    }

    return ((_batteryState == BatteryState::DISCHARGE_ALLOWED || _batteryState == BatteryState::DISCHARGE_NIGHT))
        ? PL_UI_STATE_USE_SOLAR_AND_BATTERY : PL_UI_STATE_USE_SOLAR_ONLY;
}

void PowerLimiterClass::resetDynamicBatteryTargetState()
{
    _lastDynamicBatteryTargetPowerMeterUpdate = 0;
    _dynamicBatteryTargetMean = 0.0f;
    _dynamicBatteryTargetVariance = 0.0f;
    _dynamicBatteryTargetInitialized = false;
}

void PowerLimiterClass::updateDynamicBatteryTarget()
{
    auto const& config = Configuration.get();

    if (!config.PowerLimiter.Enabled
            || Mode::Normal != _mode
            || _inverters.empty()) {
        return;
    }

    if (!config.PowerLimiter.BatteryTargetPowerConsumptionDynamicEnabled
            || config.PowerLimiter.BatteryTargetPowerConsumptionDynamicWindow == 0) {
        resetDynamicBatteryTargetState();
        _batteryTargetPowerConsumption = config.PowerLimiter.BatteryTargetPowerConsumption;
        return;
    }

    if (!PowerMeter.isDataValid()) { return; }

    auto const powerMeterUpdate = PowerMeter.getLastUpdate();
    if (powerMeterUpdate == _lastDynamicBatteryTargetPowerMeterUpdate) { return; }

    auto const previousPowerMeterUpdate = _lastDynamicBatteryTargetPowerMeterUpdate;
    _lastDynamicBatteryTargetPowerMeterUpdate = powerMeterUpdate;

    auto const meterValue = PowerMeter.getPowerTotal();
    auto const watts = std::clamp<float>(
            meterValue - static_cast<float>(config.PowerLimiter.TargetPowerConsumption),
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max());

    if (!_dynamicBatteryTargetInitialized) {
        _dynamicBatteryTargetMean = watts;
        _dynamicBatteryTargetVariance = 0.0f;
        _dynamicBatteryTargetInitialized = true;
        return;
    }

    auto const alpha = calcExponentialSmoothingAlpha(
            powerMeterUpdate - previousPowerMeterUpdate,
            config.PowerLimiter.BatteryTargetPowerConsumptionDynamicWindow);
    if (alpha <= 0.0f) { return; }

    auto const previousMean = _dynamicBatteryTargetMean;
    auto const deviation = watts - previousMean;
    _dynamicBatteryTargetMean = previousMean + (alpha * deviation);
    _dynamicBatteryTargetVariance = (1.0f - alpha)
        * (_dynamicBatteryTargetVariance + (alpha * deviation * deviation));

    auto const calculatedTarget = calcBatteryTargetPowerConsumption();
    auto const smoothedTarget = _batteryTargetPowerConsumption
        + (alpha * (calculatedTarget - _batteryTargetPowerConsumption));
    _batteryTargetPowerConsumption = std::clamp<float>(
            smoothedTarget,
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max());
}

float PowerLimiterClass::getBatteryTargetPowerConsumption() const
{
    auto const& config = Configuration.get();

    if (!config.PowerLimiter.BatteryTargetPowerConsumptionDynamicEnabled
            || config.PowerLimiter.BatteryTargetPowerConsumptionDynamicWindow == 0) {
        return config.PowerLimiter.BatteryTargetPowerConsumption;
    }

    return _batteryTargetPowerConsumption;
}

float PowerLimiterClass::getTargetPowerConsumption() const
{
    auto const& config = Configuration.get();

    if (!config.PowerLimiter.TargetPowerConsumptionFollowStorageTarget) {
        return config.PowerLimiter.TargetPowerConsumption;
    }

    auto const target = getStorageTargetPowerConsumption()
        - static_cast<float>(config.PowerLimiter.TargetPowerConsumptionStorageOffset);
    return std::clamp<float>(
            target,
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max());
}

float PowerLimiterClass::getStorageTargetPowerConsumption() const
{
    return getBatteryTargetPowerConsumption();
}

bool PowerLimiterClass::isGridChargerManaged() const
{
    auto const& config = Configuration.get();
    return config.PowerLimiter.Enabled
        && config.GridCharger.Enabled
        && config.GridCharger.AutoPowerEnabled
        && Mode::Normal == _mode
        && !Battery.getStats()->getImmediateChargingRequest();
}

float PowerLimiterClass::calcBatteryTargetPowerConsumption() const
{
    auto const& config = Configuration.get();
    auto const staticTarget = static_cast<float>(config.PowerLimiter.BatteryTargetPowerConsumption);

    if (!config.PowerLimiter.BatteryTargetPowerConsumptionDynamicEnabled
            || !_dynamicBatteryTargetInitialized) {
        return staticTarget;
    }

    auto const maxDynamicBias = static_cast<float>(std::numeric_limits<int16_t>::max());
    auto const staticBias = staticTarget < 0.0f
        ? std::min(-staticTarget, maxDynamicBias)
        : 0.0f;
    auto const configuredMaxBias = std::min<float>(
            config.PowerLimiter.BatteryTargetPowerConsumptionDynamicMax,
            maxDynamicBias);
    auto const maxBias = std::max(configuredMaxBias, staticBias);
    auto const multiplier = std::max(0.0f, config.PowerLimiter.BatteryTargetPowerConsumptionDynamicMultiplier);
    auto calculatedBias = std::sqrt(std::max(0.0f, _dynamicBatteryTargetVariance)) * multiplier;
    calculatedBias = std::clamp(calculatedBias, 0.0f, maxDynamicBias);
    auto const dynamicBias = std::min(
            maxBias,
            std::max(staticBias, calculatedBias));

    return -dynamicBias;
}

uint16_t PowerLimiterClass::calcTargetOutput() const
{
    return calcTargetOutput(getTargetPowerConsumption(), 0);
}

uint16_t PowerLimiterClass::calcTargetOutput(float targetConsumption) const
{
    return calcTargetOutput(targetConsumption, 0);
}

uint16_t PowerLimiterClass::calcTargetOutput(float targetConsumption, int32_t meterAdjustmentWatts) const
{
    auto const& config = Configuration.get();
    auto baseLoad = config.PowerLimiter.BaseLoadLimit;

    auto meterValid = PowerMeter.isDataValid();
    auto meterValue = PowerMeter.getPowerTotal();

    DTU_LOGD("targeting %.1f W, base load is %u W, power meter reads %.1f W (%s)",
            targetConsumption, baseLoad, meterValue,
            (meterValid?"valid":"stale"));

    if (!meterValid) { return baseLoad; }

    // the desired total output of all eligible inverters is whatever they are
    // producing right now plus the difference between the target consumption
    // and the power meter reading
    float adjustedMeterValue = meterValue + meterAdjustmentWatts;

    // we have to correct the meter reading if there are inverters connected to
    // AC between the grid (billing meter) and FluxDTU's power meter.
    // example: billing meter in the basement, inverter connected next to it,
    // and an additional power meter in the flat which is read by FluxDTU. In
    // that case power produced by the respective inverter is
    // still registered as consumed power by the power meter as it flows into
    // the household, even though it is not billed. essentially, we derive the
    // billing meter's reading, whose value we actually want to optimize to
    // reach the target consumption setting value.
    for (auto const& upInv : _inverters) {
        if (upInv->isBehindPowerMeter()) { continue; }

        // it is to be expected that solar-powered inverters are unreachable
        // during the night, in which case we don't want to account for their
        // last reported AC output, as they are not producing power.
        auto isDayPeriod = SunPosition.isDayPeriod();
        if (upInv->isSolarPowered() && !upInv->isReachable() && !isDayPeriod) { continue; }

        // in all other cases, even for unreachable inverters, we assume that
        // they still produce the amount of AC output that they last reported.
        // if we assumed unreachable inverters are not producing, we will
        // potentially produce way too much power. as information is missing
        // that could make sure we do the right thing, we have to make an
        // assumption about unreachable inverters.
        adjustedMeterValue -= upInv->getCurrentOutputAcWatts();
    }

    int32_t currentTotalOutput = 0;
    for (auto const& upInv : _inverters) {
        // non-eligible inverters don't participate in this DPL round at all.
        // inverters in standby report 0 W output, so we can iterate them.
        if (!upInv->isEligible()) { continue; }

        currentTotalOutput += upInv->getCurrentOutputAcWatts();
    }

    // this value is negative if we are exporting more than "targetConsumption"
    // power to the grid using generators other than DPL-governed inverters.
    float targetOutput = static_cast<float>(currentTotalOutput)
        + adjustedMeterValue
        - targetConsumption;

    // if we are already exporting more power than the (negative) target
    // consumption value allows us to, we don't want DPL-governed inverters to
    // produce any power at all.
    if (targetOutput <= 0.0f) { return 0; }

    return static_cast<uint16_t>(std::round(std::min<float>(
            targetOutput,
            std::numeric_limits<uint16_t>::max())));
}

/**
 * assigns new limits to all inverters matching the filter. returns the total
 * amount of power these inverters are expected to produce after the new limits
 * were applied.
 */
uint16_t PowerLimiterClass::updateInverterLimits(uint16_t powerRequested,
        PowerLimiterClass::inverter_filter_t filter, std::string const& filterExpression,
        bool allowStandby)
{
    std::vector<PowerLimiterInverter*> matchingInverters;
    uint16_t producing = 0; // sum of AC power the matching inverters produce now

    for (auto& upInv : _inverters) {
        if (!filter(*upInv)) { continue; }

        if (!upInv->isEligible()) { continue; }

        producing += upInv->getCurrentOutputAcWatts();
        matchingInverters.push_back(upInv.get());
    }

    if (matchingInverters.empty()) { return 0; }

    // if we update battery-powered inverters and the battery is in the STOP state,
    // we must put all battery-powered inverters into standby mode,
    // regardless of whether the standby option is enabled or not.
    if ((matchingInverters[0]->isBatteryPowered()) && (_batteryState == BatteryState::STOP)) {
        for (auto pInv : matchingInverters) { pInv->standby(); }
        DTU_LOGD("battery is in STOP state, all battery-powered inverters are put into standby.");
        return 0;
    }

    int32_t diff = powerRequested - producing;

    auto const& config = Configuration.get();
    uint16_t hysteresis = config.PowerLimiter.TargetPowerConsumptionHysteresis;

    bool plural = matchingInverters.size() != 1;
    DTU_LOGD("requesting %d W from %d %s inverter%s currently "
            "producing %d W (diff %i W, hysteresis %d W)",
            powerRequested, matchingInverters.size(), filterExpression.c_str(),
            (plural?"s":""), producing, diff, hysteresis);

    bool limitRefreshScheduled = false;
    for (auto pInv : matchingInverters) {
        limitRefreshScheduled |= pInv->applyConfiguredLimitBounds();
    }

    if (limitRefreshScheduled) {
        uint16_t covered = 0;
        for (auto pInv : matchingInverters) {
            covered += pInv->getExpectedOutputAcWatts();
        }
        return covered;
    }

    // if 0 W are requested, we set hysteresis to 0 to basically ignore it
    // which allows battery-powered inverters to go into standby and avoid
    // that the battery gets fully discharged.
    if (powerRequested == 0) {
        hysteresis = 0;
    }

    if (std::abs(diff) < static_cast<int32_t>(hysteresis)) {
        bool periodicRefreshScheduled = false;
        for (auto pInv : matchingInverters) {
            periodicRefreshScheduled |= pInv->refreshStaleLimit();
        }

        uint16_t const thermalExchangeBudget = diff <= 0
            ? getMeasuredThermalExchangeBudgetWatts(hysteresis, getTargetPowerConsumption())
            : 0;
        if (!periodicRefreshScheduled
                && !rebalanceThermally(matchingInverters, filterExpression, thermalExchangeBudget)) {
            return producing;
        }

        uint16_t covered = 0;
        for (auto pInv : matchingInverters) {
            covered += pInv->getExpectedOutputAcWatts();
        }
        return covered;
    }

    uint16_t covered = 0;

    if (diff < 0) {
        uint16_t reduction = static_cast<uint16_t>(diff * -1);
        uint16_t const measuredThermalExchangeBudget =
            getMeasuredThermalExchangeBudgetWatts(hysteresis, getTargetPowerConsumption());

        std::vector<DistributedChange> changes;
        for (auto const pInv : matchingInverters) {
            uint16_t maxReduction = pInv->getMaxReductionWatts(false/*no standby*/);
            if (maxReduction == 0) { continue; }
            changes.push_back({ pInv, maxReduction });
        }

        applyThermalWeights(changes, matchingInverters,
                ThermalPreference::Hotter, filterExpression);
        distributeByWeight(reduction, changes);
        uint16_t appliedReduction = 0;
        for (auto& change : changes) {
            if (change.amount == 0) { continue; }
            appliedReduction += change.inverter->applyReduction(change.amount, false/*no standby*/);
        }

        uint16_t remainingReduction = (reduction > appliedReduction) ? reduction - appliedReduction : 0;
        if (remainingReduction > 0 && allowStandby) {
            std::vector<DistributedChange> standbyChanges;
            for (auto const pInv : matchingInverters) {
                uint16_t maxReductionWithoutStandby = pInv->getMaxReductionWatts(false/*no standby*/);
                uint16_t maxReductionWithStandby = pInv->getMaxReductionWatts(true/*allow standby*/);
                if (maxReductionWithStandby <= maxReductionWithoutStandby) { continue; }
                standbyChanges.push_back({ pInv, static_cast<uint16_t>(maxReductionWithStandby - maxReductionWithoutStandby) });
            }

            std::sort(standbyChanges.begin(), standbyChanges.end(),
                    [](auto const& a, auto const& b) {
                        return a.capacity < b.capacity;
                    });

            for (auto& change : standbyChanges) {
                if (remainingReduction == 0) { break; }
                change.inverter->standby();
                remainingReduction = (remainingReduction > change.capacity)
                    ? remainingReduction - change.capacity
                    : 0;
            }
        }

        uint16_t const remainingThermalExchangeBudget =
            measuredThermalExchangeBudget > reduction
                ? static_cast<uint16_t>(measuredThermalExchangeBudget - reduction)
                : 0;
        rebalanceThermally(matchingInverters, filterExpression,
                remainingThermalExchangeBudget);

        for (auto pInv : matchingInverters) {
            covered += pInv->getExpectedOutputAcWatts();
        }
    }
    else {
        uint16_t increase = static_cast<uint16_t>(diff);

        std::vector<DistributedChange> changes;
        for (auto const pInv : matchingInverters) {
            uint16_t maxIncrease = pInv->getMaxIncreaseWatts();
            if (maxIncrease == 0) { continue; }
            changes.push_back({ pInv, maxIncrease });
        }

        // When output is short, hitting the power target has priority. Thermal
        // exchange is handled separately once there is no active shortfall.
        distributeByWeight(increase, changes);
        for (auto& change : changes) {
            if (change.amount == 0) { continue; }
            change.inverter->applyIncrease(change.amount);
        }

        rebalanceThermally(matchingInverters, filterExpression, 0);

        for (auto pInv : matchingInverters) {
            covered += pInv->getExpectedOutputAcWatts();
        }
    }

    DTU_LOGD("will cover %d W using %d %s inverter%s",
            covered, matchingInverters.size(),
            filterExpression.c_str(), (plural?"s":""));

    return covered;
}

// calculates how much power the battery-powered inverters shall draw from the
// power bus, which we call the part of the circuitry that is supplied by the
// solar charge controller(s), possibly an AC charger, as well as the battery.
uint16_t PowerLimiterClass::calcPowerBusUsage(uint16_t powerRequested) const
{
    // We check if the PSU is on and disable battery-powered inverters in this
    // case. The PSU should reduce power or shut down first before the
    // battery-powered inverters kick in. The only case where this is not
    // desired is if the battery is over the Full Solar Passthrough Threshold.
    // In this case battery-powered inverters should produce power and the PSU
    // will shut down as a consequence.
    if (!isGridChargerManaged()
            && !isFullSolarPassthroughActive()
            && GridCharger.getAutoPowerStatus()) {
        DTU_LOGD("DC power bus usage blocked by GridCharger auto power");
        return 0;
    }

    if (Battery.getStats()->getImmediateChargingRequest()) {
        DTU_LOGD("DC power bus usage blocked by immediate charging request");
        return 0;
    }

    if (_batteryState == BatteryState::STOP) {
        DTU_LOGD("DC power bus usage blocked by battery below the stop threshold");
        return 0;
    }

    auto solarOutputDc = getSolarPassthroughPower();
    auto solarOutputAc = dcPowerBusToInverterAc(solarOutputDc);
    if (isFullSolarPassthroughActive() && solarOutputAc > powerRequested) {
        DTU_LOGD("using %u/%u W DC/AC from DC power bus (full solar-passthrough)",
                solarOutputDc, solarOutputAc);

        return solarOutputAc;
    }

    auto oBatteryDischargeLimit = getBatteryDischargeLimit();
    if (!oBatteryDischargeLimit) {
        DTU_LOGD("granting %d W from DC power bus (no battery discharge "
                "limit), solar power is %u/%u W DC/AC",
                powerRequested, solarOutputDc, solarOutputAc);
        return powerRequested;
    }

    auto batteryAllowanceAc = dcPowerBusToInverterAc(*oBatteryDischargeLimit);

    DTU_LOGD("battery allowance is %u/%u W DC/AC, solar power is %u/%u W DC/AC, "
            "requested are %u W AC",
            *oBatteryDischargeLimit, batteryAllowanceAc,
            solarOutputDc, solarOutputAc, powerRequested);

    uint16_t allowance = batteryAllowanceAc + solarOutputAc;
    return std::min(powerRequested, allowance);
}

bool PowerLimiterClass::updateInverters()
{
    bool busy = false;

    for (auto& upInv : _inverters) {
        if (upInv->update()) { busy = true; }
    }

    auto iter = _retirees.begin();
    while (iter != _retirees.end()) {
        if ((*iter)->retire()) {
            busy = true;
            ++iter;
            continue;
        }

        iter = _retirees.erase(iter);
    }

    return busy;
}

uint16_t PowerLimiterClass::getSolarPassthroughPower() const
{
    if (!isSolarPassThroughEnabled() || isBelowStopThreshold()) {
        return 0;
    }

    std::optional<float> oSolarChargerOutput = SolarCharger.getStats()->getOutputPowerWatts();

    // This value can be negative if a charge controller with a load output is used
    // and the load is consuming more power than the charge controller is producing.
    return std::max<float>(0, oSolarChargerOutput.value_or(0));
}

float PowerLimiterClass::getBatteryInvertersOutputAcWatts() const
{
    float res = 0;

    for (auto const& upInv : _inverters) {
        if (!upInv->isBatteryPowered()) { continue; }
        // TODO(schlimmchen): we must use the DC power instead, as the battery
        // voltage drops proportional to the DC current draw, but the AC power
        // output does not correlate with the battery current or voltage.
        res += upInv->getCurrentOutputAcWatts();
    }

    return res;
}

uint16_t PowerLimiterClass::getCurrentInvertersOutputAcWatts() const
{
    uint32_t res = 0;

    for (auto const& upInv : _inverters) {
        if (!upInv->isEligible()) { continue; }
        res += upInv->getCurrentOutputAcWatts();
    }

    return static_cast<uint16_t>(std::min<uint32_t>(
            res,
            std::numeric_limits<uint16_t>::max()));
}

uint16_t PowerLimiterClass::getBatteryInvertersMinOutputAcWatts() const
{
    uint32_t res = 0;

    for (auto const& upInv : _inverters) {
        if (!upInv->isBatteryPowered()) { continue; }
        if (!upInv->isEligible()) { continue; }
        if (!upInv->isProducing()) { continue; }

        auto const currentOutput = upInv->getCurrentOutputAcWatts();
        auto const reducibleWithoutStandby = upInv->getMaxReductionWatts(false/*no standby*/);
        res += (currentOutput > reducibleWithoutStandby)
            ? currentOutput - reducibleWithoutStandby
            : 0;
    }

    return static_cast<uint16_t>(std::min<uint32_t>(
            res, std::numeric_limits<uint16_t>::max()));
}

std::optional<uint16_t> PowerLimiterClass::getBatteryDischargeLimit() const
{
    if ((_batteryState == BatteryState::STOP) || (_batteryState == BatteryState::NO_DISCHARGE)) { return 0; }

    auto currentLimit = Battery.getDischargeCurrentLimit();
    if (currentLimit == FLT_MAX) { return std::nullopt; }

    if (currentLimit <= 0) { currentLimit = -currentLimit; }

    // this uses inverter voltage since there is a voltage drop between
    // battery and inverter, so since we are regulating the inverter
    // power we should use its voltage.
    auto inverter = getInverterDcVoltage();
    if (inverter.first <= 0) {
        DTU_LOGE("could not determine inverter voltage");
        return 0;
    }

    return inverter.first * currentLimit;
}

bool PowerLimiterClass::testThreshold(float socThreshold, float voltThreshold,
        std::function<bool(float, float)> compare) const
{
    auto const& config = Configuration.get();

    // prefer SoC provided through battery interface, unless disabled by user
    auto stats = Battery.getStats();
    if (!config.PowerLimiter.IgnoreSoc
            && config.Battery.Enabled
            && socThreshold > 0.0
            && stats->isSoCValid()
            && stats->getSoCAgeSeconds() < 60) {
              return compare(stats->getSoC(), socThreshold);
    }

    // use voltage threshold as fallback
    if (voltThreshold <= 0.0) { return false; }

    return compare(_loadCorrectedVoltage, voltThreshold);
}

bool PowerLimiterClass::isStartThresholdReached() const
{
    auto const& config = Configuration.get();

    return testThreshold(
            config.PowerLimiter.BatterySocStartThreshold,
            config.PowerLimiter.VoltageStartThreshold,
            [](float a, float b) -> bool { return a >= b; }
    );
}

bool PowerLimiterClass::isStopThresholdReached() const
{
    auto const& config = Configuration.get();

    return testThreshold(
            config.PowerLimiter.BatterySocStopThreshold,
            config.PowerLimiter.VoltageStopThreshold,
            [](float a, float b) -> bool { return a <= b; }
    );
}

bool PowerLimiterClass::isBelowStopThreshold() const
{
    auto const& config = Configuration.get();

    return testThreshold(
            config.PowerLimiter.BatterySocStopThreshold,
            config.PowerLimiter.VoltageStopThreshold,
            [](float a, float b) -> bool { return a < b; }
    );
}

void PowerLimiterClass::calcNextInverterRestart()
{
    if (!usesBatteryPoweredInverter() && !usesSmartBufferPoweredInverter()) {
        _nextInverterRestart = { false, 0 };
        DTU_LOGD("automatic inverter restart disabled");
        return;
    }

    auto const& config = Configuration.get();
    struct tm timeinfo;
    getLocalTime(&timeinfo, 5); // always succeeds as we call this method only
                                // from the DPL loop *after* we already made
                                // sure that time information is available.

    // calculation first step is offset to next restart in minutes
    uint16_t dayMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    uint16_t targetMinutes = config.PowerLimiter.RestartHour * 60;
    uint32_t restartMillis = 0;
    if (config.PowerLimiter.RestartHour > timeinfo.tm_hour) {
        // next restart is on the same day
        restartMillis = targetMinutes - dayMinutes;
    } else {
        // next restart is on next day
        restartMillis = 1440 - dayMinutes + targetMinutes;
    }

    DTU_LOGD("Localtime read %02d:%02d / configured RestartHour %d",
            timeinfo.tm_hour, timeinfo.tm_min, config.PowerLimiter.RestartHour);
    DTU_LOGD("dayMinutes %d / targetMinutes %d", dayMinutes, targetMinutes);
    DTU_LOGD("next inverter restart in %d minutes", restartMillis);

    // convert unit for next restart to milliseconds and add current uptime
    restartMillis *= 60000;
    restartMillis += millis();

    DTU_LOGI("next inverter restart @ %d millis", restartMillis);

    _nextInverterRestart = { true, restartMillis };
}

bool PowerLimiterClass::isSolarPassThroughEnabled() const
{
    auto const& config = Configuration.get();

    // solar passthrough only applies to setups with battery-powered inverters
    if (!usesBatteryPoweredInverter()) { return false; }

    // solarcharger is needed for solar passthrough
    if (!config.SolarCharger.Enabled) { return false; }

    return config.PowerLimiter.SolarPassThroughEnabled;
}

bool PowerLimiterClass::usesBatteryPoweredInverter() const
{
    for (auto const& upInv : _inverters) {
        if (upInv->isBatteryPowered()) { return true; }
    }

    return false;
}

bool PowerLimiterClass::usesSmartBufferPoweredInverter() const
{
    for (auto const& upInv : _inverters) {
        if (upInv->isSmartBufferPowered()) { return true; }
    }

    return false;
}

bool PowerLimiterClass::isGovernedBatteryPoweredInverterProducing() const
{
    for (auto const& upInv : _inverters) {
        if (upInv->isBatteryPowered() && upInv->isProducing()) { return true; }
    }
    return false;
}
