// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Thomas Basler and others
 */

#include <battery/Controller.h>
#include <battery/Stats.h>
#include <powermeter/Controller.h>
#include <Hoymiles.h>
#include "PowerLimiter.h"
#include "PowerLimiterBatteryCurrentLimit.h"
#include "Configuration.h"
#include "MqttSettings.h"
#include "NetworkSettings.h"
#include <gridcharger/Controller.h>
#include <solarcharger/Controller.h>
#include <LittleFS.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <cinttypes>
#include <esp_heap_caps.h>
#include <limits>
#include <vector>
#include <frozen/map.h>
#include "SunPosition.h"
#include <LogHelper.h>

#undef TAG
static const char* TAG = "dynamicPowerLimiter";
static const char* SUBTAG = "Controller";
static const char* RUNTIME_STATE_FILENAME = "/powerlimiter_state.json";
static const char* RUNTIME_STATE_TEMP_FILENAME = "/powerlimiter_state.tmp";
static constexpr uint32_t PredictiveQueuedRequestStaleMillis = 35 * 1000;

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

static uint32_t adaptivePlannerMaxIntervalMillis(PowerLimiterConfig const& powerLimiterConfig)
{
    auto const seconds = std::max<uint32_t>(
            1,
            powerLimiterConfig.AdaptivePlannerMaxInterval);
    return seconds * 1000U;
}

static float wattSecondsToWattMillis(uint32_t wattSeconds)
{
    return static_cast<float>(wattSeconds) * 1000.0f;
}

static uint32_t currentEpochSeconds()
{
    auto const now = std::time(nullptr);
    if (now <= 0) { return 0; }
    return static_cast<uint32_t>(now);
}

static bool epochLooksValid(uint32_t timestamp)
{
    // 2020-01-01; lower values indicate that SNTP has not populated wall time.
    return timestamp >= 1577836800UL;
}

static String serialToJsonString(uint64_t serial)
{
    char buffer[17];
    snprintf(buffer, sizeof(buffer), "%0x%08x",
            static_cast<uint32_t>((serial >> 32) & 0xFFFFFFFF),
            static_cast<uint32_t>(serial & 0xFFFFFFFF));
    return String(buffer);
}

static uint64_t serialFromJsonString(String const& serial)
{
    return strtoull(serial.c_str(), nullptr, 16);
}

static int16_t clampToInt16(float value)
{
    return static_cast<int16_t>(std::clamp<float>(
            std::round(value),
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max()));
}

static void hashValue(uint32_t& hash, uint32_t value)
{
    for (uint8_t i = 0; i < 4; ++i) {
        hash ^= (value >> (i * 8)) & 0xff;
        hash *= 16777619UL;
    }
}

static bool readUint16(JsonObjectConst source, char const* key, uint16_t& target)
{
    if (!source[key].is<int>()) { return false; }
    auto const value = source[key].as<int>();
    if (value < 0 || value > std::numeric_limits<uint16_t>::max()) { return false; }
    target = static_cast<uint16_t>(value);
    return true;
}

static bool readInt16(JsonObjectConst source, char const* key, int16_t& target)
{
    if (!source[key].is<int>()) { return false; }
    auto const value = source[key].as<int>();
    if (value < std::numeric_limits<int16_t>::min()
            || value > std::numeric_limits<int16_t>::max()) {
        return false;
    }
    target = static_cast<int16_t>(value);
    return true;
}

namespace Predictive = PowerLimiterPredictiveControl;

static std::string predictiveInverterKey(PowerLimiterInverter const& inverter)
{
    return std::string("inverter:") + inverter.getSerialStr();
}

static char const* sPredictiveGridChargerKey = "gridcharger:auto";

enum class PredictiveHistorySource : uint8_t {
    Battery,
    Solar,
    SmartBuffer,
    GridCharger,
};

static uint8_t predictiveHistorySourceValue(PredictiveHistorySource source)
{
    return static_cast<uint8_t>(source);
}

static char const* predictiveHistorySourceText(uint8_t source)
{
    switch (static_cast<PredictiveHistorySource>(source)) {
    case PredictiveHistorySource::Battery: return "battery";
    case PredictiveHistorySource::Solar: return "solar";
    case PredictiveHistorySource::SmartBuffer: return "smart_buffer";
    case PredictiveHistorySource::GridCharger: return "grid_charger";
    }
    return "unknown";
}

static uint8_t predictiveSourceFromActuatorKind(Predictive::ActuatorKind kind)
{
    switch (kind) {
    case Predictive::ActuatorKind::Battery:
        return predictiveHistorySourceValue(PredictiveHistorySource::Battery);
    case Predictive::ActuatorKind::Solar:
        return predictiveHistorySourceValue(PredictiveHistorySource::Solar);
    case Predictive::ActuatorKind::SmartBuffer:
        return predictiveHistorySourceValue(PredictiveHistorySource::SmartBuffer);
    case Predictive::ActuatorKind::Charger:
        return predictiveHistorySourceValue(PredictiveHistorySource::GridCharger);
    }
    return predictiveHistorySourceValue(PredictiveHistorySource::Battery);
}

static uint8_t predictiveSourceFromConfig(PowerLimiterInverterConfig::InverterPowerSource source)
{
    switch (source) {
    case PowerLimiterInverterConfig::InverterPowerSource::Battery:
        return predictiveHistorySourceValue(PredictiveHistorySource::Battery);
    case PowerLimiterInverterConfig::InverterPowerSource::Solar:
        return predictiveHistorySourceValue(PredictiveHistorySource::Solar);
    case PowerLimiterInverterConfig::InverterPowerSource::SmartBuffer:
        return predictiveHistorySourceValue(PredictiveHistorySource::SmartBuffer);
    }
    return predictiveHistorySourceValue(PredictiveHistorySource::Battery);
}

static uint8_t predictiveSourceFromPowerLimiterSerial(CONFIG_T const& config, uint64_t serial)
{
    for (auto const& inverter : config.PowerLimiter.Inverters) {
        if (inverter.Serial == 0ULL) { break; }
        if (inverter.Serial == serial) {
            return predictiveSourceFromConfig(inverter.PowerSource);
        }
    }
    return predictiveHistorySourceValue(PredictiveHistorySource::Solar);
}

static uint64_t predictiveSerialFromActuatorKey(std::string const& actuatorKey)
{
    static char const prefix[] = "inverter:";
    if (actuatorKey.rfind(prefix, 0) != 0) { return 0; }
    return strtoull(actuatorKey.c_str() + strlen(prefix), nullptr, 16);
}

static uint8_t inverterIndexFromSerial(CONFIG_T const& config, uint64_t serial)
{
    for (uint8_t i = 0; i < INV_MAX_COUNT; ++i) {
        if (config.Inverter[i].Serial == serial) { return i; }
    }
    return 0xff;
}

static uint8_t inverterOrderFromSerial(CONFIG_T const& config, uint64_t serial)
{
    for (uint8_t i = 0; i < INV_MAX_COUNT; ++i) {
        if (config.Inverter[i].Serial == serial) { return config.Inverter[i].Order; }
    }
    return 0xff;
}

static String inverterNameFromSerial(CONFIG_T const& config, uint64_t serial)
{
    for (uint8_t i = 0; i < INV_MAX_COUNT; ++i) {
        auto const& inverter = config.Inverter[i];
        if (inverter.Serial != serial) { continue; }
        return inverter.Name[0] != '\0' ? String(inverter.Name) : serialToJsonString(serial);
    }
    return serial == 0 ? String() : serialToJsonString(serial);
}

static uint32_t epochSecondsFromMillis(uint32_t eventMillis, uint32_t nowMillis, uint32_t nowEpoch)
{
    if (eventMillis == 0 || !epochLooksValid(nowEpoch)) { return 0; }

    const auto ageSeconds = (nowMillis - eventMillis) / 1000;
    if (ageSeconds > nowEpoch) { return 0; }
    return nowEpoch - ageSeconds;
}

static char const* predictiveRequestDomainText(Predictive::RequestDomain domain)
{
    switch (domain) {
    case Predictive::RequestDomain::Storage: return "storage";
    case Predictive::RequestDomain::Global: return "global";
    }
    return "unknown";
}

static char const* predictiveRequestCauseText(Predictive::RequestCause cause)
{
    switch (cause) {
    case Predictive::RequestCause::Normal: return "normal";
    case Predictive::RequestCause::Safety: return "safety";
    case Predictive::RequestCause::Manual: return "manual";
    case Predictive::RequestCause::Recovery: return "recovery";
    case Predictive::RequestCause::Thermal: return "thermal";
    }
    return "unknown";
}

static char const* predictiveRequestStateText(Predictive::RequestState state)
{
    switch (state) {
    case Predictive::RequestState::Queued: return "queued";
    case Predictive::RequestState::SupersededBeforeSend: return "superseded_before_send";
    case Predictive::RequestState::Sent: return "sent";
    case Predictive::RequestState::Accepted: return "accepted";
    case Predictive::RequestState::Rejected: return "rejected";
    case Predictive::RequestState::FailedNoEffect: return "failed_no_effect";
    case Predictive::RequestState::FailedAmbiguous: return "failed_ambiguous";
    case Predictive::RequestState::SettledByMeter: return "settled_by_meter";
    case Predictive::RequestState::SettledByTelemetry: return "settled_by_telemetry";
    case Predictive::RequestState::SettledByTimeout: return "settled_by_timeout";
    case Predictive::RequestState::SettledSuperseded: return "settled_superseded";
    case Predictive::RequestState::SettledDiscarded: return "settled_discarded";
    }
    return "unknown";
}

static char const* predictiveEffectKindText(Predictive::EffectKind effectKind)
{
    switch (effectKind) {
    case Predictive::EffectKind::Deterministic: return "deterministic";
    case Predictive::EffectKind::SolarCapacityLimited: return "solar_capacity_limited";
    case Predictive::EffectKind::Ambiguous: return "ambiguous";
    }
    return "unknown";
}

static char const* predictiveTransitionKindText(Predictive::TransitionKind transitionKind)
{
    switch (transitionKind) {
    case Predictive::TransitionKind::Setpoint: return "setpoint";
    case Predictive::TransitionKind::Startup: return "startup";
    case Predictive::TransitionKind::Standby: return "standby";
    }
    return "unknown";
}

static char const* predictiveCorrectionActionText(Predictive::CorrectionAction action)
{
    switch (action) {
    case Predictive::CorrectionAction::Hold: return "hold";
    case Predictive::CorrectionAction::MoreEffectiveOutput: return "more_effective_output";
    case Predictive::CorrectionAction::LessEffectiveOutput: return "less_effective_output";
    }
    return "unknown";
}

static char const* radioRequestStateText(HoymilesRadio::RequestHistoryState state)
{
    switch (state) {
    case HoymilesRadio::RequestHistoryState::Queued: return "rf_queued";
    case HoymilesRadio::RequestHistoryState::InProcess: return "rf_in_process";
    case HoymilesRadio::RequestHistoryState::Success: return "rf_success";
    case HoymilesRadio::RequestHistoryState::Failed: return "rf_failed";
    }
    return "rf_failed";
}

static constexpr uint32_t sInverterRfQueueCongestionThreshold = 4;
static constexpr uint32_t sBatteryTargetEffectAssumptionMillis = 3 * 1000;
static constexpr float sAdaptivePlannerMinOutsideUncertaintyRatio = 0.5f;
static constexpr float sBatteryStandbyReassertThresholdWatts = 10.0f;
static constexpr float sPendingRfCounterTargetDeadbandWatts = 5.0f;
static constexpr uint16_t sGridChargerStagedTargetDeadbandWatts = 5;

enum class DplGuardLog : uint8_t {
    InverterRfCongested,
    BatteryIncreaseBlockedByCharger,
    ChargerIncreaseBlockedByStorageOutput,
    ChargerIncreaseBlockedByGlobalDemand,
    StorageChargerOverlapResolved,
    StorageBatteryCorrectionDeferredByPendingRf,
    StorageBatteryCounterTargetDeferredByPendingRf,
    PlannerTargetDeferredByPendingRf,
    GridChargerSocPlanCap,
    InvalidMeterGridChargerShed,
    EmergencyGridChargerShed,
    PlannerPowerMeterSampleChanged,
    Count,
};

static bool shouldLogDplGuard(DplGuardLog guard, uint32_t intervalMillis = 10 * 1000)
{
    static uint32_t sLastLogMillis[static_cast<uint8_t>(DplGuardLog::Count)] = {};
    auto const index = static_cast<uint8_t>(guard);
    if (index >= static_cast<uint8_t>(DplGuardLog::Count)) { return false; }

    auto const now = millis();
    if (sLastLogMillis[index] == 0 || (now - sLastLogMillis[index]) >= intervalMillis) {
        sLastLogMillis[index] = now;
        return true;
    }
    return false;
}

static bool predictiveRequestKeepsSetpointPending(
        Predictive::PendingControlRequest const& request)
{
    return request.state == Predictive::RequestState::Queued
        || request.isPhysicalPending();
}

static void logPredictiveDecisionEvent(
        bool dueBootstrap,
        bool dueGrid,
        bool dueTarget,
        bool dueTargetBand,
        bool dueInterval,
        uint32_t lastPlannerAgeMillis,
        float gridWatts,
        float storageTargetWatts,
        float globalTargetWatts,
        Predictive::ControlForecast const& storageForecast,
        Predictive::CorrectionDecision const& storageDecision,
        Predictive::ControlForecast const& globalForecast,
        Predictive::CorrectionDecision const& globalDecision,
        float plannerGridDeviationWatts,
        float plannerGridUncertaintyWatts,
        float plannerGridOutsideUncertaintyRatio,
        float plannerTargetBandErrorWatts,
        float plannerTargetBandErrorIntegralWattMillis,
        float plannerTargetBandErrorThresholdWattMillis,
        bool chargerMayBeActive,
        bool storageOutputMayBeActive,
        size_t ledgerSize,
        size_t activeRequestCount)
{
    if (!DTU_LOG_IS_DEBUG) { return; }

    DTU_LOGD("dpl.decision.v1 part=due due_bootstrap=%u due_grid=%u "
            "due_target=%u due_target_band=%u due_interval=%u "
            "last_planner_age_ms=%" PRIu32 " grid_w=%.0f "
            "storage_target_w=%.0f global_target_w=%.0f",
            dueBootstrap ? 1U : 0U,
            dueGrid ? 1U : 0U,
            dueTarget ? 1U : 0U,
            dueTargetBand ? 1U : 0U,
            dueInterval ? 1U : 0U,
            lastPlannerAgeMillis,
            gridWatts,
            storageTargetWatts,
            globalTargetWatts);
    DTU_LOGD("dpl.decision.v1 part=storage valid=%u min_w=%.0f nominal_w=%.0f "
            "max_w=%.0f pending=%u action=%s residual_w=%.0f edge_w=%.0f "
            "reason=%s",
            storageForecast.valid ? 1U : 0U,
            storageForecast.valid ? storageForecast.gridMinWatts : 0.0f,
            storageForecast.valid ? storageForecast.gridNominalWatts : 0.0f,
            storageForecast.valid ? storageForecast.gridMaxWatts : 0.0f,
            storageForecast.valid ? static_cast<unsigned>(storageForecast.pendingCount) : 0U,
            predictiveCorrectionActionText(storageDecision.action),
            storageDecision.residualEffectiveWatts,
            storageDecision.nearestCorridorEdgeWatts,
            storageDecision.reason);
    DTU_LOGD("dpl.decision.v1 part=global valid=%u min_w=%.0f nominal_w=%.0f "
            "max_w=%.0f pending=%u action=%s residual_w=%.0f edge_w=%.0f "
            "reason=%s",
            globalForecast.valid ? 1U : 0U,
            globalForecast.valid ? globalForecast.gridMinWatts : 0.0f,
            globalForecast.valid ? globalForecast.gridNominalWatts : 0.0f,
            globalForecast.valid ? globalForecast.gridMaxWatts : 0.0f,
            globalForecast.valid ? static_cast<unsigned>(globalForecast.pendingCount) : 0U,
            predictiveCorrectionActionText(globalDecision.action),
            globalDecision.residualEffectiveWatts,
            globalDecision.nearestCorridorEdgeWatts,
            globalDecision.reason);
    DTU_LOGD("dpl.decision.v1 part=planner deviation_w=%.0f uncertainty_w=%.0f "
            "ratio=%.2f target_band_error_w=%.0f "
            "target_band_integral_wms=%.0f target_band_threshold_wms=%.0f "
            "ledger=%u active=%u charger_active=%u "
            "storage_output_active=%u",
            plannerGridDeviationWatts,
            plannerGridUncertaintyWatts,
            std::isfinite(plannerGridOutsideUncertaintyRatio)
                ? plannerGridOutsideUncertaintyRatio
                : 9999.0f,
            plannerTargetBandErrorWatts,
            plannerTargetBandErrorIntegralWattMillis,
            plannerTargetBandErrorThresholdWattMillis,
            static_cast<unsigned>(ledgerSize),
            static_cast<unsigned>(activeRequestCount),
            chargerMayBeActive ? 1U : 0U,
            storageOutputMayBeActive ? 1U : 0U);
}

static int16_t clampTraceWatts(float watts)
{
    return static_cast<int16_t>(std::clamp<float>(
            std::round(watts),
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max()));
}

static float medianOfThree(float a, float b, float c)
{
    return a + b + c
        - std::min(a, std::min(b, c))
        - std::max(a, std::max(b, c));
}

struct DistributedChange {
    PowerLimiterInverter* inverter;
    uint16_t capacity;
    uint16_t amount = 0;
    uint16_t weight = 1000;
    float thermalScore = 0.0f;
    float fallbackThermalScore = 0.0f;
};

enum class ThermalPreference {
    Cooler,
    Hotter,
};

static constexpr uint16_t sDistributionWeightBase = 1000;
static constexpr uint16_t sDistributionWeightBonus = 4000;
static constexpr float sThermalControlDeadbandCelsius = 0.0f;
static constexpr float sThermalWeightFullBiasSpreadCelsius = 20.0f;
static constexpr float sThermalRebalanceTargetConvergenceFractionPerMinute = 0.002578125f;
static constexpr float sThermalRebalanceWattsPerCelsiusPerMinute = 480.0f;
static constexpr uint16_t sThermalRebalanceMaxStepWatts = 60;
static constexpr uint32_t sThermalRebalanceMinIntervalMillis = 15000;
static constexpr uint32_t sDistributedChangeRepeatLockoutMillis = 2000;
static constexpr float sThermalDerivativeLeadMinutes = 6.0f;
static constexpr float sThermalDerivativeFilterTimeConstantMinutes = 1.0f;
static constexpr float sThermalDerivativeMaxCelsius = 4.0f;
static constexpr float sThermalSlopeChangeDampingMinutes = 24.0f;
static constexpr float sThermalSlopeChangeDampingMaxCelsius = 12.0f;
static constexpr uint16_t sSolarOutputLimitSafetyHeadroomWatts = 200;

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
    float slopeChangeCelsiusPerMinute = 0.0f;
    float slopeChangeDampingCelsius = 0.0f;
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
    float slopeChangeCelsiusPerMinute;
    float slopeChangeDampingCelsius;
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
    float slopeChangeCelsiusPerMinute = 0.0f;
    float slopeChangeDampingCelsius = 0.0f;
    float dError = 0.0f;
    bool derivativeValid = false;
};

static std::vector<ThermalTrendState> sThermalTrendStates;
static std::vector<ThermalDebugItem> sThermalDebugItems;
static String sThermalDebugSource;
static String sThermalDebugFilterExpression;
static uint32_t sThermalDebugMillis = 0;
static uint16_t sThermalDebugExchangeBudgetWatts = 0;
static uint16_t sThermalDebugEffectiveExchangeBudgetWatts = 0;
static uint32_t sThermalRebalanceLastExchangeMillis = 0;

static char const* thermalPreferenceName(ThermalPreference preference)
{
    return preference == ThermalPreference::Cooler ? "cooler" : "hotter";
}

static float thermalPreferenceDelta(ThermalProjection const& projection,
        ThermalPreference preference)
{
    float const score = preference == ThermalPreference::Cooler
        ? -projection.pdError
        : projection.pdError;
    return score > 0.0f ? score : 0.0f;
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
        projection.slopeChangeCelsiusPerMinute = iter->slopeChangeCelsiusPerMinute;
        projection.slopeChangeDampingCelsius = iter->slopeChangeDampingCelsius;
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
            projection.slopeChangeCelsiusPerMinute = iter->derivativeValid
                ? projection.slopeCelsiusPerMinute - iter->slopeCelsiusPerMinute
                : 0.0f;
            projection.slopeChangeDampingCelsius = std::clamp(
                    projection.slopeChangeCelsiusPerMinute
                        * sThermalSlopeChangeDampingMinutes,
                    -sThermalSlopeChangeDampingMaxCelsius,
                    sThermalSlopeChangeDampingMaxCelsius);
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
        iter->slopeChangeCelsiusPerMinute = projection.slopeChangeCelsiusPerMinute;
        iter->slopeChangeDampingCelsius = projection.slopeChangeDampingCelsius;
        iter->dError = projection.dError;
        iter->derivativeValid = projection.derivativeValid;
    }

    projection.pdError = projection.pError
        + projection.dError
        + projection.slopeChangeDampingCelsius;
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
        uint16_t exchangeBudgetWatts = 0,
        uint16_t effectiveExchangeBudgetWatts = 0)
{
    sThermalDebugSource = source.c_str();
    sThermalDebugFilterExpression = filterExpression.c_str();
    sThermalDebugMillis = millis();
    sThermalDebugExchangeBudgetWatts = exchangeBudgetWatts;
    sThermalDebugEffectiveExchangeBudgetWatts = effectiveExchangeBudgetWatts;
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
            projection.slopeChangeCelsiusPerMinute,
            projection.slopeChangeDampingCelsius,
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

    for (auto& projection : projections) {
        projection.preferredDelta = thermalPreferenceDelta(projection, preference);
        projection.effectiveDelta = thermalEffectiveDelta(projection.preferredDelta);
        projection.weight = thermalDistributionWeight(projection.preferredDelta);

        auto change = std::find_if(changes.begin(), changes.end(),
                [&projection](auto const& item) {
                    return item.inverter == projection.inverter;
                });
        if (change != changes.end()) {
            change->weight = projection.weight;
            change->thermalScore = projection.preferredDelta;
            change->fallbackThermalScore = preference == ThermalPreference::Cooler
                ? -projection.temperature
                : projection.temperature;
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
    root["slope_change_damping_min"] = sThermalSlopeChangeDampingMinutes;
    root["slope_change_damping_max_c"] = sThermalSlopeChangeDampingMaxCelsius;
    root["target_convergence_fraction_per_min"] =
        sThermalRebalanceTargetConvergenceFractionPerMinute;
    root["watts_per_c_per_min"] = sThermalRebalanceWattsPerCelsiusPerMinute;
    root["max_step_w"] = sThermalRebalanceMaxStepWatts;
    root["min_interval_ms"] = sThermalRebalanceMinIntervalMillis;
    root["exchange_budget_w"] = sThermalDebugExchangeBudgetWatts;
    root["effective_exchange_budget_w"] = sThermalDebugEffectiveExchangeBudgetWatts;
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
        inverter["slope_change_c_per_min"] = item.slopeChangeCelsiusPerMinute;
        inverter["slope_change_damping_c"] = item.slopeChangeDampingCelsius;
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

void PowerLimiterClass::recordTraceSample(Status status)
{
    _lastTraceStatus = status;
    persistRuntimeStateIfDue();

    auto now = millis();
    auto const bucket = now / 1000;

    TraceSample sample;
    sample.millis = now;
    sample.powerMeterValid = PowerMeter.isDataValid();
    sample.powerMeter = sample.powerMeterValid
        ? clampTraceWatts(PowerMeter.getPowerTotal())
        : 0;
    sample.target = clampTraceWatts(getTargetPowerConsumption());
    sample.storageTarget = clampTraceWatts(getStorageTargetPowerConsumption());
    sample.expectedMeter = _lastExpectedPowerMeterValueValid
        ? clampTraceWatts(_lastExpectedPowerMeterValue)
        : 0;
    sample.expectedMeterValid = _lastExpectedPowerMeterValueValid;
    auto const batteryStats = Battery.getStats();
    sample.batteryBmsPowerValid =
        Configuration.get().Battery.Enabled
        && batteryStats->isVoltageValid()
        && batteryStats->isCurrentValid();
    if (sample.batteryBmsPowerValid) {
        sample.batteryBmsPower = clampTraceWatts(
                batteryStats->getVoltage() * batteryStats->getChargeCurrent());
    }
    sample.chargerInput = GridCharger.getPowerLimiterCurrentInputPowerWatts();
    auto const oChargerTargetInput = GridCharger.getPowerLimiterTargetInputPowerWatts();
    sample.chargerTargetInputValid = oChargerTargetInput.has_value();
    sample.chargerTargetInput = oChargerTargetInput.value_or(0);
    sample.chargerMaxInput = GridCharger.getPowerLimiterMaxInputPowerWatts();
    sample.status = static_cast<uint8_t>(status);

    auto copyForecast = [](TraceForecastSample& target,
            Predictive::ControlForecast const& source) {
        target.valid = source.valid;
        if (!source.valid) { return; }

        target.virtualMeter = clampTraceWatts(source.virtualMeterWatts);
        target.gridMin = clampTraceWatts(source.gridMinWatts);
        target.gridNominal = clampTraceWatts(source.gridNominalWatts);
        target.gridMax = clampTraceWatts(source.gridMaxWatts);
        target.pending = source.pendingCount;
    };
    copyForecast(sample.predictiveStorage, _lastPredictiveStorageForecast);
    copyForecast(sample.predictiveGlobal, _lastPredictiveGlobalForecast);
    sample.predictiveStorageTarget = calcDynamicStorageTargetForecast(
            _lastPredictiveGlobalForecast,
            now);
    sample.predictiveChargerMayBeActive = _lastPredictiveChargerMayBeActive;
    sample.predictiveStorageOutputMayBeActive = _lastPredictiveStorageOutputMayBeActive;
    sample.loopRuntimeMicros = _lastLoopRuntimeMicros;
    if (_predictiveModelInitialized) {
        sample.predictiveLedgerSize = static_cast<uint16_t>(std::min<size_t>(
                _predictiveModel.ledger().size(),
                std::numeric_limits<uint16_t>::max()));
        sample.predictiveActiveRequests = static_cast<uint16_t>(std::min<size_t>(
                _predictiveModel.activeRequestCount(),
                std::numeric_limits<uint16_t>::max()));
    }

    auto accumulateProvider = [](TraceProviderSample& target,
            PowerLimiterInverter const& inverter) {
        target.output = static_cast<uint16_t>(std::min<uint32_t>(
                static_cast<uint32_t>(target.output) + inverter.getCurrentOutputAcWatts(),
                std::numeric_limits<uint16_t>::max()));
        target.expectedOutput = static_cast<uint16_t>(std::min<uint32_t>(
                static_cast<uint32_t>(target.expectedOutput) + inverter.getExpectedOutputAcWatts(),
                std::numeric_limits<uint16_t>::max()));
        target.limit = static_cast<uint16_t>(std::min<uint32_t>(
                static_cast<uint32_t>(target.limit) + inverter.getCurrentLimitWattsForTrace(),
                std::numeric_limits<uint16_t>::max()));
        target.expectedLimit = static_cast<uint16_t>(std::min<uint32_t>(
                static_cast<uint32_t>(target.expectedLimit) + inverter.getExpectedLimitWattsForTrace(),
                std::numeric_limits<uint16_t>::max()));
        if (inverter.hasPendingTargetForTrace()) { target.pending = 1; }
        if (inverter.isUsingAssumedOutputForTrace()) { target.assumed = 1; }
    };

    for (auto const& upInv : _inverters) {
        if (upInv->isBatteryPowered()) {
            accumulateProvider(sample.battery, *upInv);
        } else if (upInv->isSolarPowered()) {
            accumulateProvider(sample.solar, *upInv);
        } else if (upInv->isSmartBufferPowered()) {
            accumulateProvider(sample.smartBuffer, *upInv);
        }
    }

    std::lock_guard<std::mutex> lock(_traceMutex);
    if (_traceSamples == nullptr || _traceSampleCapacity == 0) { return; }

    if (_traceSampleCount > 0) {
        auto const lastIndex = (_traceSampleWrite + _traceSampleCapacity - 1) % _traceSampleCapacity;
        if ((_traceSamples[lastIndex].millis / 1000) == bucket) {
            _traceSamples[lastIndex] = sample;
            return;
        }
    }

    _traceSamples[_traceSampleWrite] = sample;
    _traceSampleWrite = (_traceSampleWrite + 1) % _traceSampleCapacity;
    _traceSampleCount = std::min(_traceSampleCount + 1, _traceSampleCapacity);
}

void PowerLimiterClass::traceSampleLoop()
{
    recordTraceSample(_lastTraceStatus);
}

void PowerLimiterClass::updateGridMedianFilter()
{
    if (!PowerMeter.isDataValid()) {
        resetGridMedianFilter();
        return;
    }

    auto const powerMeterUpdate = PowerMeter.getLastUpdate();
    if (_gridMedianFilterPowerMeterValueValid
            && powerMeterUpdate == _gridMedianFilterPowerMeterUpdate) {
        return;
    }

    auto const powerMeterValue = PowerMeter.getPowerTotal();
    _gridMedianFilterSamples[_gridMedianFilterWrite] = powerMeterValue;
    _gridMedianFilterWrite = (_gridMedianFilterWrite + 1) % _gridMedianFilterCapacity;
    if (_gridMedianFilterCount < _gridMedianFilterCapacity) {
        ++_gridMedianFilterCount;
    }
    _gridMedianFilterPowerMeterUpdate = powerMeterUpdate;
    _gridMedianFilterPowerMeterValue = _gridMedianFilterCount == _gridMedianFilterCapacity
        ? medianOfThree(
                _gridMedianFilterSamples[0],
                _gridMedianFilterSamples[1],
                _gridMedianFilterSamples[2])
        : powerMeterValue;
    _gridMedianFilterPowerMeterValueValid = true;
}

void PowerLimiterClass::resetGridMedianFilter()
{
    _gridMedianFilterCount = 0;
    _gridMedianFilterWrite = 0;
    _gridMedianFilterPowerMeterUpdate = 0;
    _gridMedianFilterPowerMeterValue = 0.0f;
    _gridMedianFilterPowerMeterValueValid = false;
}

float PowerLimiterClass::getRegulationPowerMeterTotal() const
{
    if (!PowerMeter.isDataValid()) { return 0.0f; }

    // Median filter disabled for now; use the current meter sample directly.
    // if (_gridMedianFilterPowerMeterValueValid
    //         && _gridMedianFilterPowerMeterUpdate == PowerMeter.getLastUpdate()) {
    //     return _gridMedianFilterPowerMeterValue;
    // }

    return PowerMeter.getPowerTotal();
}

uint32_t PowerLimiterClass::getRegulationPowerMeterUpdate() const
{
    if (!PowerMeter.isDataValid()) { return 0; }

    auto const powerMeterUpdate = PowerMeter.getLastUpdate();
    // if (powerMeterUpdate < _gridMedianFilterObservationLatencyMs) { return 0; }

    // return powerMeterUpdate - _gridMedianFilterObservationLatencyMs;
    return powerMeterUpdate;
}

void PowerLimiterClass::addTraceJson(
        JsonObject root,
        uint32_t offsetSeconds,
        bool compact,
        uint32_t requestedRangeSeconds,
        bool compactV2) const
{
    auto addProvider = [](JsonObject target, TraceProviderSample const& source) {
        target["out"] = source.output;
        target["exp"] = source.expectedOutput;
        target["lim"] = source.limit;
        target["elim"] = source.expectedLimit;
        target["pending"] = source.pending;
        target["assumed"] = source.assumed;
    };
    auto addForecast = [](JsonObject target, TraceForecastSample const& source) {
        target["valid"] = source.valid;
        if (!source.valid) { return; }

        target["virtual"] = source.virtualMeter;
        target["min"] = source.gridMin;
        target["nominal"] = source.gridNominal;
        target["max"] = source.gridMax;
        target["pending"] = source.pending;
    };
    auto addStorageTargetForecast = [](
            JsonObject target,
            TraceStorageTargetForecastSample const& source) {
        target["valid"] = source.valid;
        target["horizon_ms"] = source.horizonMillis;
        if (source.valid) {
            target["min"] = source.min;
            target["nominal"] = source.nominal;
            target["max"] = source.max;
        }
        if (source.dueValid) {
            target["due_min"] = source.dueMin;
            target["due_nominal"] = source.dueNominal;
            target["due_max"] = source.dueMax;
            target["due_age_ms"] = source.dueAgeMillis;
        }
        if (source.oracleValid) {
            target["oracle"] = source.oracle;
            target["oracle_age_ms"] = source.oracleAgeMillis;
        }
    };
    auto addCompactProvider = [](JsonArray target, TraceProviderSample const& source) {
        target.add(source.output);
        target.add(source.expectedOutput);
        target.add(source.limit);
        target.add(source.expectedLimit);
        target.add(source.pending);
        target.add(source.assumed);
    };
    auto addOptionalInt = [](JsonArray target, bool valid, int value) {
        if (valid) {
            target.add(value);
        } else {
            target.add(nullptr);
        }
    };

    auto const now = millis();
    auto const nowEpoch = currentEpochSeconds();
    auto const validNowEpoch = epochLooksValid(nowEpoch) ? nowEpoch : 0;
    std::lock_guard<std::mutex> lock(_traceMutex);
    if (_traceSamples == nullptr || _traceSampleCapacity == 0) { return; }
    auto const historySeconds = static_cast<uint32_t>(std::min<size_t>(
            _traceSampleCapacity,
            std::numeric_limits<uint32_t>::max()));
    auto const visibleWindowSeconds = std::min<uint32_t>(_traceSampleWindowSeconds, historySeconds);
    auto const responseRangeLimitSeconds = visibleWindowSeconds;
    auto const rangeSeconds = requestedRangeSeconds > 0
        ? std::min(requestedRangeSeconds, responseRangeLimitSeconds)
        : visibleWindowSeconds;
    auto const maxOffsetSeconds = historySeconds > rangeSeconds
        ? historySeconds - rangeSeconds
        : 0;
    offsetSeconds = requestedRangeSeconds > 0
        ? std::min(offsetSeconds, historySeconds)
        : std::min(offsetSeconds, maxOffsetSeconds);
    auto const rangeEndSeconds = std::min<uint32_t>(
            offsetSeconds + rangeSeconds,
            historySeconds);

    root["now_ms"] = now;
    root["now_ts"] = validNowEpoch;
    root["capacity"] = static_cast<uint32_t>(_traceSampleCapacity);
    root["history_s"] = historySeconds;
    root["window_s"] = visibleWindowSeconds;
    root["offset_s"] = offsetSeconds;
    root["range_from_age_s"] = offsetSeconds;
    root["range_to_age_s"] = rangeEndSeconds;
    root["psram"] = _traceSamplesInPsram;
    root["bytes"] = static_cast<uint32_t>(std::min<size_t>(
            _traceSampleCapacity * sizeof(TraceSample),
            std::numeric_limits<uint32_t>::max()));
    root["format"] = compact ? (compactV2 ? "compact-v2" : "compact-v1") : "object-v1";

    auto samples = root["samples"].to<JsonArray>();
    root["sample_count_total"] = static_cast<uint32_t>(_traceSampleCount);
    if (_traceSampleCount > 0) {
        auto const oldestIndex = (_traceSampleWrite + _traceSampleCapacity - _traceSampleCount)
            % _traceSampleCapacity;
        auto const oldestAgeSeconds = static_cast<uint32_t>(now - _traceSamples[oldestIndex].millis) / 1000;
        root["available_s"] = std::min(oldestAgeSeconds + 1, historySeconds);
    } else {
        root["available_s"] = 0;
    }

    for (size_t i = 0; i < _traceSampleCount; ++i) {
        auto const index = (_traceSampleWrite + _traceSampleCapacity - _traceSampleCount + i)
            % _traceSampleCapacity;
        auto const& source = _traceSamples[index];
        auto const ageMillis = static_cast<uint32_t>(now - source.millis);
        auto const ageSeconds = ageMillis / 1000;
        if (ageSeconds < offsetSeconds || ageSeconds >= rangeEndSeconds) { continue; }

        if (compact) {
            auto sample = samples.add<JsonArray>();
            sample.add(compactV2 ? ageMillis : source.millis);
            addOptionalInt(sample, source.powerMeterValid, source.powerMeter);
            sample.add(source.target);
            sample.add(source.storageTarget);
            addOptionalInt(sample, source.expectedMeterValid, source.expectedMeter);
            sample.add(source.status);
            sample.add(source.chargerInput);
            addOptionalInt(sample,
                    source.chargerTargetInputValid,
                    source.chargerTargetInput);
            sample.add(source.chargerMaxInput);
            addCompactProvider(sample, source.battery);
            addCompactProvider(sample, source.solar);
            addCompactProvider(sample, source.smartBuffer);
            addOptionalInt(sample, source.batteryBmsPowerValid,
                    source.batteryBmsPower);
            continue;
        }

        auto sample = samples.add<JsonObject>();
        sample["ms"] = source.millis;
        if (source.powerMeterValid) {
            sample["pm"] = source.powerMeter;
        } else {
            sample["pm"] = nullptr;
        }
        sample["target"] = source.target;
        sample["storage"] = source.storageTarget;
        if (source.expectedMeterValid) {
            sample["expected"] = source.expectedMeter;
        }
        sample["status"] = source.status;
        auto charger = sample["charger"].to<JsonObject>();
        charger["in"] = source.chargerInput;
        if (source.chargerTargetInputValid) {
            charger["target"] = source.chargerTargetInput;
            charger["exp"] = source.chargerTargetInput;
        }
        charger["max"] = source.chargerMaxInput;
        if (source.batteryBmsPowerValid) {
            sample["battery_bms"] = source.batteryBmsPower;
        }
        addProvider(sample["battery"].to<JsonObject>(), source.battery);
        addProvider(sample["solar"].to<JsonObject>(), source.solar);
        addProvider(sample["smart"].to<JsonObject>(), source.smartBuffer);
        auto predictive = sample["predictive"].to<JsonObject>();
        addForecast(predictive["storage"].to<JsonObject>(), source.predictiveStorage);
        addForecast(predictive["global"].to<JsonObject>(), source.predictiveGlobal);
        addStorageTargetForecast(
                predictive["storage_target"].to<JsonObject>(),
                source.predictiveStorageTarget);
        predictive["charger_active"] = source.predictiveChargerMayBeActive;
        predictive["storage_output_active"] = source.predictiveStorageOutputMayBeActive;
        predictive["storage_charger_overlap"] =
            source.predictiveChargerMayBeActive
            && source.predictiveStorageOutputMayBeActive;
        predictive["ledger"] = source.predictiveLedgerSize;
        predictive["active"] = source.predictiveActiveRequests;
        sample["loop_us"] = source.loopRuntimeMicros;
    }
}

void PowerLimiterClass::addPredictiveRequestHistoryJson(
        JsonObject root,
        uint32_t updatedFrom,
        uint32_t displayFrom,
        uint32_t to) const
{
    struct Section {
        uint8_t source = 0;
        uint64_t serial = 0;
        uint8_t inverterIndex = 0xff;
        uint8_t order = 0xff;
        bool enabled = true;
        String name;
    };

    auto sectionKey = [](uint8_t source, uint64_t serial) -> String {
        if (static_cast<PredictiveHistorySource>(source) == PredictiveHistorySource::GridCharger) {
            return String("grid_charger");
        }
        return String(predictiveHistorySourceText(source)) + ":" + serialToJsonString(serial);
    };

    auto sameRequest = [](PredictiveRequestHistoryRecord const& left,
            PredictiveRequestHistoryRecord const& right) {
        return left.seq == right.seq
            && left.createdMillis == right.createdMillis
            && left.serial == right.serial
            && left.source == right.source;
    };

    auto requestEndMillis = [](PredictiveRequestHistoryRecord const& record) {
        return std::max<uint32_t>(
                record.latestEffectMillis,
                std::max<uint32_t>(
                    record.ackMillis,
                    std::max<uint32_t>(record.sentMillis, record.updatedMillis)));
    };

    auto radioRequestEndMillis = [](HoymilesRadio::RequestHistoryRecord const& record) {
        return std::max<uint32_t>(
                record.updatedMillis,
                std::max<uint32_t>(record.sentMillis, record.queuedMillis));
    };

    auto addSection = [&](std::vector<Section>& sections, Section section) {
        auto const key = sectionKey(section.source, section.serial);
        auto const exists = std::any_of(sections.begin(), sections.end(),
                [&](auto const& existing) {
                    return sectionKey(existing.source, existing.serial) == key;
                });
        if (!exists) {
            sections.push_back(section);
        }
    };

    auto const nowMillis = millis();
    auto const nowEpoch = currentEpochSeconds();
    static constexpr size_t MaxPredictiveRequestJsonRecords = 160;
    static constexpr size_t MaxRadioRequestJsonRecords = 128;

    root["request_history_seconds"] = PredictiveRequestHistorySeconds;
    root["request_history_capacity"] = static_cast<uint32_t>(
            std::min<size_t>(_predictiveRequestHistoryCapacity, std::numeric_limits<uint32_t>::max()));
    root["request_history_psram"] = _predictiveRequestHistoryInPsram;
    root["request_history_bytes"] = static_cast<uint32_t>(std::min<size_t>(
            _predictiveRequestHistoryCapacity * sizeof(PredictiveRequestHistoryRecord),
            std::numeric_limits<uint32_t>::max()));
    root["request_since"] = updatedFrom;
    root["request_incremental"] = updatedFrom > displayFrom;
    root["request_predictive_records_limit"] = static_cast<uint32_t>(MaxPredictiveRequestJsonRecords);
    root["request_rf_records_limit"] = static_cast<uint32_t>(MaxRadioRequestJsonRecords);

    std::vector<PredictiveRequestHistoryRecord> latestRecords;
    latestRecords.reserve(MaxPredictiveRequestJsonRecords);
    uint32_t predictiveRecordsAvailable = 0;
    uint32_t predictiveRecordsDropped = 0;
    auto includePredictiveRecord = [&](PredictiveRequestHistoryRecord const& record) {
        if (record.seq == 0 || record.createdMillis == 0 || record.updatedMillis == 0) { return false; }
        if ((nowMillis - record.updatedMillis) > (PredictiveRequestHistorySeconds + 2) * 1000UL) { return false; }

        const auto updatedEpoch = epochSecondsFromMillis(record.updatedMillis, nowMillis, nowEpoch);
        if (updatedEpoch == 0 || updatedEpoch < updatedFrom || updatedEpoch > to) { return false; }

        const auto createdEpoch = epochSecondsFromMillis(record.createdMillis, nowMillis, nowEpoch);
        const auto endEpoch = epochSecondsFromMillis(requestEndMillis(record), nowMillis, nowEpoch);
        if (createdEpoch == 0 || createdEpoch > to || endEpoch < displayFrom) { return false; }
        return true;
    };
    {
        std::lock_guard<std::mutex> lock(_predictiveRequestHistoryMutex);
        if (_predictiveRequestHistory != nullptr && _predictiveRequestHistoryCapacity > 0) {
            for (size_t i = 0; i < _predictiveRequestHistoryCount; ++i) {
                auto const index =
                    (_predictiveRequestHistoryWrite + _predictiveRequestHistoryCapacity - 1 - i)
                    % _predictiveRequestHistoryCapacity;
                auto const& record = _predictiveRequestHistory[index];
                if (!includePredictiveRecord(record)) { continue; }

                auto iter = std::find_if(latestRecords.begin(), latestRecords.end(),
                        [&](auto const& existing) { return sameRequest(existing, record); });
                if (iter != latestRecords.end()) { continue; }

                ++predictiveRecordsAvailable;
                if (latestRecords.size() < MaxPredictiveRequestJsonRecords) {
                    latestRecords.push_back(record);
                } else {
                    ++predictiveRecordsDropped;
                }
            }
        }
    }
    root["request_predictive_records_available"] = predictiveRecordsAvailable;
    root["request_predictive_records_dropped"] = predictiveRecordsDropped;
    root["request_predictive_records_truncated"] = predictiveRecordsDropped > 0;

    std::vector<HoymilesRadio::RequestHistoryRecord> radioRecords;
    Hoymiles.getRadioRequestHistory(radioRecords);
    std::vector<HoymilesRadio::RequestHistoryRecord> latestRadioRecords;
    latestRadioRecords.reserve(MaxRadioRequestJsonRecords);
    uint32_t radioRecordsAvailable = 0;
    uint32_t radioRecordsDropped = 0;
    auto includeRadioRecord = [&](HoymilesRadio::RequestHistoryRecord const& record) {
        if (record.seq == 0 || record.queuedMillis == 0 || record.updatedMillis == 0) { return false; }
        if ((nowMillis - record.updatedMillis) > (PredictiveRequestHistorySeconds + 2) * 1000UL) { return false; }

        const auto updatedEpoch = epochSecondsFromMillis(record.updatedMillis, nowMillis, nowEpoch);
        if (updatedEpoch == 0 || updatedEpoch < updatedFrom || updatedEpoch > to) { return false; }

        const auto createdEpoch = epochSecondsFromMillis(record.queuedMillis, nowMillis, nowEpoch);
        const auto endEpoch = epochSecondsFromMillis(radioRequestEndMillis(record), nowMillis, nowEpoch);
        if (createdEpoch == 0 || createdEpoch > to || endEpoch < displayFrom) { return false; }
        return true;
    };
    for (auto iter = radioRecords.rbegin(); iter != radioRecords.rend(); ++iter) {
        if (!includeRadioRecord(*iter)) { continue; }
        ++radioRecordsAvailable;
        if (latestRadioRecords.size() < MaxRadioRequestJsonRecords) {
            latestRadioRecords.push_back(*iter);
        } else {
            ++radioRecordsDropped;
        }
    }
    root["request_rf_records_available"] = radioRecordsAvailable;
    root["request_rf_records_dropped"] = radioRecordsDropped;
    root["request_rf_records_truncated"] = radioRecordsDropped > 0;

    auto const& config = Configuration.get();
    std::vector<Section> sections;
    for (auto const& inverter : config.PowerLimiter.Inverters) {
        if (inverter.Serial == 0ULL) { break; }
        if (!inverter.IsGoverned) { continue; }

        Section section;
        section.source = predictiveSourceFromConfig(inverter.PowerSource);
        section.serial = inverter.Serial;
        section.inverterIndex = inverterIndexFromSerial(config, inverter.Serial);
        section.order = inverterOrderFromSerial(config, inverter.Serial);
        section.enabled = true;
        section.name = inverterNameFromSerial(config, inverter.Serial);
        addSection(sections, section);
    }
    if (config.GridCharger.Enabled && config.GridCharger.AutoPowerEnabled) {
        Section section;
        section.source = predictiveHistorySourceValue(PredictiveHistorySource::GridCharger);
        section.name = "Grid Charger";
        addSection(sections, section);
    }
    for (auto const& record : latestRecords) {
        Section section;
        section.source = record.source;
        section.serial = record.serial;
        section.inverterIndex = record.inverterIndex;
        section.order = record.serial == 0 ? 0xff : inverterOrderFromSerial(config, record.serial);
        section.enabled = true;
        section.name = record.serial == 0
            ? String("Grid Charger")
            : inverterNameFromSerial(config, record.serial);
        addSection(sections, section);
    }
    for (auto const& record : latestRadioRecords) {
        Section section;
        section.source = predictiveSourceFromPowerLimiterSerial(config, record.serial);
        section.serial = record.serial;
        section.inverterIndex = inverterIndexFromSerial(config, record.serial);
        section.order = inverterOrderFromSerial(config, record.serial);
        section.enabled = true;
        section.name = inverterNameFromSerial(config, record.serial);
        addSection(sections, section);
    }

    auto sourceOrder = [](uint8_t source) {
        switch (static_cast<PredictiveHistorySource>(source)) {
        case PredictiveHistorySource::Battery: return 0;
        case PredictiveHistorySource::Solar: return 1;
        case PredictiveHistorySource::SmartBuffer: return 2;
        case PredictiveHistorySource::GridCharger: return 3;
        }
        return 4;
    };
    std::sort(sections.begin(), sections.end(), [&](auto const& left, auto const& right) {
        auto const leftSourceOrder = sourceOrder(left.source);
        auto const rightSourceOrder = sourceOrder(right.source);
        if (leftSourceOrder != rightSourceOrder) { return leftSourceOrder < rightSourceOrder; }
        if (left.order != right.order) { return left.order < right.order; }
        if (left.inverterIndex != right.inverterIndex) { return left.inverterIndex < right.inverterIndex; }
        return left.serial < right.serial;
    });

    auto requestSections = root["request_sections"].to<JsonArray>();
    for (auto const& section : sections) {
        auto item = requestSections.add<JsonObject>();
        item["key"] = sectionKey(section.source, section.serial);
        item["source"] = predictiveHistorySourceText(section.source);
        item["serial"] = section.serial == 0 ? String() : serialToJsonString(section.serial);
        if (section.inverterIndex == 0xff) {
            item["inverter_index"] = nullptr;
        } else {
            item["inverter_index"] = section.inverterIndex;
        }
        item["order"] = section.order;
        item["enabled"] = section.enabled;
        item["name"] = section.name;
    }

    std::sort(latestRecords.begin(), latestRecords.end(),
            [](auto const& left, auto const& right) {
                if (left.createdMillis != right.createdMillis) {
                    return (left.createdMillis - right.createdMillis)
                        > (std::numeric_limits<uint32_t>::max() / 2);
                }
                return left.seq < right.seq;
            });
    std::sort(latestRadioRecords.begin(), latestRadioRecords.end(),
            [](auto const& left, auto const& right) {
                if (left.queuedMillis != right.queuedMillis) {
                    return (left.queuedMillis - right.queuedMillis)
                        > (std::numeric_limits<uint32_t>::max() / 2);
                }
                return left.seq < right.seq;
            });

    auto requests = root["requests"].to<JsonArray>();
    for (auto const& record : latestRecords) {
        auto item = requests.add<JsonObject>();
        auto const createdEpoch = epochSecondsFromMillis(record.createdMillis, nowMillis, nowEpoch);
        auto const updatedEpoch = epochSecondsFromMillis(record.updatedMillis, nowMillis, nowEpoch);
        item["key"] = String(record.seq) + ":"
            + predictiveHistorySourceText(record.source) + ":" + serialToJsonString(record.serial);
        item["seq"] = record.seq;
        item["section_key"] = sectionKey(record.source, record.serial);
        item["source"] = predictiveHistorySourceText(record.source);
        item["serial"] = record.serial == 0 ? String() : serialToJsonString(record.serial);
        if (record.inverterIndex == 0xff) {
            item["inverter_index"] = nullptr;
        } else {
            item["inverter_index"] = record.inverterIndex;
        }
        item["domain"] = predictiveRequestDomainText(static_cast<Predictive::RequestDomain>(record.domain));
        item["cause"] = predictiveRequestCauseText(static_cast<Predictive::RequestCause>(record.cause));
        item["status"] = predictiveRequestStateText(static_cast<Predictive::RequestState>(record.state));
        item["effect_kind"] = predictiveEffectKindText(static_cast<Predictive::EffectKind>(record.effectKind));
        item["transition"] = predictiveTransitionKindText(static_cast<Predictive::TransitionKind>(record.transitionKind));
        item["created"] = createdEpoch;
        item["updated"] = updatedEpoch;
        item["sent"] = epochSecondsFromMillis(record.sentMillis, nowMillis, nowEpoch);
        item["ack"] = epochSecondsFromMillis(record.ackMillis, nowMillis, nowEpoch);
        item["not_before_effect"] = epochSecondsFromMillis(record.notBeforeEffectMillis, nowMillis, nowEpoch);
        item["earliest_expected"] = epochSecondsFromMillis(record.earliestExpectedMillis, nowMillis, nowEpoch);
        item["typical_effect"] = epochSecondsFromMillis(record.typicalEffectMillis, nowMillis, nowEpoch);
        item["latest_effect"] = epochSecondsFromMillis(record.latestEffectMillis, nowMillis, nowEpoch);
        item["base_w"] = record.baseSetpointWatts;
        item["target_w"] = record.targetSetpointWatts;
        item["delta_w"] = record.effectiveDeltaWatts;
        item["capacity_full_output_possible"] = record.capacityFullOutputPossible;
        item["dispatch_pending"] = record.dispatchPending;
    }
    for (auto const& record : latestRadioRecords) {
        auto item = requests.add<JsonObject>();
        auto const source = predictiveSourceFromPowerLimiterSerial(config, record.serial);
        auto const createdEpoch = epochSecondsFromMillis(record.queuedMillis, nowMillis, nowEpoch);
        auto const updatedEpoch = epochSecondsFromMillis(record.updatedMillis, nowMillis, nowEpoch);
        item["key"] = String("rf:") + String(record.seq) + ":"
            + predictiveHistorySourceText(source) + ":" + serialToJsonString(record.serial);
        item["seq"] = record.seq;
        item["section_key"] = sectionKey(source, record.serial);
        item["source"] = predictiveHistorySourceText(source);
        item["serial"] = serialToJsonString(record.serial);
        auto const inverterIndex = inverterIndexFromSerial(config, record.serial);
        if (inverterIndex == 0xff) {
            item["inverter_index"] = nullptr;
        } else {
            item["inverter_index"] = inverterIndex;
        }
        item["domain"] = "rf";
        item["cause"] = record.retransmit ? "retransmit" : "normal";
        item["status"] = radioRequestStateText(record.state);
        item["effect_kind"] = "rf";
        item["transition"] = "rf";
        item["command"] = String(record.command);
        item["created"] = createdEpoch;
        item["updated"] = updatedEpoch;
        item["sent"] = epochSecondsFromMillis(record.sentMillis, nowMillis, nowEpoch);
        item["ack"] = 0;
        item["not_before_effect"] = 0;
        item["earliest_expected"] = 0;
        item["typical_effect"] = 0;
        item["latest_effect"] = 0;
        item["base_w"] = 0;
        item["target_w"] = 0;
        item["delta_w"] = 0;
        item["capacity_full_output_possible"] = true;
    }
}

static void traceCompactV2FormatOptionalInt(char* target, size_t size, bool valid, int value)
{
    if (valid) {
        snprintf(target, size, "%d", value);
    } else {
        snprintf(target, size, "null");
    }
}

static void traceCompactV2Queue(
        PowerLimiterClass::TraceCompactV2ChunkCursor& cursor,
        char const* data,
        size_t length)
{
    auto const copyLength = std::min<size_t>(
            length,
            PowerLimiterClass::TraceCompactV2ChunkCursor::PendingBufferSize - 1);
    if (data != cursor.pending) {
        memcpy(cursor.pending, data, copyLength);
    }
    cursor.pending[copyLength] = '\0';
    cursor.pendingOffset = 0;
    cursor.pendingLength = copyLength;
}

static bool traceCompactV2FlushPending(
        PowerLimiterClass::TraceCompactV2ChunkCursor& cursor,
        uint8_t* buffer,
        size_t maxLen,
        size_t& written)
{
    if (cursor.pendingOffset >= cursor.pendingLength) { return false; }

    auto const bytesToWrite = std::min(
            maxLen - written,
            cursor.pendingLength - cursor.pendingOffset);
    memcpy(buffer + written, cursor.pending + cursor.pendingOffset, bytesToWrite);
    cursor.pendingOffset += bytesToWrite;
    written += bytesToWrite;
    if (cursor.pendingOffset >= cursor.pendingLength) {
        cursor.pendingOffset = 0;
        cursor.pendingLength = 0;
    }
    return true;
}

static size_t traceCompactV2IndexAt(
        PowerLimiterClass::TraceCompactV2ChunkCursor const& cursor,
        size_t logicalIndex)
{
    auto index = cursor.firstIndex + logicalIndex;
    if (index >= cursor.capacitySnapshot) {
        index -= cursor.capacitySnapshot;
    }
    return index;
}

void PowerLimiterClass::writeTraceCompactV2Json(
        Print& output,
        uint32_t offsetSeconds,
        uint32_t requestedRangeSeconds) const
{
    TraceCompactV2ChunkCursor cursor;
    uint8_t buffer[512];
    while (true) {
        auto const written = writeTraceCompactV2JsonChunk(
                cursor,
                buffer,
                sizeof(buffer),
                offsetSeconds,
                requestedRangeSeconds);
        if (written == 0) { break; }
        output.write(buffer, written);
    }
}

size_t PowerLimiterClass::writeTraceCompactV2JsonChunk(
        TraceCompactV2ChunkCursor& cursor,
        uint8_t* buffer,
        size_t maxLen,
        uint32_t offsetSeconds,
        uint32_t requestedRangeSeconds) const
{
    if (buffer == nullptr || maxLen == 0) { return 0; }

    if (!cursor.initialized) {
        auto const now = millis();
        auto const nowEpoch = currentEpochSeconds();
        auto const validNowEpoch = epochLooksValid(nowEpoch) ? nowEpoch : 0;
        std::lock_guard<std::mutex> lock(_traceMutex);
        cursor.now = now;
        cursor.nowEpoch = validNowEpoch;
        cursor.capacitySnapshot = _traceSampleCapacity;
        cursor.capacity = static_cast<uint32_t>(std::min<size_t>(
                _traceSampleCapacity,
                std::numeric_limits<uint32_t>::max()));
        cursor.historySeconds = cursor.capacity;
        cursor.visibleWindowSeconds = std::min<uint32_t>(
                _traceSampleWindowSeconds,
                cursor.historySeconds);
        auto const rangeSeconds = requestedRangeSeconds > 0
            ? std::min(requestedRangeSeconds, cursor.visibleWindowSeconds)
            : cursor.visibleWindowSeconds;
        cursor.offsetSeconds = std::min(offsetSeconds, cursor.historySeconds);
        cursor.rangeEndSeconds = std::min<uint32_t>(
                cursor.offsetSeconds + rangeSeconds,
                cursor.historySeconds);
        cursor.samplesInPsram = _traceSamplesInPsram;
        cursor.traceBytes = static_cast<uint32_t>(std::min<size_t>(
                _traceSampleCapacity * sizeof(TraceSample),
                std::numeric_limits<uint32_t>::max()));
        cursor.sampleCount = _traceSamples == nullptr || _traceSampleCapacity == 0
            ? 0
            : _traceSampleCount;
        cursor.sampleCountTotal = static_cast<uint32_t>(std::min<size_t>(
                cursor.sampleCount,
                std::numeric_limits<uint32_t>::max()));
        if (cursor.sampleCount > 0) {
            cursor.firstIndex = (_traceSampleWrite + _traceSampleCapacity - _traceSampleCount)
                % _traceSampleCapacity;
            auto const oldestIndex = cursor.firstIndex;
            auto const oldestAgeSeconds =
                static_cast<uint32_t>(now - _traceSamples[oldestIndex].millis) / 1000;
            cursor.availableSeconds = std::min(oldestAgeSeconds + 1, cursor.historySeconds);

            auto ageSecondsAt = [this, &cursor](size_t logicalIndex) {
                auto const& source = _traceSamples[traceCompactV2IndexAt(cursor, logicalIndex)];
                return static_cast<uint32_t>(cursor.now - source.millis) / 1000;
            };

            size_t firstCandidate = 0;
            size_t searchEnd = cursor.sampleCount;
            while (firstCandidate < searchEnd) {
                auto const mid = firstCandidate + (searchEnd - firstCandidate) / 2;
                if (ageSecondsAt(mid) >= cursor.rangeEndSeconds) {
                    firstCandidate = mid + 1;
                } else {
                    searchEnd = mid;
                }
            }
            cursor.sampleIndex = firstCandidate;
        }
        cursor.initialized = true;
    }

    size_t written = 0;
    while (written < maxLen) {
        if (traceCompactV2FlushPending(cursor, buffer, maxLen, written)) {
            continue;
        }

        if (!cursor.headerWritten) {
            int const length = snprintf(cursor.pending, sizeof(cursor.pending),
                    "{\"now_ms\":%" PRIu32
                    ",\"now_ts\":%" PRIu32
                    ",\"capacity\":%" PRIu32
                    ",\"history_s\":%" PRIu32
                    ",\"window_s\":%" PRIu32
                    ",\"offset_s\":%" PRIu32
                    ",\"range_from_age_s\":%" PRIu32
                    ",\"range_to_age_s\":%" PRIu32
                    ",\"psram\":%s"
                    ",\"bytes\":%" PRIu32
                    ",\"format\":\"compact-v2\""
                    ",\"samples\":[",
                    cursor.now,
                    cursor.nowEpoch,
                    cursor.capacity,
                    cursor.historySeconds,
                    cursor.visibleWindowSeconds,
                    cursor.offsetSeconds,
                    cursor.offsetSeconds,
                    cursor.rangeEndSeconds,
                    cursor.samplesInPsram ? "true" : "false",
                    cursor.traceBytes);
            traceCompactV2Queue(cursor, cursor.pending, static_cast<size_t>(std::max(0, length)));
            cursor.headerWritten = true;
            continue;
        }

        if (cursor.sampleIndex < cursor.sampleCount) {
            TraceSample source;
            bool foundSample = false;
            {
                std::lock_guard<std::mutex> lock(_traceMutex);
                if (_traceSamples != nullptr
                        && _traceSampleCapacity == cursor.capacitySnapshot
                        && cursor.capacitySnapshot > 0) {
                    while (cursor.sampleIndex < cursor.sampleCount) {
                        source = _traceSamples[traceCompactV2IndexAt(cursor, cursor.sampleIndex)];
                        ++cursor.sampleIndex;

                        auto const ageMillis = static_cast<uint32_t>(cursor.now - source.millis);
                        auto const ageSeconds = ageMillis / 1000;
                        if (ageSeconds < cursor.offsetSeconds) {
                            cursor.sampleIndex = cursor.sampleCount;
                            break;
                        }
                        if (ageSeconds >= cursor.rangeEndSeconds) { continue; }

                        foundSample = true;
                        break;
                    }
                } else {
                    cursor.sampleIndex = cursor.sampleCount;
                }
            }
            if (!foundSample) { continue; }

            auto const ageMillis = static_cast<uint32_t>(cursor.now - source.millis);

            char expectedMeter[8];
            char powerMeter[8];
            char batteryBmsPower[8];
            char chargerTargetInput[8];
            char predictiveGlobalMin[8];
            char predictiveGlobalNominal[8];
            char predictiveGlobalMax[8];
            char predictiveStorageTargetMin[8];
            char predictiveStorageTargetNominal[8];
            char predictiveStorageTargetMax[8];
            char predictiveStorageTargetOracle[8];
            char predictiveStorageTargetDueMin[8];
            char predictiveStorageTargetDueNominal[8];
            char predictiveStorageTargetDueMax[8];
            char predictiveStorageTargetDueAge[8];
            traceCompactV2FormatOptionalInt(expectedMeter, sizeof(expectedMeter),
                    source.expectedMeterValid, source.expectedMeter);
            traceCompactV2FormatOptionalInt(powerMeter, sizeof(powerMeter),
                    source.powerMeterValid, source.powerMeter);
            traceCompactV2FormatOptionalInt(batteryBmsPower, sizeof(batteryBmsPower),
                    source.batteryBmsPowerValid, source.batteryBmsPower);
            traceCompactV2FormatOptionalInt(chargerTargetInput, sizeof(chargerTargetInput),
                    source.chargerTargetInputValid, source.chargerTargetInput);
            traceCompactV2FormatOptionalInt(predictiveGlobalMin, sizeof(predictiveGlobalMin),
                    source.predictiveGlobal.valid, source.predictiveGlobal.gridMin);
            traceCompactV2FormatOptionalInt(predictiveGlobalNominal, sizeof(predictiveGlobalNominal),
                    source.predictiveGlobal.valid, source.predictiveGlobal.gridNominal);
            traceCompactV2FormatOptionalInt(predictiveGlobalMax, sizeof(predictiveGlobalMax),
                    source.predictiveGlobal.valid, source.predictiveGlobal.gridMax);
            traceCompactV2FormatOptionalInt(predictiveStorageTargetMin,
                    sizeof(predictiveStorageTargetMin),
                    source.predictiveStorageTarget.valid,
                    source.predictiveStorageTarget.min);
            traceCompactV2FormatOptionalInt(predictiveStorageTargetNominal,
                    sizeof(predictiveStorageTargetNominal),
                    source.predictiveStorageTarget.valid,
                    source.predictiveStorageTarget.nominal);
            traceCompactV2FormatOptionalInt(predictiveStorageTargetMax,
                    sizeof(predictiveStorageTargetMax),
                    source.predictiveStorageTarget.valid,
                    source.predictiveStorageTarget.max);
            traceCompactV2FormatOptionalInt(predictiveStorageTargetOracle,
                    sizeof(predictiveStorageTargetOracle),
                    source.predictiveStorageTarget.oracleValid,
                    source.predictiveStorageTarget.oracle);
            traceCompactV2FormatOptionalInt(predictiveStorageTargetDueMin,
                    sizeof(predictiveStorageTargetDueMin),
                    source.predictiveStorageTarget.dueValid,
                    source.predictiveStorageTarget.dueMin);
            traceCompactV2FormatOptionalInt(predictiveStorageTargetDueNominal,
                    sizeof(predictiveStorageTargetDueNominal),
                    source.predictiveStorageTarget.dueValid,
                    source.predictiveStorageTarget.dueNominal);
            traceCompactV2FormatOptionalInt(predictiveStorageTargetDueMax,
                    sizeof(predictiveStorageTargetDueMax),
                    source.predictiveStorageTarget.dueValid,
                    source.predictiveStorageTarget.dueMax);
            traceCompactV2FormatOptionalInt(predictiveStorageTargetDueAge,
                    sizeof(predictiveStorageTargetDueAge),
                    source.predictiveStorageTarget.dueValid,
                    source.predictiveStorageTarget.dueAgeMillis);

            int const length = snprintf(cursor.pending, sizeof(cursor.pending),
                    "%s%" PRIu32 ",%s,%d,%d,%s,%u,"
                    "%u,%s,%u,"
                    "%u,%u,%u,%u,%u,%u,"
                    "%u,%u,%u,%u,%u,%u,"
                    "%u,%u,%u,%u,%u,%u,%s,%s,%s,%s,%s,%s,%s,%s,%u,%s,%s,%s,%s]",
                    cursor.firstSample ? "[" : ",[",
                    ageMillis,
                    powerMeter,
                    static_cast<int>(source.target),
                    static_cast<int>(source.storageTarget),
                    expectedMeter,
                    static_cast<unsigned>(source.status),
                    static_cast<unsigned>(source.chargerInput),
                    chargerTargetInput,
                    static_cast<unsigned>(source.chargerMaxInput),
                    static_cast<unsigned>(source.battery.output),
                    static_cast<unsigned>(source.battery.expectedOutput),
                    static_cast<unsigned>(source.battery.limit),
                    static_cast<unsigned>(source.battery.expectedLimit),
                    static_cast<unsigned>(source.battery.pending),
                    static_cast<unsigned>(source.battery.assumed),
                    static_cast<unsigned>(source.solar.output),
                    static_cast<unsigned>(source.solar.expectedOutput),
                    static_cast<unsigned>(source.solar.limit),
                    static_cast<unsigned>(source.solar.expectedLimit),
                    static_cast<unsigned>(source.solar.pending),
                    static_cast<unsigned>(source.solar.assumed),
                    static_cast<unsigned>(source.smartBuffer.output),
                    static_cast<unsigned>(source.smartBuffer.expectedOutput),
                    static_cast<unsigned>(source.smartBuffer.limit),
                    static_cast<unsigned>(source.smartBuffer.expectedLimit),
                    static_cast<unsigned>(source.smartBuffer.pending),
                    static_cast<unsigned>(source.smartBuffer.assumed),
                    batteryBmsPower,
                    predictiveGlobalMin,
                    predictiveGlobalNominal,
                    predictiveGlobalMax,
                    predictiveStorageTargetMin,
                    predictiveStorageTargetNominal,
                    predictiveStorageTargetMax,
                    predictiveStorageTargetOracle,
                    static_cast<unsigned>(source.predictiveStorageTarget.horizonMillis),
                    predictiveStorageTargetDueMin,
                    predictiveStorageTargetDueNominal,
                    predictiveStorageTargetDueMax,
                    predictiveStorageTargetDueAge);
            traceCompactV2Queue(cursor, cursor.pending, static_cast<size_t>(std::max(0, length)));
            cursor.firstSample = false;
            continue;
        }

        if (!cursor.footerWritten) {
            int const length = snprintf(cursor.pending, sizeof(cursor.pending),
                    "],\"sample_count_total\":%" PRIu32
                    ",\"available_s\":%" PRIu32 "}",
                    cursor.sampleCountTotal,
                    cursor.availableSeconds);
            traceCompactV2Queue(cursor, cursor.pending, static_cast<size_t>(std::max(0, length)));
            cursor.footerWritten = true;
            continue;
        }

        break;
    }

    return written;
}

static float thermalRebalancePairConvergenceDeficit(ThermalProjection const& warmer,
        ThermalProjection const& cooler)
{
    float const gap = warmer.temperature - cooler.temperature;
    float const effectiveGap = thermalEffectiveDelta(gap);
    if (effectiveGap <= 0.0f) { return 0.0f; }
    if (!warmer.derivativeValid || !cooler.derivativeValid) { return 0.0f; }

    float const actualGapSlope =
        warmer.slopeCelsiusPerMinute - cooler.slopeCelsiusPerMinute;
    float const targetGapSlope =
        -effectiveGap * sThermalRebalanceTargetConvergenceFractionPerMinute;
    return actualGapSlope - targetGapSlope;
}

static int16_t thermalRebalancePairExchange(float convergenceDeficit,
        uint16_t maxForwardWatts,
        uint16_t maxReverseWatts)
{
    if (maxForwardWatts == 0 && maxReverseWatts == 0) { return 0; }
    if (std::abs(convergenceDeficit) <= 0.001f) { return 0; }

    auto requested = static_cast<uint16_t>(std::round(
            std::abs(convergenceDeficit) * sThermalRebalanceWattsPerCelsiusPerMinute));
    requested = std::min<uint16_t>(requested, sThermalRebalanceMaxStepWatts);

    if (convergenceDeficit > 0.0f) {
        return static_cast<int16_t>(std::min<uint16_t>(requested, maxForwardWatts));
    }

    return static_cast<int16_t>(
            -static_cast<int16_t>(std::min<uint16_t>(requested, maxReverseWatts)));
}

static uint32_t sProductionLimitCapRoundRobinCursor = 0;
static uint32_t sThermalRebalanceReductionRoundRobinCursor = 0;
static uint32_t sThermalRebalanceIncreaseRoundRobinCursor = 0;
static uint32_t sBatteryStandbyTargetRoundRobinCursor = 0;
static uint32_t sSolarSparseLimitRoundRobinCursor = 0;
static uint32_t sBatterySparseLimitRoundRobinCursor = 0;
static uint32_t sThermalDistributionRoundRobinCursor = 0;
static uint64_t sThermalRebalanceLastReductionSerial = 0;
static uint64_t sThermalRebalanceLastIncreaseSerial = 0;
static uint64_t sBatteryStandbyTargetLastAdjustedSerial = 0;
static uint64_t sSolarSparseLastAdjustedSerial = 0;
static uint64_t sBatterySparseLastAdjustedSerial = 0;
static uint64_t sThermalDistributionLastAdjustedSerial = 0;
static uint32_t sThermalRebalanceLastReductionMillis = 0;
static uint32_t sThermalRebalanceLastIncreaseMillis = 0;
static uint32_t sBatteryStandbyTargetLastAdjustedMillis = 0;
static uint32_t sSolarSparseLastAdjustedMillis = 0;
static uint32_t sBatterySparseLastAdjustedMillis = 0;
static uint32_t sThermalDistributionLastAdjustedMillis = 0;

static uint64_t repeatLockedLastAdjustedSerial(uint64_t serial,
        uint32_t adjustedMillis,
        uint32_t now)
{
    if (serial == 0 || adjustedMillis == 0) { return 0; }

    bool const lockoutElapsed = millisAtOrAfter(
            now,
            adjustedMillis + sDistributedChangeRepeatLockoutMillis);
    return lockoutElapsed ? 0 : serial;
}

static void rotateDistributedChanges(std::vector<DistributedChange>& changes,
        uint32_t& cursor)
{
    if (changes.size() < 2) { return; }

    auto const offset = cursor++ % changes.size();
    if (offset > 0) {
        std::rotate(changes.begin(), changes.begin() + offset, changes.end());
    }
}

static void prioritizeDistributedChanges(std::vector<DistributedChange>& changes,
        uint32_t& cursor,
        uint64_t lastAdjustedSerial = 0)
{
    auto isThermallyPreferred = [](auto const& change) {
        return change.weight > sDistributionWeightBase;
    };
    auto thermalScoreComparable = [](float left, float right) {
        return left >= right - 0.001f;
    };
    bool const hasThermallyPreferredAlternative = lastAdjustedSerial != 0
        && std::any_of(changes.begin(), changes.end(),
                [lastAdjustedSerial,&isThermallyPreferred](auto const& change) {
                    return change.capacity > change.amount
                        && change.inverter->getSerial() != lastAdjustedSerial
                        && isThermallyPreferred(change);
                });
    auto const lastAdjustedChange = lastAdjustedSerial == 0
        ? changes.end()
        : std::find_if(changes.begin(), changes.end(),
                [lastAdjustedSerial](auto const& change) {
                    return change.inverter->getSerial() == lastAdjustedSerial;
                });
    bool const lastAdjustedIsThermallyPreferred = lastAdjustedChange != changes.end()
        && isThermallyPreferred(*lastAdjustedChange);
    float const lastAdjustedThermalScore = lastAdjustedChange != changes.end()
        ? lastAdjustedChange->thermalScore
        : 0.0f;
    float const lastAdjustedFallbackThermalScore = lastAdjustedChange != changes.end()
        ? lastAdjustedChange->fallbackThermalScore
        : 0.0f;
    bool const hasAnyThermallyPreferredCandidate = std::any_of(
            changes.begin(),
            changes.end(),
            [&isThermallyPreferred](auto const& change) {
                return change.capacity > change.amount
                    && isThermallyPreferred(change);
            });
    bool const hasComparableAlternative = lastAdjustedChange != changes.end()
        && std::any_of(changes.begin(), changes.end(),
                [
                    lastAdjustedSerial,
                    &isThermallyPreferred,
                    lastAdjustedIsThermallyPreferred,
                    lastAdjustedThermalScore,
                    lastAdjustedFallbackThermalScore,
                    hasAnyThermallyPreferredCandidate,
                    &thermalScoreComparable
                ](auto const& change) {
                    if (change.capacity <= change.amount) { return false; }
                    if (change.inverter->getSerial() == lastAdjustedSerial) {
                        return false;
                    }
                    if (lastAdjustedIsThermallyPreferred) {
                        return isThermallyPreferred(change);
                    }
                    if (isThermallyPreferred(change)) { return true; }
                    if (hasAnyThermallyPreferredCandidate) { return false; }
                    return thermalScoreComparable(
                            change.fallbackThermalScore,
                            lastAdjustedFallbackThermalScore);
                });
    auto lastAdjustedPriority = [
            hasComparableAlternative,
            hasThermallyPreferredAlternative,
            &isThermallyPreferred,
            lastAdjustedSerial](
            auto const& change) {
        if (change.inverter->getSerial() != lastAdjustedSerial) { return 0; }

        if (isThermallyPreferred(change)) {
            return hasThermallyPreferredAlternative ? 1 : 0;
        }

        return (!hasThermallyPreferredAlternative && hasComparableAlternative) ? 1 : 0;
    };
    auto thermalScoreEqual = [](float left, float right) {
        return std::abs(left - right) <= 0.001f;
    };

    std::stable_sort(changes.begin(), changes.end(),
            [
                &isThermallyPreferred,
                &lastAdjustedPriority,
                &thermalScoreEqual
            ](auto const& left, auto const& right) {
                bool const leftThermal = isThermallyPreferred(left);
                bool const rightThermal = isThermallyPreferred(right);
                if (leftThermal != rightThermal) {
                    return leftThermal;
                }
                auto const leftLastAdjusted = lastAdjustedPriority(left);
                auto const rightLastAdjusted = lastAdjustedPriority(right);
                if (leftLastAdjusted != rightLastAdjusted) {
                    return leftLastAdjusted < rightLastAdjusted;
                }
                if (left.weight != right.weight) {
                    return left.weight > right.weight;
                }
                if (leftThermal && !thermalScoreEqual(
                        left.thermalScore,
                        right.thermalScore)) {
                    return left.thermalScore > right.thermalScore;
                }
                if (!leftThermal && !thermalScoreEqual(
                        left.fallbackThermalScore,
                        right.fallbackThermalScore)) {
                    return left.fallbackThermalScore > right.fallbackThermalScore;
                }
                return left.capacity > right.capacity;
            });

    if (changes.size() < 2) { return; }

    auto const cursorValue = cursor++;
    size_t groupBegin = 0;
    while (groupBegin < changes.size()) {
        size_t groupEnd = groupBegin + 1;
        while (groupEnd < changes.size()
                && isThermallyPreferred(changes[groupEnd])
                    == isThermallyPreferred(changes[groupBegin])
                && changes[groupEnd].weight == changes[groupBegin].weight
                && thermalScoreEqual(
                    changes[groupEnd].thermalScore,
                    changes[groupBegin].thermalScore)
                && thermalScoreEqual(
                    changes[groupEnd].fallbackThermalScore,
                    changes[groupBegin].fallbackThermalScore)
                && lastAdjustedPriority(changes[groupEnd])
                    == lastAdjustedPriority(changes[groupBegin])) {
            ++groupEnd;
        }

        auto const groupSize = groupEnd - groupBegin;
        if (groupSize > 1) {
            auto const offset = cursorValue % groupSize;
            if (offset > 0) {
                std::rotate(
                        changes.begin() + groupBegin,
                        changes.begin() + groupBegin + offset,
                        changes.begin() + groupEnd);
            }
        }

        groupBegin = groupEnd;
    }
}

static uint16_t distributeGreedyByPriority(uint16_t requested,
        std::vector<DistributedChange>& changes)
{
    uint32_t totalCapacity = 0;
    for (auto const& change : changes) {
        totalCapacity += change.capacity;
    }

    uint32_t remaining = std::min<uint32_t>(requested, totalCapacity);
    uint32_t allocated = remaining;

    for (auto& change : changes) {
        if (remaining == 0) { break; }
        if (change.amount >= change.capacity) { continue; }

        auto const room = static_cast<uint16_t>(change.capacity - change.amount);
        auto const delta = static_cast<uint16_t>(
                std::min<uint32_t>(room, remaining));
        change.amount += delta;
        remaining -= delta;
    }

    return allocated - remaining;
}

static bool capProductionLimitedOutputLimits(
        std::vector<PowerLimiterInverter*> const& matchingInverters,
        uint16_t powerRequested,
        uint16_t minimumHeadroom,
        std::vector<PowerLimiterInverter*>* cappedInverters = nullptr)
{
    uint32_t expectedOutput = 0;
    std::vector<DistributedChange> headroom;

    for (auto const inverter : matchingInverters) {
        auto const output = inverter->getExpectedOutputAcWatts();
        expectedOutput += output;

        if (inverter->hasPendingTargetForTrace()
                || inverter->isUsingAssumedOutputForTrace()) {
            continue;
        }

        auto const maximum = inverter->getConfiguredMaxPowerWatts();
        if (maximum <= output) { continue; }

        headroom.push_back({
            inverter,
            static_cast<uint16_t>(maximum - output),
        });
    }

    if (headroom.empty()) { return false; }

    uint32_t const targetOutput = std::max<uint32_t>(
            powerRequested,
            expectedOutput + minimumHeadroom);
    uint16_t const targetHeadroom = static_cast<uint16_t>(std::min<uint32_t>(
            targetOutput > expectedOutput ? targetOutput - expectedOutput : 0,
            std::numeric_limits<uint16_t>::max()));

    rotateDistributedChanges(headroom, sProductionLimitCapRoundRobinCursor);
    distributeGreedyByPriority(targetHeadroom, headroom);

    bool updated = false;
    for (auto const& item : headroom) {
        if (item.amount == 0) { continue; }

        auto const outputLimit = static_cast<uint16_t>(std::min<uint32_t>(
                static_cast<uint32_t>(item.inverter->getExpectedOutputAcWatts())
                    + item.amount,
                std::numeric_limits<uint16_t>::max()));
        if (item.inverter->capOutputLimit(outputLimit)) {
            updated = true;
            if (cappedInverters != nullptr) {
                cappedInverters->push_back(item.inverter);
            }
        }
    }

    return updated;
}

static bool rebalanceThermally(std::vector<PowerLimiterInverter*>& matchingInverters,
        std::string const& filterExpression,
        uint16_t exchangeBudgetWatts,
        std::vector<PowerLimiterInverter*>* changedInverters = nullptr)
{
    float averageTemperature = 0.0f;
    auto projections = collectThermalProjections(matchingInverters, averageTemperature);
    if (projections.empty()) { return false; }

    std::vector<DistributedChange> reductions;
    std::vector<DistributedChange> increases;

    for (auto& projection : projections) {
        float const effectiveError = std::abs(projection.pdError);
        projection.preferredDelta = effectiveError;
        projection.effectiveDelta = thermalEffectiveDelta(effectiveError);
        projection.weight = thermalDistributionWeight(effectiveError);

        if (projection.pdError > 0.0f) {
            uint16_t maxReduction = projection.inverter->getMaxReductionWatts(false/*no standby*/);
            projection.capacity = maxReduction;
            if (maxReduction == 0) { continue; }

            reductions.push_back({
                projection.inverter,
                maxReduction,
                0,
                projection.weight,
            });
        } else if (projection.pdError < 0.0f) {
            uint16_t maxIncrease = projection.inverter->getMaxNonProbingIncreaseWatts();
            projection.capacity = maxIncrease;
            if (maxIncrease == 0) { continue; }

            increases.push_back({
                projection.inverter,
                maxIncrease,
                0,
                projection.weight,
            });
        }
    }

    auto const now = millis();
    bool const rateLimited = sThermalRebalanceLastExchangeMillis != 0
        && (now - sThermalRebalanceLastExchangeMillis) < sThermalRebalanceMinIntervalMillis;

    uint16_t const effectiveExchangeBudgetWatts = rateLimited
        ? 0
        : std::min<uint16_t>(exchangeBudgetWatts, sThermalRebalanceMaxStepWatts);

    if (rateLimited || effectiveExchangeBudgetWatts == 0
            || reductions.empty() || increases.empty()) {
        char const* action = "increase-needed";
        if (rateLimited) {
            action = "rate-limited";
        } else if (effectiveExchangeBudgetWatts == 0) {
            action = "no-exchange-budget";
        }

        publishThermalDebug(rateLimited ? "rebalance-rate-limited" : "rebalance-blocked",
                filterExpression, projections,
                action,
                exchangeBudgetWatts,
                effectiveExchangeBudgetWatts);
        return false;
    }

    auto const lockedLastReductionSerial = repeatLockedLastAdjustedSerial(
            sThermalRebalanceLastReductionSerial,
            sThermalRebalanceLastReductionMillis,
            now);
    auto const lockedLastIncreaseSerial = repeatLockedLastAdjustedSerial(
            sThermalRebalanceLastIncreaseSerial,
            sThermalRebalanceLastIncreaseMillis,
            now);

    prioritizeDistributedChanges(reductions,
            sThermalRebalanceReductionRoundRobinCursor,
            lockedLastReductionSerial);
    prioritizeDistributedChanges(increases,
            sThermalRebalanceIncreaseRoundRobinCursor,
            lockedLastIncreaseSerial);

    auto findProjection = [&projections](PowerLimiterInverter const* inverter)
            -> ThermalProjection const* {
        auto const iter = std::find_if(projections.begin(), projections.end(),
                [inverter](auto const& projection) {
                    return projection.inverter == inverter;
                });
        return iter == projections.end() ? nullptr : &(*iter);
    };

    PowerLimiterInverter* reducingInverter = nullptr;
    PowerLimiterInverter* increasingInverter = nullptr;
    uint16_t exchange = 0;
    bool forwardExchange = true;
    bool selectedUsesLastAdjusted = true;
    float selectedUsefulCorrection = 0.0f;
    float selectedPathDeviation = 0.0f;
    auto usesLastAdjustedPair = [
            lockedLastReductionSerial,
            lockedLastIncreaseSerial](
            PowerLimiterInverter const* reducing,
            PowerLimiterInverter const* increasing) {
        bool const repeatsReduction = lockedLastReductionSerial != 0
            && reducing->getSerial() == lockedLastReductionSerial;
        bool const repeatsIncrease = lockedLastIncreaseSerial != 0
            && increasing->getSerial() == lockedLastIncreaseSerial;
        return repeatsReduction || repeatsIncrease;
    };
    for (auto& warmerCandidate : reductions) {
        auto const* warmer = findProjection(warmerCandidate.inverter);
        if (warmer == nullptr) { continue; }

        for (auto& coolerCandidate : increases) {
            auto const* cooler = findProjection(coolerCandidate.inverter);
            if (cooler == nullptr) { continue; }

            auto const maxForwardExchange = static_cast<uint16_t>(
                    std::min<uint32_t>({
                        warmerCandidate.capacity,
                        coolerCandidate.capacity,
                        effectiveExchangeBudgetWatts,
                    }));
            auto const maxReverseExchange = static_cast<uint16_t>(
                    std::min<uint32_t>({
                        warmer->inverter->getMaxNonProbingIncreaseWatts(),
                        cooler->inverter->getMaxReductionWatts(false/*no standby*/),
                        effectiveExchangeBudgetWatts,
                    }));
            auto const convergenceDeficit = thermalRebalancePairConvergenceDeficit(
                    *warmer,
                    *cooler);
            auto const signedExchange = thermalRebalancePairExchange(
                    convergenceDeficit,
                    maxForwardExchange,
                    maxReverseExchange);
            if (signedExchange == 0) { continue; }
            auto const pathDeviation = std::abs(convergenceDeficit);
            auto const exchangeMagnitude = static_cast<uint16_t>(
                    signedExchange > 0
                        ? signedExchange
                        : -signedExchange);
            auto const usefulCorrection = std::min(
                    pathDeviation,
                    static_cast<float>(exchangeMagnitude)
                        / sThermalRebalanceWattsPerCelsiusPerMinute);
            PowerLimiterInverter* candidateReducingInverter = nullptr;
            PowerLimiterInverter* candidateIncreasingInverter = nullptr;
            if (signedExchange > 0) {
                candidateReducingInverter = warmer->inverter;
                candidateIncreasingInverter = cooler->inverter;
            } else {
                candidateReducingInverter = cooler->inverter;
                candidateIncreasingInverter = warmer->inverter;
            }
            bool const candidateUsesLastAdjusted = usesLastAdjustedPair(
                    candidateReducingInverter,
                    candidateIncreasingInverter);

            if (exchange != 0) {
                if (selectedUsesLastAdjusted && !candidateUsesLastAdjusted) {
                    // Prefer any viable non-repeating pair over immediately
                    // sending another thermal request to the same inverter.
                } else if (candidateUsesLastAdjusted != selectedUsesLastAdjusted) {
                    continue;
                } else if (usefulCorrection < selectedUsefulCorrection - 0.0001f) {
                    continue;
                } else if (std::abs(usefulCorrection - selectedUsefulCorrection) <= 0.0001f
                        && pathDeviation <= selectedPathDeviation) {
                    continue;
                }
            }

            reducingInverter = candidateReducingInverter;
            increasingInverter = candidateIncreasingInverter;
            exchange = exchangeMagnitude;
            forwardExchange = signedExchange > 0;
            selectedUsesLastAdjusted = candidateUsesLastAdjusted;
            selectedUsefulCorrection = usefulCorrection;
            selectedPathDeviation = pathDeviation;
        }
    }

    if (exchange == 0) {
        publishThermalDebug("rebalance-on-track", filterExpression, projections,
                "on-track", exchangeBudgetWatts, effectiveExchangeBudgetWatts);
        return false;
    }

    publishThermalDebug("rebalance", filterExpression, projections,
            forwardExchange ? "exchange" : "reverse-exchange",
            exchangeBudgetWatts, effectiveExchangeBudgetWatts);

    uint16_t appliedReduction = 0;
    appliedReduction += reducingInverter->applyReduction(
            exchange, false/*no standby*/);

    if (appliedReduction == 0) { return false; }
    sThermalRebalanceLastReductionSerial = reducingInverter->getSerial();
    sThermalRebalanceLastReductionMillis = now;
    if (changedInverters != nullptr
            && std::find(changedInverters->begin(), changedInverters->end(),
                    reducingInverter) == changedInverters->end()) {
        changedInverters->push_back(reducingInverter);
    }
    sThermalRebalanceLastExchangeMillis = now;

    auto const increaseAmount = static_cast<uint16_t>(
            std::min<uint32_t>(appliedReduction, exchange));

    uint16_t appliedIncrease = 0;
    appliedIncrease += increasingInverter->applyIncrease(increaseAmount);

    if (appliedIncrease == 0) { return true; }
    sThermalRebalanceLastIncreaseSerial = increasingInverter->getSerial();
    sThermalRebalanceLastIncreaseMillis = now;
    if (changedInverters != nullptr
            && std::find(changedInverters->begin(), changedInverters->end(),
                    increasingInverter) == changedInverters->end()) {
        changedInverters->push_back(increasingInverter);
    }

    DTU_LOGD("thermal rebalance for %s inverters: shifting %u W from "
            "1 %s inverter to 1 %s inverter around %.1f C",
            filterExpression.c_str(), appliedIncrease,
            forwardExchange ? "warmer" : "cooler",
            forwardExchange ? "cooler" : "warmer",
            averageTemperature);

    return true;
}

PowerLimiterClass PowerLimiter;

void PowerLimiterClass::initTraceBuffer()
{
    std::lock_guard<std::mutex> lock(_traceMutex);

    if (_traceSamplesInPsram) { return; }

    _traceSamples = _traceSampleFallback;
    _traceSampleCapacity = _traceSampleFallbackCapacity;
    _traceSampleWrite = 0;
    _traceSampleCount = 0;

    auto* samples = static_cast<TraceSample*>(heap_caps_calloc(
            _traceSampleHistorySeconds,
            sizeof(TraceSample),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (samples == nullptr) {
        DTU_LOGW("using internal trace fallback: %u samples, %u bytes",
                static_cast<unsigned>(_traceSampleFallbackCapacity),
                static_cast<unsigned>(_traceSampleFallbackCapacity * sizeof(TraceSample)));
        return;
    }

    _traceSamples = samples;
    _traceSampleCapacity = _traceSampleHistorySeconds;
    _traceSamplesInPsram = true;
    DTU_LOGI("using PSRAM trace buffer: %u samples, %u bytes",
            static_cast<unsigned>(_traceSampleCapacity),
            static_cast<unsigned>(_traceSampleCapacity * sizeof(TraceSample)));
}

void PowerLimiterClass::initPredictiveRequestHistoryBuffer()
{
    std::lock_guard<std::mutex> lock(_predictiveRequestHistoryMutex);

    if (_predictiveRequestHistoryInPsram) { return; }

    _predictiveRequestHistory = _predictiveRequestHistoryFallback;
    _predictiveRequestHistoryCapacity = _predictiveRequestHistoryFallbackCapacity;
    _predictiveRequestHistoryWrite = 0;
    _predictiveRequestHistoryCount = 0;

    auto* records = static_cast<PredictiveRequestHistoryRecord*>(heap_caps_calloc(
            _predictiveRequestHistoryDesiredCapacity,
            sizeof(PredictiveRequestHistoryRecord),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (records == nullptr) {
        DTU_LOGW("using internal predictive request fallback: %u records, %u bytes",
                static_cast<unsigned>(_predictiveRequestHistoryFallbackCapacity),
                static_cast<unsigned>(_predictiveRequestHistoryFallbackCapacity
                    * sizeof(PredictiveRequestHistoryRecord)));
        return;
    }

    _predictiveRequestHistory = records;
    _predictiveRequestHistoryCapacity = _predictiveRequestHistoryDesiredCapacity;
    _predictiveRequestHistoryInPsram = true;
    DTU_LOGI("using PSRAM predictive request buffer: %u records, %u bytes",
            static_cast<unsigned>(_predictiveRequestHistoryCapacity),
            static_cast<unsigned>(_predictiveRequestHistoryCapacity
                * sizeof(PredictiveRequestHistoryRecord)));
}

void PowerLimiterClass::init(Scheduler& scheduler)
{
    _batteryTargetPowerConsumption = Configuration.get().PowerLimiter.BatteryTargetPowerConsumption;
    initTraceBuffer();
    initPredictiveRequestHistoryBuffer();

    scheduler.addTask(_loopTask);
    _loopTask.setCallback(std::bind(&PowerLimiterClass::loop, this));
    _loopTask.setIterations(TASK_FOREVER);
    _loopTask.enable();

    scheduler.addTask(_traceSampleTask);
    _traceSampleTask.setCallback(std::bind(&PowerLimiterClass::traceSampleLoop, this));
    _traceSampleTask.setIterations(TASK_FOREVER);
    _traceSampleTask.setInterval(1 * TASK_SECOND);
    _traceSampleTask.enable();
}

void PowerLimiterClass::loadPersistedRuntimeState()
{
    if (_runtimeStateRestoreChecked) { return; }

    _runtimeStateRestoreChecked = true;
    _runtimeStateRestoreValid = false;
    _runtimeStateRestoreExpired = false;
    _runtimeStateRestoreApplied = false;
    _runtimeStateRestoreTimestamp = 0;
    _runtimeStateRestoreAgeSeconds = 0;
    _runtimeStateRestoredInverterCount = 0;
    _runtimeStateAppliedInverterCount = 0;
    _runtimeStateRestoreError = "";
    _restoredBatteryTargetPowerConsumption.reset();
    _restoredRuntimeStates.clear();

    if (!LittleFS.exists(RUNTIME_STATE_FILENAME)) {
        _runtimeStateRestoreError = "not_found";
        DTU_LOGD("no persisted power limiter state found");
        return;
    }

    File file = LittleFS.open(RUNTIME_STATE_FILENAME, "r", false);
    if (!file) {
        _runtimeStateRestoreError = "open_failed";
        DTU_LOGW("failed to open persisted power limiter state");
        return;
    }

    JsonDocument doc;
    auto const error = deserializeJson(doc, file);
    file.close();
    if (error) {
        _runtimeStateRestoreError = String("parse_failed: ") + error.c_str();
        DTU_LOGW("failed to read persisted power limiter state: %s", error.c_str());
        return;
    }

    if ((doc["version"] | 0) != 1) {
        _runtimeStateRestoreError = "unsupported_version";
        DTU_LOGW("persisted power limiter state has unsupported version");
        return;
    }

    auto const timestamp = doc["timestamp"].as<uint32_t>();
    auto const now = currentEpochSeconds();
    _runtimeStateRestoreTimestamp = timestamp;
    if (!epochLooksValid(now) || !epochLooksValid(timestamp) || timestamp > now) {
        _runtimeStateRestoreError = "invalid_timestamp";
        DTU_LOGW("persisted power limiter state has invalid timestamp");
        return;
    }

    _runtimeStateRestoreAgeSeconds = now - timestamp;
    if (_runtimeStateRestoreAgeSeconds > _runtimeStateRestoreMaxAgeSeconds) {
        _runtimeStateRestoreExpired = true;
        _runtimeStateRestoreError = "expired";
        DTU_LOGI("persisted power limiter state is %u s old, ignoring",
                _runtimeStateRestoreAgeSeconds);
        return;
    }

    int16_t restoredBatteryTarget = 0;
    if (readInt16(doc.as<JsonObjectConst>(), "battery_target_power_consumption",
                restoredBatteryTarget)) {
        _restoredBatteryTargetPowerConsumption = restoredBatteryTarget;
    }

    if (!doc["inverters"].is<JsonArray>()) {
        _runtimeStateRestoreError = "missing_inverters";
        DTU_LOGW("persisted power limiter state has no inverter array");
        return;
    }

    JsonArrayConst inverters = doc["inverters"].as<JsonArrayConst>();
    for (JsonObjectConst persisted : inverters) {
        auto const serial = serialFromJsonString(persisted["serial"] | "");
        if (serial == 0ULL) { continue; }

        PowerLimiterInverter::RuntimeState state;
        state.Serial = serial;
        if (!readUint16(persisted, "output_w", state.OutputAcWatts)) { continue; }
        if (!readUint16(persisted, "limit_w", state.PowerLimitWatts)) { continue; }
        if (!readUint16(persisted, "max_power_w", state.MaxPowerWatts)) { continue; }
        if (!readUint16(persisted, "expected_output_w", state.ExpectedOutputAcWatts)) {
            state.ExpectedOutputAcWatts = state.OutputAcWatts;
        }
        if (state.MaxPowerWatts == 0 || state.PowerLimitWatts == 0) { continue; }

        state.GridVoltage = persisted["grid_voltage"] | 0.0f;
        if (!std::isfinite(state.GridVoltage) || state.GridVoltage < 0.0f) {
            state.GridVoltage = 0.0f;
        }
        state.Reachable = persisted["reachable"] | false;
        state.Producing = persisted["producing"]
            | (state.OutputAcWatts > 0 || state.ExpectedOutputAcWatts > 0);

        _restoredRuntimeStates.push_back(state);
    }

    _runtimeStateRestoredInverterCount = _restoredRuntimeStates.size();
    if (_restoredRuntimeStates.empty()) {
        _runtimeStateRestoreError = "no_valid_inverters";
        DTU_LOGW("persisted power limiter state has no valid inverter entries");
        return;
    }

    _runtimeStateRestoreValid = true;
    DTU_LOGI("loaded persisted power limiter state: %u inverter%s, age %u s",
            _runtimeStateRestoredInverterCount,
            _runtimeStateRestoredInverterCount == 1 ? "" : "s",
            _runtimeStateRestoreAgeSeconds);
}

std::optional<PowerLimiterInverter::RuntimeState>
PowerLimiterClass::findRestoredRuntimeState(uint64_t serial) const
{
    for (auto const& state : _restoredRuntimeStates) {
        if (state.Serial == serial) { return state; }
    }

    return std::nullopt;
}

void PowerLimiterClass::applyRestoredRuntimeState(PowerLimiterInverter& inverter)
{
    auto state = findRestoredRuntimeState(inverter.getSerial());
    if (!state) { return; }

    inverter.restoreRuntimeState(*state, millis());
    ++_runtimeStateAppliedInverterCount;
}

void PowerLimiterClass::persistRuntimeStateIfDue()
{
    auto const now = millis();
    if (_lastRuntimeStatePersistCheckMillis != 0
            && (now - _lastRuntimeStatePersistCheckMillis) < _runtimeStatePersistIntervalMs) {
        return;
    }

    _lastRuntimeStatePersistCheckMillis = now;
    persistRuntimeState();
}

bool PowerLimiterClass::persistRuntimeState(bool force)
{
    auto const epoch = currentEpochSeconds();
    if (!epochLooksValid(epoch)) {
        _lastRuntimeStatePersistError = "invalid_time";
        return false;
    }

    std::vector<PowerLimiterInverter::RuntimeState> states;
    states.reserve(_inverters.size());
    for (auto const& inverter : _inverters) {
        if (!inverter->hasFreshRuntimeStateForPersistence()) { continue; }
        states.push_back(inverter->getRuntimeStateForPersistence());
    }

    if (states.empty()) {
        _lastRuntimeStatePersistError = "no_fresh_inverter_state";
        return false;
    }

    if (_runtimeStateRestoreApplied
            && _runtimeStateAppliedInverterCount > 0
            && states.size() < _runtimeStateAppliedInverterCount) {
        _lastRuntimeStatePersistError = "partial_fresh_inverter_state";
        return false;
    }

    int16_t const batteryTarget = clampToInt16(getBatteryTargetPowerConsumption());
    int16_t const storageTarget = clampToInt16(getStorageTargetPowerConsumption());
    int16_t const target = clampToInt16(getTargetPowerConsumption());

    uint32_t hash = 2166136261UL;
    hashValue(hash, static_cast<uint16_t>(batteryTarget));
    hashValue(hash, static_cast<uint16_t>(storageTarget));
    hashValue(hash, static_cast<uint16_t>(target));
    hashValue(hash, _lastExpectedInverterOutput);
    hashValue(hash, static_cast<uint32_t>(states.size()));
    for (auto const& state : states) {
        hashValue(hash, static_cast<uint32_t>((state.Serial >> 32) & 0xFFFFFFFF));
        hashValue(hash, static_cast<uint32_t>(state.Serial & 0xFFFFFFFF));
        hashValue(hash, state.OutputAcWatts);
        hashValue(hash, state.ExpectedOutputAcWatts);
        hashValue(hash, state.PowerLimitWatts);
        hashValue(hash, state.MaxPowerWatts);
        hashValue(hash, static_cast<uint32_t>(std::max(0.0f, state.GridVoltage) * 10.0f));
        hashValue(hash, state.Reachable ? 1 : 0);
        hashValue(hash, state.Producing ? 1 : 0);
    }

    auto const now = millis();
    bool const stateChanged = !_lastRuntimeStatePersistHashValid
        || hash != _lastRuntimeStatePersistHash;
    bool const refreshDue = !_lastRuntimeStatePersistSucceeded
        || (now - _lastRuntimeStatePersistMillis) >= _runtimeStateUnchangedRefreshIntervalMs;
    if (!force && !stateChanged && !refreshDue) { return true; }

    JsonDocument doc;
    doc["version"] = 1;
    doc["timestamp"] = epoch;
    doc["target_power_consumption"] = target;
    doc["storage_target_power_consumption"] = storageTarget;
    doc["battery_target_power_consumption"] = batteryTarget;
    doc["last_expected_inverter_output_w"] = _lastExpectedInverterOutput;

    JsonArray inverters = doc["inverters"].to<JsonArray>();
    for (auto const& state : states) {
        JsonObject target = inverters.add<JsonObject>();
        target["serial"] = serialToJsonString(state.Serial);
        target["output_w"] = state.OutputAcWatts;
        target["expected_output_w"] = state.ExpectedOutputAcWatts;
        target["limit_w"] = state.PowerLimitWatts;
        target["max_power_w"] = state.MaxPowerWatts;
        target["grid_voltage"] = state.GridVoltage;
        target["reachable"] = state.Reachable;
        target["producing"] = state.Producing;
    }

    File file = LittleFS.open(RUNTIME_STATE_TEMP_FILENAME, "w");
    if (!file) {
        _lastRuntimeStatePersistSucceeded = false;
        _lastRuntimeStatePersistError = "open_temp_failed";
        DTU_LOGW("failed to open temporary power limiter state for writing");
        return false;
    }

    if (serializeJson(doc, file) == 0) {
        file.close();
        LittleFS.remove(RUNTIME_STATE_TEMP_FILENAME);
        _lastRuntimeStatePersistSucceeded = false;
        _lastRuntimeStatePersistError = "serialize_failed";
        DTU_LOGW("failed to serialize power limiter state");
        return false;
    }
    file.close();

    if (LittleFS.exists(RUNTIME_STATE_FILENAME)) {
        LittleFS.remove(RUNTIME_STATE_FILENAME);
    }

    if (!LittleFS.rename(RUNTIME_STATE_TEMP_FILENAME, RUNTIME_STATE_FILENAME)) {
        LittleFS.remove(RUNTIME_STATE_TEMP_FILENAME);
        _lastRuntimeStatePersistSucceeded = false;
        _lastRuntimeStatePersistError = "rename_failed";
        DTU_LOGW("failed to save power limiter state");
        return false;
    }

    _lastRuntimeStatePersistCheckMillis = now;
    _lastRuntimeStatePersistMillis = now;
    _lastRuntimeStatePersistHash = hash;
    _lastRuntimeStatePersistHashValid = true;
    _lastRuntimeStatePersistSucceeded = true;
    _lastRuntimeStatePersistTimestamp = epoch;
    _lastRuntimeStatePersistInverterCount = states.size();
    _lastRuntimeStatePersistError = "";
    DTU_LOGD("persisted power limiter state: %u inverter%s%s",
            _lastRuntimeStatePersistInverterCount,
            _lastRuntimeStatePersistInverterCount == 1 ? "" : "s",
            stateChanged ? "" : " timestamp refresh");
    return true;
}

void PowerLimiterClass::addStatePersistenceJson(JsonObject root) const
{
    root["filename"] = RUNTIME_STATE_FILENAME;
    root["exists"] = LittleFS.exists(RUNTIME_STATE_FILENAME);
    root["restore_checked"] = _runtimeStateRestoreChecked;
    root["restore_valid"] = _runtimeStateRestoreValid;
    root["restore_expired"] = _runtimeStateRestoreExpired;
    root["restore_consumed"] = _runtimeStateRestoreConsumed;
    root["restore_applied"] = _runtimeStateRestoreApplied;
    root["restore_timestamp"] = _runtimeStateRestoreTimestamp;
    root["restore_age_s"] = _runtimeStateRestoreAgeSeconds;
    root["restore_inverters"] = _runtimeStateRestoredInverterCount;
    root["restore_applied_inverters"] = _runtimeStateAppliedInverterCount;
    root["restore_error"] = _runtimeStateRestoreError;
    root["last_write_success"] = _lastRuntimeStatePersistSucceeded;
    root["last_write_timestamp"] = _lastRuntimeStatePersistTimestamp;
    root["last_write_inverters"] = _lastRuntimeStatePersistInverterCount;
    root["last_write_error"] = _lastRuntimeStatePersistError;
    root["last_write_age_ms"] = _lastRuntimeStatePersistMillis == 0
        ? 0
        : millis() - _lastRuntimeStatePersistMillis;
    root["restore_max_age_s"] = _runtimeStateRestoreMaxAgeSeconds;
    root["persist_interval_ms"] = _runtimeStatePersistIntervalMs;
    root["unchanged_refresh_interval_ms"] = _runtimeStateUnchangedRefreshIntervalMs;

    if (!LittleFS.exists(RUNTIME_STATE_FILENAME)) { return; }

    File file = LittleFS.open(RUNTIME_STATE_FILENAME, "r", false);
    if (!file) {
        root["file_error"] = "open_failed";
        return;
    }
    root["file_size"] = static_cast<uint32_t>(file.size());

    JsonDocument doc;
    auto const error = deserializeJson(doc, file);
    file.close();
    if (error) {
        root["file_error"] = error.c_str();
        return;
    }

    auto const fileTimestamp = doc["timestamp"] | 0UL;
    auto const now = currentEpochSeconds();
    root["file_version"] = doc["version"] | 0;
    root["file_timestamp"] = fileTimestamp;
    if (epochLooksValid(now) && epochLooksValid(fileTimestamp) && fileTimestamp <= now) {
        auto const age = now - fileTimestamp;
        root["file_age_s"] = age;
        root["file_fresh"] = age <= _runtimeStateRestoreMaxAgeSeconds;
    }
    root["file_target_power_consumption"] = doc["target_power_consumption"] | 0;
    root["file_storage_target_power_consumption"] = doc["storage_target_power_consumption"] | 0;
    root["file_battery_target_power_consumption"] = doc["battery_target_power_consumption"] | 0;
    root["file_last_expected_inverter_output_w"] = doc["last_expected_inverter_output_w"] | 0;

    JsonArray persistedInverters = root["file_inverters"].to<JsonArray>();
    JsonArrayConst sourceInverters = doc["inverters"].as<JsonArrayConst>();
    for (JsonObjectConst source : sourceInverters) {
        JsonObject target = persistedInverters.add<JsonObject>();
        target["serial"] = source["serial"] | "";
        target["output_w"] = source["output_w"] | 0;
        target["expected_output_w"] = source["expected_output_w"] | 0;
        target["limit_w"] = source["limit_w"] | 0;
        target["max_power_w"] = source["max_power_w"] | 0;
        target["grid_voltage"] = source["grid_voltage"] | 0.0f;
        target["reachable"] = source["reachable"] | false;
        target["producing"] = source["producing"] | false;
    }
}

bool PowerLimiterClass::clearPersistedRuntimeState()
{
    bool success = true;
    if (LittleFS.exists(RUNTIME_STATE_FILENAME)) {
        success &= LittleFS.remove(RUNTIME_STATE_FILENAME);
    }
    if (LittleFS.exists(RUNTIME_STATE_TEMP_FILENAME)) {
        success &= LittleFS.remove(RUNTIME_STATE_TEMP_FILENAME);
    }

    if (!success) { return false; }

    _runtimeStateRestoreChecked = true;
    _runtimeStateRestoreValid = false;
    _runtimeStateRestoreExpired = false;
    _runtimeStateRestoreConsumed = true;
    _runtimeStateRestoreApplied = false;
    _runtimeStateRestoreTimestamp = 0;
    _runtimeStateRestoreAgeSeconds = 0;
    _runtimeStateRestoredInverterCount = 0;
    _runtimeStateAppliedInverterCount = 0;
    _runtimeStateRestoreError = "cleared";
    _restoredBatteryTargetPowerConsumption.reset();
    _restoredRuntimeStates.clear();
    _lastRuntimeStatePersistHashValid = false;
    _lastRuntimeStatePersistSucceeded = false;
    _lastRuntimeStatePersistTimestamp = 0;
    _lastRuntimeStatePersistInverterCount = 0;
    _lastRuntimeStatePersistError = "cleared";
    return true;
}

frozen::string const& PowerLimiterClass::getStatusText(PowerLimiterClass::Status status) const
{
    static const frozen::string missing = "programmer error: missing status text";

    static const frozen::map<Status, frozen::string, 12> texts = {
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
        { Status::EmergencyFullOutput, "emergency grid consumption: stopping charger and maximizing inverter output" },
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
    loadPersistedRuntimeState();
    bool const applyStartupRestore = _runtimeStateRestoreValid
        && !_runtimeStateRestoreConsumed;

    resetRegulationState();

    if (!config.PowerLimiter.Enabled || Mode::Disabled == _mode) {
        _runtimeStateRestoreConsumed = true;
        resetPredictiveControlState();
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
        if (upInv) {
            if (applyStartupRestore) {
                applyRestoredRuntimeState(*upInv);
            }
            _inverters.push_back(std::move(upInv));
        }
    }

    _runtimeStateRestoreConsumed = true;
    _runtimeStateRestoreApplied = _runtimeStateAppliedInverterCount > 0;

    calcNextInverterRestart();
    resetDynamicBatteryTargetState();
    resetBatteryDischargeCurrentLimitBudget();
    resetRegulationState();
    resetPredictiveControlState();
    _batteryTargetPowerConsumption = config.PowerLimiter.BatteryTargetPowerConsumption;
    if (applyStartupRestore
            && config.PowerLimiter.BatteryTargetPowerConsumptionDynamicEnabled
            && _restoredBatteryTargetPowerConsumption) {
        _batteryTargetPowerConsumption = *_restoredBatteryTargetPowerConsumption;
    }
    _lastExpectedInverterOutput = 0;
    for (auto const& upInv : _inverters) {
        _lastExpectedInverterOutput = static_cast<uint16_t>(std::min<uint32_t>(
                static_cast<uint32_t>(_lastExpectedInverterOutput)
                    + upInv->getExpectedOutputAcWatts(),
                std::numeric_limits<uint16_t>::max()));
    }

    _reloadConfigFlag = false;
}

void PowerLimiterClass::rememberRegulationOutcome(
        float expectedPowerMeterValue,
        bool expectedPowerMeterValueValid)
{
    if (!PowerMeter.isDataValid()) {
        resetRegulationState();
        return;
    }

    _lastRegulationPowerMeterUpdate = PowerMeter.getLastUpdate();
    _lastRegulationPowerMeterValue = getRegulationPowerMeterTotal();
    if (!expectedPowerMeterValueValid) {
        _lastExpectedPowerMeterValue = 0.0f;
        _lastExpectedPowerMeterValueValid = false;
        return;
    }

    _lastExpectedPowerMeterValue = expectedPowerMeterValue;
    _lastExpectedPowerMeterValueValid = true;
}

void PowerLimiterClass::resetRegulationState()
{
    _lastRegulationPowerMeterUpdate = 0;
    _lastRegulationPowerMeterValue = 0.0f;
    _lastExpectedPowerMeterValue = 0.0f;
    _lastExpectedPowerMeterValueValid = false;
    resetGridMedianFilter();
    _lastPlannerRunMillis = 0;
    _lastPlannerPowerMeterUpdate = 0;
    _lastPlannerPowerMeterValue = 0.0f;
    _lastPlannerStorageTargetPowerConsumption = 0.0f;
    _lastPlannerTargetPowerConsumption = 0.0f;
    _lastPlannerGridErrorUpdateMillis = 0;
    _plannerGridOutsideErrorIntegralWattMillis = 0.0f;
    _lastPlannerTargetBandErrorUpdateMillis = 0;
    _plannerTargetBandErrorIntegralWattMillis = 0.0f;
}

void PowerLimiterClass::recordLoopRuntime(uint32_t startedMicros)
{
    auto const elapsed = static_cast<uint32_t>(micros() - startedMicros);
    _lastLoopRuntimeMicros = elapsed;
    _maxLoopRuntimeMicros = std::max(_maxLoopRuntimeMicros, elapsed);
}

void PowerLimiterClass::recordGridChargerApplyRuntime(uint32_t startedMicros)
{
    auto const elapsed = static_cast<uint32_t>(micros() - startedMicros);
    _lastGridChargerApplyRuntimeMicros = elapsed;
    _maxGridChargerApplyRuntimeMicros = std::max(
            _maxGridChargerApplyRuntimeMicros,
            elapsed);
}

void PowerLimiterClass::addRuntimeDebugJson(JsonObject root) const
{
    root["last_loop_us"] = _lastLoopRuntimeMicros;
    root["max_loop_us"] = _maxLoopRuntimeMicros;
    root["last_charger_apply_us"] = _lastGridChargerApplyRuntimeMicros;
    root["max_charger_apply_us"] = _maxGridChargerApplyRuntimeMicros;
    root["inverter_rf_queue_size"] = getMaxInverterRadioQueueSize();
    root["inverter_rf_queue_congestion_threshold"] =
        sInverterRfQueueCongestionThreshold;
    root["inverter_rf_queue_congested"] = isInverterRadioQueueCongested();
    auto const& powerLimiterConfig = Configuration.get().PowerLimiter;
    root["predictive_initialized"] = _predictiveModelInitialized;
    if (_predictiveModelInitialized) {
        root["predictive_ledger_size"] = static_cast<uint32_t>(
                _predictiveModel.ledger().size());
        root["predictive_active_requests"] = static_cast<uint32_t>(
                _predictiveModel.activeRequestCount());
    }
    root["adaptive_planner_grid_change_threshold_w"] =
        powerLimiterConfig.TargetPowerConsumptionHysteresis;
    root["small_correction_damping_threshold_w"] =
        powerLimiterConfig.SmallCorrectionDampingThreshold;
    root["adaptive_planner_target_change_threshold_w"] =
        powerLimiterConfig.AdaptivePlannerTargetChangeThreshold;
    root["adaptive_planner_grid_error_integral_wms"] =
        _plannerGridOutsideErrorIntegralWattMillis;
    root["adaptive_planner_grid_error_integral_threshold_wms"] =
        wattSecondsToWattMillis(
                powerLimiterConfig.TargetPowerConsumptionCorridorErrorThresholdWs);
    root["adaptive_planner_target_band_error_integral_wms"] =
        _plannerTargetBandErrorIntegralWattMillis;
    root["adaptive_planner_target_band_error_threshold_wms"] =
        wattSecondsToWattMillis(
                powerLimiterConfig.TargetPowerConsumptionBandErrorThresholdWs);
    auto const nowMillis = millis();
    root["adaptive_planner_early_blocked_ms"] = 0;
    root["adaptive_planner_grid_early_blocked_ms"] = 0;
    root["adaptive_planner_min_outside_uncertainty_ratio"] =
        sAdaptivePlannerMinOutsideUncertaintyRatio;
    root["adaptive_planner_max_interval_ms"] =
        adaptivePlannerMaxIntervalMillis(powerLimiterConfig);
    root["adaptive_planner_last_run_age_ms"] =
        _lastPlannerRunMillis == 0 ? 0 : nowMillis - _lastPlannerRunMillis;
    root["battery_discharge_current_estimate_a"] =
        _lastBatteryDischargeCurrentEstimateAmps;
    root["battery_discharge_current_limit_a"] =
        _lastBatteryDischargeCurrentLimitAmps;
    root["battery_discharge_current_limit_budget_as"] =
        _batteryDischargeCurrentLimitBudgetAmpSeconds;
    root["battery_discharge_current_limit_budget_capacity_as"] =
        _batteryDischargeCurrentLimitBudgetCapacityAmpSeconds;
    root["battery_discharge_current_limit_budget_used_permille"] =
        _lastBatteryDischargeCurrentLimitBudgetUsedPermille;
    if (_lastPlannerPowerMeterUpdate != 0) {
        auto const storageTarget = getStorageTargetPowerConsumption();
        auto const target = Predictive::validateTargetOrdering(
                storageTarget,
                getTargetPowerConsumption(),
                true/*clamp*/);
        root["adaptive_planner_target_deviation_w"] =
            Predictive::targetChangeDistanceWatts(
                    storageTarget,
                    target,
                    _lastPlannerStorageTargetPowerConsumption,
                    _lastPlannerTargetPowerConsumption);
        root["adaptive_planner_last_storage_target_w"] =
            _lastPlannerStorageTargetPowerConsumption;
        root["adaptive_planner_last_target_w"] =
            _lastPlannerTargetPowerConsumption;
    }
    auto const dynamicTargetForecast =
        calcDynamicStorageTargetForecast(_lastPredictiveGlobalForecast, nowMillis);
    root["dynamic_storage_target_forecast_horizon_ms"] =
        dynamicTargetForecast.horizonMillis;
    if (dynamicTargetForecast.valid) {
        root["dynamic_storage_target_forecast_min_w"] =
            dynamicTargetForecast.min;
        root["dynamic_storage_target_forecast_nominal_w"] =
            dynamicTargetForecast.nominal;
        root["dynamic_storage_target_forecast_max_w"] =
            dynamicTargetForecast.max;
    }
    if (dynamicTargetForecast.dueValid) {
        root["dynamic_storage_target_due_forecast_min_w"] =
            dynamicTargetForecast.dueMin;
        root["dynamic_storage_target_due_forecast_nominal_w"] =
            dynamicTargetForecast.dueNominal;
        root["dynamic_storage_target_due_forecast_max_w"] =
            dynamicTargetForecast.dueMax;
        root["dynamic_storage_target_due_forecast_age_ms"] =
            dynamicTargetForecast.dueAgeMillis;
    }
    if (dynamicTargetForecast.oracleValid) {
        root["dynamic_storage_target_oracle_w"] =
            dynamicTargetForecast.oracle;
        root["dynamic_storage_target_oracle_age_ms"] =
            dynamicTargetForecast.oracleAgeMillis;
    }
    if (PowerMeter.isDataValid()) {
        root["grid_median_filter_raw_w"] = PowerMeter.getPowerTotal();
        root["grid_median_filter_w"] = getRegulationPowerMeterTotal();
        root["grid_median_filter_samples"] = _gridMedianFilterCount;
        root["grid_median_filter_observation_latency_ms"] =
            _gridMedianFilterObservationLatencyMs;
        root["grid_median_filter_observation_age_ms"] =
            nowMillis - getRegulationPowerMeterUpdate();
        if (_lastPlannerPowerMeterUpdate != 0) {
            auto const estimate = getPlannerGridEstimate(getRegulationPowerMeterUpdate());
            if (estimate.valid) {
                auto const meterValue = getRegulationPowerMeterTotal();
                auto const deviation =
                    Predictive::distanceOutsideCorridor(meterValue, estimate);
                auto const uncertainty =
                    Predictive::corridorUncertaintyWatts(estimate);
                auto const ratio =
                    Predictive::outsideCorridorUncertaintyRatio(deviation, uncertainty);
                root["adaptive_planner_grid_estimate_min_w"] = estimate.gridMinWatts;
                root["adaptive_planner_grid_estimate_max_w"] = estimate.gridMaxWatts;
                root["adaptive_planner_grid_estimate_uncertainty_w"] = uncertainty;
                root["adaptive_planner_grid_estimate_deviation_w"] = deviation;
                root["adaptive_planner_grid_estimate_deviation_ratio"] =
                    std::isfinite(ratio) ? ratio : 9999.0f;
            }
        }
        if (_lastPredictiveGlobalForecast.valid) {
            root["adaptive_planner_target_band_error_w"] =
                Predictive::targetBandErrorWatts(
                        getStorageTargetPowerConsumption(),
                        getTargetPowerConsumption(),
                        getRegulationPowerMeterTotal(),
                        _lastPredictiveGlobalForecast);
        }
    }
}

void PowerLimiterClass::resetPredictiveControlState()
{
    _predictiveModel = Predictive::Model();
    _predictiveModelInitialized = false;
    _lastPredictiveStorageForecast = Predictive::ControlForecast();
    _lastPredictiveGlobalForecast = Predictive::ControlForecast();
    _lastPredictiveChargerMayBeActive = false;
    _lastPredictiveStorageOutputMayBeActive = false;
}

void PowerLimiterClass::syncPredictiveActuators(bool gridChargerManaged)
{
    if (!_predictiveModelInitialized) {
        _predictiveModel = Predictive::Model();
        _predictiveModelInitialized = true;
    }

    auto actuatorHasOpenRequest = [this](std::string const& actuatorKey) {
        auto const& ledger = _predictiveModel.ledger();
        return std::any_of(ledger.begin(), ledger.end(),
                [&actuatorKey](auto const& request) {
                    return request.actuatorKey == actuatorKey
                        && predictiveRequestKeepsSetpointPending(request);
                });
    };

    auto syncActuator = [this,&actuatorHasOpenRequest](
            Predictive::ActuatorState actuator,
            float currentSetpointWatts) {
        auto* stored = _predictiveModel.findActuator(actuator.actuatorKey);
        if (stored == nullptr) {
            actuator.safeSetpointWatts = currentSetpointWatts;
            actuator.requestedSetpointWatts = currentSetpointWatts;
            actuator.normalize();
            _predictiveModel.addActuator(actuator);
            return;
        }

        auto const hasOpenRequest = actuatorHasOpenRequest(actuator.actuatorKey);
        stored->kind = actuator.kind;
        stored->capabilities = actuator.capabilities;
        stored->measuredOutputWatts = actuator.measuredOutputWatts;
        if (!hasOpenRequest) {
            stored->safeSetpointWatts = currentSetpointWatts;
            stored->requestedSetpointWatts = currentSetpointWatts;
        }
        stored->normalize();
    };

    for (auto const& upInv : _inverters) {
        Predictive::ActuatorState actuator;
        actuator.actuatorKey = predictiveInverterKey(*upInv);
        if (upInv->isBatteryPowered()) {
            actuator.kind = Predictive::ActuatorKind::Battery;
        } else if (upInv->isSmartBufferPowered()) {
            actuator.kind = Predictive::ActuatorKind::SmartBuffer;
        } else {
            actuator.kind = Predictive::ActuatorKind::Solar;
        }
        actuator.measuredOutputWatts = upInv->getCurrentOutputAcWatts();
        actuator.capabilities.maxSetpointWatts = upInv->getConfiguredMaxPowerWatts();
        syncActuator(actuator, upInv->getExpectedOutputAcWatts());
    }

    if (gridChargerManaged) {
        Predictive::ActuatorState charger;
        charger.actuatorKey = sPredictiveGridChargerKey;
        charger.kind = Predictive::ActuatorKind::Charger;
        charger.measuredOutputWatts = GridCharger.getPowerLimiterCurrentInputPowerWatts();
        auto const currentSetpoint = GridCharger.getPowerLimiterExpectedInputPowerWatts();
        charger.capabilities.maxSetpointWatts = std::max<uint16_t>(
                GridCharger.getPowerLimiterMaxInputPowerWatts(),
                currentSetpoint);
        syncActuator(charger, currentSetpoint);
    }
}

void PowerLimiterClass::advancePredictiveLedger()
{
    if (!_predictiveModelInitialized || !PowerMeter.isDataValid()) { return; }

    auto const before = predictiveRequestStates();
    _predictiveModel.advanceLedger(getRegulationPowerMeterUpdate());
    _predictiveModel.discardStaleQueuedRequests(millis(), PredictiveQueuedRequestStaleMillis);
    recordPredictiveRequestHistoryChanges(before);
    _predictiveModel.pruneInactiveRequests();
}

Predictive::MeterSnapshot PowerLimiterClass::makePredictiveMeterSnapshot() const
{
    Predictive::MeterSnapshot snapshot;
    snapshot.physicalGridWatts = PowerMeter.isDataValid() ? getRegulationPowerMeterTotal() : 0.0f;
    snapshot.meterSampleMillis = PowerMeter.isDataValid() ? getRegulationPowerMeterUpdate() : millis();
    return snapshot;
}

void PowerLimiterClass::updatePredictiveForecasts()
{
    if (!_predictiveModelInitialized || !PowerMeter.isDataValid()) {
        _lastPredictiveStorageForecast = Predictive::ControlForecast();
        _lastPredictiveGlobalForecast = Predictive::ControlForecast();
        _lastPredictiveChargerMayBeActive = false;
        _lastPredictiveStorageOutputMayBeActive = false;
        return;
    }

    auto const snapshot = makePredictiveMeterSnapshot();
    _lastPredictiveStorageForecast =
        _predictiveModel.forecast(snapshot, Predictive::ForecastDomain::Storage);
    _lastPredictiveGlobalForecast =
        _predictiveModel.forecast(snapshot, Predictive::ForecastDomain::Global);
    _lastPredictiveChargerMayBeActive =
        _predictiveModel.anyActuatorMayBePhysicallyActive(Predictive::ActuatorKind::Charger);
    _lastPredictiveStorageOutputMayBeActive =
        _predictiveModel.anyActuatorMayBePhysicallyActive(
                Predictive::ActuatorKind::Battery,
                0.5f,
                true/*assume dispatched standby inactive*/)
        || _predictiveModel.anyActuatorMayBePhysicallyActive(Predictive::ActuatorKind::SmartBuffer);
}

Predictive::ControlForecast PowerLimiterClass::getPlannerGridEstimate(
        uint32_t meterSampleMillis) const
{
    if (!_predictiveModelInitialized || _lastPlannerPowerMeterUpdate == 0) {
        return Predictive::ControlForecast();
    }

    Predictive::MeterSnapshot baseSnapshot;
    baseSnapshot.physicalGridWatts = _lastPlannerPowerMeterValue;
    baseSnapshot.meterSampleMillis = _lastPlannerPowerMeterUpdate;
    return _predictiveModel.currentEstimate(
            baseSnapshot,
            Predictive::ForecastDomain::Global,
            meterSampleMillis);
}

bool PowerLimiterClass::predictiveStorageOutputMayBeActive() const
{
    return _lastPredictiveStorageOutputMayBeActive;
}

bool PowerLimiterClass::predictiveChargerMayBeActive() const
{
    return _lastPredictiveChargerMayBeActive;
}

std::vector<std::pair<uint32_t, Predictive::RequestState>>
PowerLimiterClass::predictiveRequestStates() const
{
    std::vector<std::pair<uint32_t, Predictive::RequestState>> states;
    if (!_predictiveModelInitialized) { return states; }

    auto const& ledger = _predictiveModel.ledger();
    states.reserve(ledger.size());
    for (auto const& request : ledger) {
        states.push_back({ request.seq, request.state });
    }
    return states;
}

void PowerLimiterClass::recordPredictiveRequestHistory(
        Predictive::PendingControlRequest const& request)
{
    auto const* actuator = _predictiveModel.findActuator(request.actuatorKey);
    if (actuator == nullptr) { return; }

    PredictiveRequestHistoryRecord record;
    record.seq = request.seq;
    record.serial = predictiveSerialFromActuatorKey(request.actuatorKey);
    record.createdMillis = request.createdMillis;
    record.sentMillis = request.sentMillis.value_or(0);
    record.ackMillis = request.ackMillis.value_or(0);
    record.notBeforeEffectMillis = request.notBeforeEffectMillis.value_or(0);
    record.earliestExpectedMillis = request.earliestExpectedMillis.value_or(0);
    record.typicalEffectMillis = request.typicalEffectMillis.value_or(0);
    record.latestEffectMillis = request.latestEffectMillis.value_or(0);
    record.updatedMillis = millis();
    record.baseSetpointWatts = clampToInt16(request.baseSetpointWatts);
    record.targetSetpointWatts = clampToInt16(request.targetSetpointWatts);
    record.effectiveDeltaWatts = clampToInt16(request.effectiveDeltaWatts);
    record.source = predictiveSourceFromActuatorKind(actuator->kind);
    record.domain = static_cast<uint8_t>(request.domain);
    record.cause = static_cast<uint8_t>(request.cause);
    record.state = static_cast<uint8_t>(request.state);
    record.effectKind = static_cast<uint8_t>(request.effectKind);
    record.transitionKind = static_cast<uint8_t>(request.transitionKind);
    record.capacityFullOutputPossible = request.capacityFullOutputPossible;
    record.dispatchPending = request.dispatchPending;

    auto const& config = Configuration.get();
    if (record.serial != 0) {
        record.inverterIndex = inverterIndexFromSerial(config, record.serial);
    }

    if (DTU_LOG_IS_INFO) {
        String const serial = record.serial == 0
            ? String("none")
            : serialToJsonString(record.serial);
        DTU_LOGI("dpl.request.v1 part=core seq=%" PRIu32 " state=%s source=%s "
                "serial=%s domain=%s cause=%s transition=%s effect=%s",
                record.seq,
                predictiveRequestStateText(static_cast<Predictive::RequestState>(record.state)),
                predictiveHistorySourceText(record.source),
                serial.c_str(),
                predictiveRequestDomainText(static_cast<Predictive::RequestDomain>(record.domain)),
                predictiveRequestCauseText(static_cast<Predictive::RequestCause>(record.cause)),
                predictiveTransitionKindText(static_cast<Predictive::TransitionKind>(record.transitionKind)),
                predictiveEffectKindText(static_cast<Predictive::EffectKind>(record.effectKind)));
        DTU_LOGI("dpl.request.v1 part=delta seq=%" PRIu32 " base_w=%d target_w=%d "
                "effective_delta_w=%d meter_delta_w=%d",
                record.seq,
                static_cast<int>(record.baseSetpointWatts),
                static_cast<int>(record.targetSetpointWatts),
                static_cast<int>(record.effectiveDeltaWatts),
                -static_cast<int>(record.effectiveDeltaWatts));
        DTU_LOGI("dpl.request.v1 part=flags seq=%" PRIu32 " dispatch_pending=%u "
                "capacity_full_output_possible=%u",
                record.seq,
                record.dispatchPending ? 1U : 0U,
                record.capacityFullOutputPossible ? 1U : 0U);
        DTU_LOGI("dpl.request.v1 part=time_a seq=%" PRIu32 " created_ms=%" PRIu32
                " sent_ms=%" PRIu32 " ack_ms=%" PRIu32 " not_before_ms=%" PRIu32,
                record.seq,
                record.createdMillis,
                record.sentMillis,
                record.ackMillis,
                record.notBeforeEffectMillis);
        DTU_LOGI("dpl.request.v1 part=time_b seq=%" PRIu32 " earliest_ms=%" PRIu32
                " typical_ms=%" PRIu32 " latest_ms=%" PRIu32 " updated_ms=%" PRIu32,
                record.seq,
                record.earliestExpectedMillis,
                record.typicalEffectMillis,
                record.latestEffectMillis,
                record.updatedMillis);
    }

    std::lock_guard<std::mutex> lock(_predictiveRequestHistoryMutex);
    if (_predictiveRequestHistory == nullptr || _predictiveRequestHistoryCapacity == 0) { return; }

    _predictiveRequestHistory[_predictiveRequestHistoryWrite] = record;
    _predictiveRequestHistoryWrite =
        (_predictiveRequestHistoryWrite + 1) % _predictiveRequestHistoryCapacity;
    _predictiveRequestHistoryCount =
        std::min(_predictiveRequestHistoryCount + 1, _predictiveRequestHistoryCapacity);
}

void PowerLimiterClass::recordPredictiveRequestHistoryChanges(
        std::vector<std::pair<uint32_t, Predictive::RequestState>> const& before)
{
    if (!_predictiveModelInitialized) { return; }

    auto stateBefore = [&before](uint32_t seq) -> std::optional<Predictive::RequestState> {
        auto const iter = std::find_if(before.begin(), before.end(),
                [seq](auto const& entry) { return entry.first == seq; });
        if (iter == before.end()) { return std::nullopt; }
        return iter->second;
    };

    for (auto const& request : _predictiveModel.ledger()) {
        auto const previous = stateBefore(request.seq);
        if (previous && *previous == request.state) { continue; }
        recordPredictiveRequestHistory(request);
    }
}

void PowerLimiterClass::recordPredictiveInverterRequest(
        PowerLimiterInverter const& inverter,
        uint16_t targetSetpointWatts,
        Predictive::RequestDomain domain,
        Predictive::RequestCause cause)
{
    if (!_predictiveModelInitialized) { return; }

    auto const actuatorKey = predictiveInverterKey(inverter);
    auto* actuator = _predictiveModel.findActuator(actuatorKey);
    if (actuator == nullptr) { return; }

    auto const target = actuator->legalizeSetpoint(targetSetpointWatts);
    if (std::abs(target - actuator->requestedSetpointWatts) <= 0.001f) {
        return;
    }

    auto const baseEffective =
        actuator->effectiveOutputForSetpoint(actuator->requestedSetpointWatts);
    auto const targetEffective = actuator->effectiveOutputForSetpoint(target);
    auto const effectiveDelta = targetEffective - baseEffective;
    bool const solarCapacityProbe = inverter.isSolarPowered()
        && effectiveDelta > 0.0f
        && cause != Predictive::RequestCause::Thermal;
    auto const effectKind = solarCapacityProbe
        ? Predictive::EffectKind::SolarCapacityLimited
        : Predictive::EffectKind::Deterministic;
    auto const actuatorHasPhysicalPendingRequest =
        std::any_of(_predictiveModel.ledger().begin(), _predictiveModel.ledger().end(),
                [&actuatorKey](auto const& request) {
                    return request.actuatorKey == actuatorKey
                        && request.isPhysicalPending();
                });
    auto const actuatorPhysicallyInactive =
        actuator->safeSetpointWatts <= 0.0f
        && (!actuator->measuredOutputWatts || *actuator->measuredOutputWatts <= 0.5f)
        && !actuatorHasPhysicalPendingRequest;
    auto const transitionKind = actuatorPhysicallyInactive && target > 0.0f
        ? Predictive::TransitionKind::Startup
        : target <= 0.0f
            ? Predictive::TransitionKind::Standby
            : Predictive::TransitionKind::Setpoint;
    auto const now = millis();
    auto const before = predictiveRequestStates();
    auto& queued = _predictiveModel.queueRequest(
            actuatorKey,
            domain,
            target,
            effectiveDelta,
            cause,
            effectKind,
            transitionKind,
            now);

    if (effectKind == Predictive::EffectKind::SolarCapacityLimited) {
        queued.capacityFullOutputPossible = true;
    }
    recordPredictiveRequestHistoryChanges(before);
}

void PowerLimiterClass::markPredictiveInverterRequestDispatched(
        PowerLimiterInverter const& inverter,
        PowerLimiterInverter::TargetDispatchEvent const& event)
{
    if (!_predictiveModelInitialized) { return; }

    Predictive::TransitionKind transitionKind = Predictive::TransitionKind::Setpoint;
    if (event.kind == PowerLimiterInverter::TargetDispatchKind::PowerState) {
        transitionKind = event.powerState
            ? Predictive::TransitionKind::Startup
            : Predictive::TransitionKind::Standby;
    }

    auto const actuatorKey = predictiveInverterKey(inverter);
    if (!event.completed) {
        auto markDispatchPending = [&](Predictive::TransitionKind candidate) {
            return _predictiveModel.markLatestQueuedRequestDispatchPending(
                    actuatorKey,
                    event.targetSetpointWatts,
                    candidate);
        };
        auto* request = markDispatchPending(transitionKind);
        if (request == nullptr
                && event.kind == PowerLimiterInverter::TargetDispatchKind::PowerLimit
                && event.targetSetpointWatts > 0) {
            request = markDispatchPending(Predictive::TransitionKind::Startup);
        }
        if (request == nullptr) {
            DTU_LOGD("no queued predictive request matched RF-dispatched target %u W "
                    "for inverter %s",
                    event.targetSetpointWatts,
                    inverter.getSerialStr());
        } else {
            recordPredictiveRequestHistory(*request);
        }
        return;
    }

    auto effectAssumptionMillisFor = [&inverter](Predictive::TransitionKind candidate) {
        bool const batteryPowerStateTransition = inverter.isBatteryPowered()
            && (candidate == Predictive::TransitionKind::Startup
                || candidate == Predictive::TransitionKind::Standby);
        return batteryPowerStateTransition
            ? sBatteryTargetEffectAssumptionMillis
            : PowerLimiterInverter::TargetEffectAssumptionMillis;
    };
    auto markSent = [&](Predictive::TransitionKind candidate) {
        return _predictiveModel.markLatestQueuedRequestSent(
                actuatorKey,
                event.targetSetpointWatts,
                candidate,
                event.sentMillis,
                event.sentMillis + effectAssumptionMillisFor(candidate));
    };
    auto* request = markSent(transitionKind);
    if (request == nullptr
            && event.kind == PowerLimiterInverter::TargetDispatchKind::PowerLimit
            && event.targetSetpointWatts > 0) {
        if (inverter.isBatteryPowered()) {
            request = _predictiveModel.markLatestQueuedRequestDispatchPending(
                    actuatorKey,
                    event.targetSetpointWatts,
                    Predictive::TransitionKind::Startup);
            if (request != nullptr) {
                DTU_LOGD("keeping predictive battery startup request queued until "
                        "power-state dispatch completes for inverter %s",
                        inverter.getSerialStr());
                return;
            }
        }
        request = markSent(Predictive::TransitionKind::Startup);
    }
    if (request == nullptr) {
        DTU_LOGD("no queued predictive request matched dispatched target %u W "
                "for inverter %s",
                event.targetSetpointWatts,
                inverter.getSerialStr());
    } else {
        recordPredictiveRequestHistory(*request);
    }
}

void PowerLimiterClass::discardPredictiveQueuedInverterRequests(
        PowerLimiterInverter const& inverter)
{
    if (!_predictiveModelInitialized) { return; }

    auto const before = predictiveRequestStates();
    _predictiveModel.discardQueuedRequests(
            predictiveInverterKey(inverter),
            inverter.getExpectedOutputAcWatts());
    recordPredictiveRequestHistoryChanges(before);
}

void PowerLimiterClass::markPredictiveGridChargerRequestDispatched(
        GridChargers::PowerLimiterTargetDispatchEvent const& event)
{
    if (!_predictiveModelInitialized) { return; }

    auto const& ledger = _predictiveModel.ledger();
    uint32_t matchedSeq = 0;
    for (auto iter = ledger.rbegin(); iter != ledger.rend(); ++iter) {
        if (iter->actuatorKey != sPredictiveGridChargerKey
                || iter->state != Predictive::RequestState::Queued
                || std::abs(iter->targetSetpointWatts - event.targetInputPowerWatts) > 0.001f) {
            continue;
        }

        matchedSeq = iter->seq;
        break;
    }

    if (matchedSeq == 0) {
        DTU_LOGD("no queued predictive request matched dispatched grid charger target %u W",
                event.targetInputPowerWatts);
        if (event.targetInputPowerWatts == 0) {
            auto* actuator = _predictiveModel.findActuator(sPredictiveGridChargerKey);
            if (actuator != nullptr && actuator->requestedSetpointWatts > 0.5f) {
                auto const recoveryTarget =
                    ceilPositiveWattsToUint16(actuator->requestedSetpointWatts);
                if (recoveryTarget > 0) {
                    _oUnexpectedGridChargerZeroTargetRecoveryWatts = recoveryTarget;
                    DTU_LOGW("unexpected grid charger standby dispatch while predictive "
                            "target is %.0f W; scheduling reassert to %u W",
                            actuator->requestedSetpointWatts,
                            static_cast<unsigned>(recoveryTarget));
                }
            }
        }
        return;
    }

    auto& request = _predictiveModel.markQueuedRequestSent(
            matchedSeq,
            event.sentMillis,
            Predictive::RequestState::Accepted,
            event.sentMillis,
            event.sentMillis,
            std::nullopt,
            std::nullopt,
            event.sentMillis + event.targetEffectAssumptionMillis);
    recordPredictiveRequestHistory(request);
}

void PowerLimiterClass::processPredictiveGridChargerDispatchEvents()
{
    if (!_predictiveModelInitialized) { return; }

    for (auto const& event : GridCharger.consumePowerLimiterTargetDispatchEvents()) {
        markPredictiveGridChargerRequestDispatched(event);
    }
}

void PowerLimiterClass::recordPredictiveGridChargerRequest(
        uint16_t targetSetpointWatts,
        Predictive::RequestCause cause)
{
    if (!_predictiveModelInitialized) { return; }

    auto* actuator = _predictiveModel.findActuator(sPredictiveGridChargerKey);
    if (actuator == nullptr) { return; }

    auto const target = actuator->legalizeSetpoint(targetSetpointWatts);
    if (std::abs(target - actuator->requestedSetpointWatts) <= 0.001f) {
        return;
    }

    auto const baseEffective =
        actuator->effectiveOutputForSetpoint(actuator->requestedSetpointWatts);
    auto const targetEffective = actuator->effectiveOutputForSetpoint(target);
    auto const effectiveDelta = targetEffective - baseEffective;
    auto const now = millis();
    auto const before = predictiveRequestStates();
    _predictiveModel.queueRequest(
            sPredictiveGridChargerKey,
            Predictive::RequestDomain::Storage,
            target,
            effectiveDelta,
            cause,
            Predictive::EffectKind::Deterministic,
            actuator->requestedSetpointWatts <= 0.0f && target > 0.0f
                    ? Predictive::TransitionKind::Startup
                    : target <= 0.0f
                        ? Predictive::TransitionKind::Standby
                        : Predictive::TransitionKind::Setpoint,
            now);
    recordPredictiveRequestHistoryChanges(before);
}

uint16_t PowerLimiterClass::applyGridChargerInputPowerIncrease(
        uint16_t increase,
        Predictive::RequestCause cause)
{
    auto const startedMicros = micros();
    auto const applied = GridCharger.applyPowerLimiterInputPowerIncrease(increase);
    recordGridChargerApplyRuntime(startedMicros);
    if (applied > 0) {
        recordPredictiveGridChargerRequest(
                GridCharger.getPowerLimiterExpectedInputPowerWatts(),
                cause);
    }
    return applied;
}

uint16_t PowerLimiterClass::applyGridChargerInputPowerReduction(
        uint16_t reduction,
        Predictive::RequestCause cause)
{
    auto const startedMicros = micros();
    auto const applied = GridCharger.applyPowerLimiterInputPowerReduction(reduction);
    recordGridChargerApplyRuntime(startedMicros);
    if (applied > 0) {
        recordPredictiveGridChargerRequest(
                GridCharger.getPowerLimiterExpectedInputPowerWatts(),
                cause);
    }
    return applied;
}

void PowerLimiterClass::loop()
{
    struct LoopRuntimeGuard {
        PowerLimiterClass* instance;
        uint32_t startedMicros;
        ~LoopRuntimeGuard() { instance->recordLoopRuntime(startedMicros); }
    } loopRuntimeGuard { this, micros() };

    auto const& config = Configuration.get();
    auto finish = [this](Status status) {
        recordTraceSample(status);
        announceStatus(status);
    };

    // if (PowerMeter.isDataValid()) {
    //     updateGridMedianFilter();
    // } else {
    //     resetGridMedianFilter();
    // }

    // we know that the Hoymiles library refuses to send any message to any
    // inverter until the system has valid time information. until then we can
    // do nothing, not even shutdown the inverter.
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 5)) {
        return finish(Status::WaitingForValidTimestamp);
    }

    // take care that the last requested power limits and power states are
    // actually reached. The predictive planner may still calculate the next
    // absolute setpoints while earlier effects are pending; the ledger keeps
    // those pending effects explicit.
    bool const inverterCommandPending = updateInverters();

    if (inverterCommandPending
            && (_reloadConfigFlag
                || !config.PowerLimiter.Enabled
                || Mode::Disabled == _mode
                || Mode::UnconditionalFullSolarPassthrough == _mode)) {
        return finish(Status::InverterCmdPending);
    }

    if (_reloadConfigFlag) {
        reloadConfig();
        return finish(Status::ConfigReload);
    }

    if (!config.PowerLimiter.Enabled) {
        return finish(Status::DisabledByConfig);
    }

    if (Mode::Disabled == _mode) {
        return finish(Status::DisabledByMqtt);
    }

    if (_inverters.empty()) {
        return finish(Status::InverterInvalid);
    }

    bool const gridChargerManaged = isGridChargerManaged()
        && GridCharger.supportsPowerLimiterControl();

    uint32_t latestOutputReferenceMillis = 0;
    bool hasOutputReference = false;

    for (auto const& upInv : _inverters) {
        // in particular, we don't want to wait for stats from inverters that
        // are not eligible because they are (currently) unreachable. this is
        // fine as we ignore them throughout the DPL loop if they are not eligible.
        if (!upInv->isEligible()) { continue; }

        auto oOutputReferenceMillis = upInv->getOutputReferenceMillis();
        if (!oOutputReferenceMillis) {
            return finish(Status::InverterStatsPending);
        }

        latestOutputReferenceMillis = hasOutputReference
            ? latestMillis(*oOutputReferenceMillis, latestOutputReferenceMillis)
            : *oOutputReferenceMillis;
        hasOutputReference = true;
    }

    if (gridChargerManaged) {
        auto oOutputReferenceMillis = GridCharger.getPowerLimiterOutputReferenceMillis();
        if (!oOutputReferenceMillis) {
            return finish(Status::InverterStatsPending);
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

    bool const emergencyStopActive = isFlexibleLoadEmergencyStopActive();
    bool const emergencyFullOutputDue = emergencyStopActive
        && shouldRequestEmergencyFullOutput();
    if (!emergencyStopActive) {
        resetEmergencyFullOutputThrottle();
    }
    bool const outputSettled = !PowerMeter.isDataValid()
        || !hasOutputReference
        || millisAtOrAfter(getRegulationPowerMeterUpdate(), latestOutputReferenceMillis);
    bool const powerMeterValid = PowerMeter.isDataValid();
    bool const backoffActive = (millis() - _lastCalculation) < _calculationBackoffMs;
    updateDynamicBatteryTarget();
    auto const currentStorageTargetPowerConsumption = getStorageTargetPowerConsumption();
    auto const currentTargetPowerConsumption = Predictive::validateTargetOrdering(
            currentStorageTargetPowerConsumption,
            getTargetPowerConsumption(),
            true/*clamp*/);
    auto storageTargetPowerConsumption = currentStorageTargetPowerConsumption;
    auto targetPowerConsumption = currentTargetPowerConsumption;
    bool dynamicStorageTargetFeedForwardActive = false;

    uint16_t gridChargerInitialInput = 0;
    uint16_t stagedGridChargerInput = 0;
    bool stagedGridChargerTargetChanged = false;
    Predictive::RequestCause stagedGridChargerCause = Predictive::RequestCause::Normal;

    auto gridChargerPlannedMeterDeltaWatts = [&]() -> float {
        if (!gridChargerManaged || !stagedGridChargerTargetChanged) { return 0.0f; }
        return static_cast<float>(stagedGridChargerInput)
            - static_cast<float>(gridChargerInitialInput);
    };

    auto refreshPredictiveForecasts = [this,&gridChargerPlannedMeterDeltaWatts,
            &stagedGridChargerInput]() {
        updatePredictiveForecasts();
        auto const chargerMeterDelta = gridChargerPlannedMeterDeltaWatts();
        if (chargerMeterDelta == 0.0f) { return; }
        _lastPredictiveStorageForecast = Predictive::forecastWithPlannedMeterDelta(
                _lastPredictiveStorageForecast,
                chargerMeterDelta);
        _lastPredictiveGlobalForecast = Predictive::forecastWithPlannedMeterDelta(
                _lastPredictiveGlobalForecast,
                chargerMeterDelta);
        if (stagedGridChargerInput > 0) {
            _lastPredictiveChargerMayBeActive = true;
        }
    };
    bool controlUpdated = false;
    bool gridChargerLimitUpdated = false;
    bool gridChargerShutdownRequestedThisCycle = false;

    auto legalizeGridChargerTarget = [&](uint16_t target) -> uint16_t {
        if (!gridChargerManaged) { return 0; }
        auto const proposal = GridCharger.getPowerLimiterControlProposal(target);
        return proposal ? proposal->targetInputPowerWatts : target;
    };

    auto stageGridChargerTarget = [&](uint16_t target,
            Predictive::RequestCause cause = Predictive::RequestCause::Normal) -> uint16_t {
        if (!gridChargerManaged) { return 0; }

        target = legalizeGridChargerTarget(target);
        auto const before = stagedGridChargerInput;
        if (target == before) { return 0; }

        auto const delta = before > target
            ? static_cast<uint16_t>(before - target)
            : static_cast<uint16_t>(target - before);
        bool const mandatoryChange = cause == Predictive::RequestCause::Safety
            || target == 0
            || before == 0;
        if (!mandatoryChange && delta < sGridChargerStagedTargetDeadbandWatts) {
            return 0;
        }

        stagedGridChargerInput = target;
        stagedGridChargerTargetChanged = stagedGridChargerInput != gridChargerInitialInput;
        if (cause == Predictive::RequestCause::Safety
                || stagedGridChargerCause != Predictive::RequestCause::Safety) {
            stagedGridChargerCause = cause;
        }
        gridChargerLimitUpdated = stagedGridChargerTargetChanged;
        controlUpdated = true;
        if (stagedGridChargerInput == 0 || stagedGridChargerInput < before) {
            gridChargerShutdownRequestedThisCycle = stagedGridChargerInput == 0;
        }
        refreshPredictiveForecasts();

        return delta;
    };

    auto commitStagedGridChargerTarget = [&]() -> uint16_t {
        if (!gridChargerManaged || !stagedGridChargerTargetChanged) { return 0; }

        auto const before = GridCharger.getPowerLimiterExpectedInputPowerWatts();
        if (before == stagedGridChargerInput) { return 0; }

        auto const startedMicros = micros();
        uint16_t applied = 0;
        if (stagedGridChargerInput > before) {
            applied = GridCharger.applyPowerLimiterInputPowerIncrease(
                    static_cast<uint16_t>(stagedGridChargerInput - before));
        } else {
            applied = GridCharger.applyPowerLimiterInputPowerReduction(
                    static_cast<uint16_t>(before - stagedGridChargerInput));
        }
        recordGridChargerApplyRuntime(startedMicros);

        auto const actualTarget = GridCharger.getPowerLimiterExpectedInputPowerWatts();
        if (actualTarget != before) {
            recordPredictiveGridChargerRequest(actualTarget, stagedGridChargerCause);
        }
        if (actualTarget != stagedGridChargerInput) {
            DTU_LOGD("grid charger staged target %u W committed as %u W",
                    static_cast<unsigned>(stagedGridChargerInput),
                    static_cast<unsigned>(actualTarget));
        }

        return applied;
    };

    auto commitStagedGridChargerTargetIfNeeded = [&]() -> uint16_t {
        if (!stagedGridChargerTargetChanged) { return 0; }

        auto const committed = commitStagedGridChargerTarget();
        if (committed == 0) {
            gridChargerLimitUpdated = false;
        }
        return committed;
    };

    auto finishAfterStagedGridChargerCommit = [&](Status status) {
        commitStagedGridChargerTargetIfNeeded();
        return finish(status);
    };

    auto const predictiveModelWasInitialized = _predictiveModelInitialized;
    if (predictiveModelWasInitialized) {
        processPredictiveGridChargerDispatchEvents();
    }
    syncPredictiveActuators(gridChargerManaged);
    if (!predictiveModelWasInitialized) {
        processPredictiveGridChargerDispatchEvents();
    }

    if (gridChargerManaged) {
        gridChargerInitialInput = GridCharger.getPowerLimiterExpectedInputPowerWatts();
        stagedGridChargerInput = gridChargerInitialInput;
        auto const proposedInput = legalizeGridChargerTarget(stagedGridChargerInput);
        if (proposedInput != stagedGridChargerInput) {
            auto const previousInput = stagedGridChargerInput;
            auto const applied = stageGridChargerTarget(proposedInput);
            if (applied > 0
                    && proposedInput < previousInput
                    && DTU_LOG_IS_INFO
                    && shouldLogDplGuard(DplGuardLog::GridChargerSocPlanCap)) {
                auto const gridChargerMaxInput = GridCharger.getPowerLimiterMaxInputPowerWatts();
                DTU_LOGI("dpl.guard.v1 guard=soc_plan "
                        "action=plan_grid_charger_controller_target "
                        "expected_w=%u proposed_w=%u max_w=%u reduction_w=%u",
                        static_cast<unsigned>(previousInput),
                        static_cast<unsigned>(stagedGridChargerInput),
                        static_cast<unsigned>(gridChargerMaxInput),
                        static_cast<unsigned>(applied));
            }
        }
    }

    if (gridChargerManaged && _oUnexpectedGridChargerZeroTargetRecoveryWatts) {
        auto const recoveryTarget = *_oUnexpectedGridChargerZeroTargetRecoveryWatts;
        _oUnexpectedGridChargerZeroTargetRecoveryWatts = std::nullopt;

        auto const gridChargerInput = stagedGridChargerInput;
        auto const gridChargerMaxInput = GridCharger.getPowerLimiterMaxInputPowerWatts();
        auto const target = static_cast<uint16_t>(std::min<uint32_t>(
                recoveryTarget,
                gridChargerMaxInput));

        if (target > gridChargerInput) {
            auto const applied = stageGridChargerTarget(
                    target,
                    Predictive::RequestCause::Normal);
            if (applied > 0) {
                DTU_LOGI("reasserted grid charger target to %u W after unexpected "
                        "standby dispatch",
                        static_cast<unsigned>(stagedGridChargerInput));
            }
        } else {
            DTU_LOGD("not reasserting unexpected grid charger standby dispatch "
                    "(target %u W, expected %u W, max %u W)",
                    static_cast<unsigned>(recoveryTarget),
                    static_cast<unsigned>(gridChargerInput),
                    static_cast<unsigned>(gridChargerMaxInput));
        }
    }

    advancePredictiveLedger();
    // Ledger settlement can release an actuator for same-loop resync before forecasting.
    syncPredictiveActuators(gridChargerManaged);
    refreshPredictiveForecasts();

    auto const nowMillis = millis();
    auto dynamicTargetForecastInput = _lastPredictiveGlobalForecast;
    auto const samePassGridChargerMeterDelta = gridChargerPlannedMeterDeltaWatts();
    if (samePassGridChargerMeterDelta != 0.0f) {
        dynamicTargetForecastInput = Predictive::forecastWithPlannedMeterDelta(
                dynamicTargetForecastInput,
                -samePassGridChargerMeterDelta);
    }
    auto const dynamicTargetForecastForPlanner =
        calcDynamicStorageTargetForecast(dynamicTargetForecastInput, nowMillis);
    if (dynamicTargetForecastForPlanner.valid) {
        auto const forecastStorageTarget =
            static_cast<float>(dynamicTargetForecastForPlanner.nominal);
        auto forecastTarget = static_cast<float>(config.PowerLimiter.TargetPowerConsumption);
        if (config.PowerLimiter.TargetPowerConsumptionFollowStorageTarget) {
            forecastTarget = forecastStorageTarget
                - static_cast<float>(config.PowerLimiter.TargetPowerConsumptionStorageOffset);
        }
        storageTargetPowerConsumption = forecastStorageTarget;
        targetPowerConsumption = Predictive::validateTargetOrdering(
                storageTargetPowerConsumption,
                forecastTarget,
                true/*clamp*/);
        dynamicStorageTargetFeedForwardActive = true;
    }
    auto const plannerMaxIntervalMillis =
        adaptivePlannerMaxIntervalMillis(config.PowerLimiter);
    auto const plannerGridChangeThresholdWatts =
        static_cast<float>(config.PowerLimiter.TargetPowerConsumptionHysteresis);
    auto const plannerTargetChangeThresholdWatts =
        static_cast<float>(config.PowerLimiter.AdaptivePlannerTargetChangeThreshold);
    auto const smallCorrectionDampingThresholdWatts =
        static_cast<float>(config.PowerLimiter.SmallCorrectionDampingThreshold);
    bool const plannerBootstrapDue = powerMeterValid
        && (_lastPlannerPowerMeterUpdate == 0 || _lastPlannerRunMillis == 0);
    auto const plannerInputPowerMeterUpdate = getRegulationPowerMeterUpdate();
    auto const plannerGridEstimate = powerMeterValid && !plannerBootstrapDue
        ? getPlannerGridEstimate(plannerInputPowerMeterUpdate)
        : Predictive::ControlForecast();
    auto const meterValue = getRegulationPowerMeterTotal();
    auto const plannerInputPowerMeterValue = meterValue;
    auto const plannerGridDeviationWatts = plannerGridEstimate.valid
        ? Predictive::distanceOutsideCorridor(meterValue, plannerGridEstimate)
        : 0.0f;
    auto const plannerGridUncertaintyWatts = plannerGridEstimate.valid
        ? Predictive::corridorUncertaintyWatts(plannerGridEstimate)
        : 0.0f;
    auto const plannerGridOutsideUncertaintyRatio =
        Predictive::outsideCorridorUncertaintyRatio(
                plannerGridDeviationWatts,
                plannerGridUncertaintyWatts);
    bool const plannerGridOutsideHysteresis = plannerGridEstimate.valid
        && plannerGridDeviationWatts > plannerGridChangeThresholdWatts
        && (plannerGridUncertaintyWatts <= plannerGridChangeThresholdWatts
            || plannerGridOutsideUncertaintyRatio >= sAdaptivePlannerMinOutsideUncertaintyRatio);
    auto const plannerTargetDeviationWatts = plannerBootstrapDue
        ? 0.0f
        : Predictive::targetChangeDistanceWatts(
                storageTargetPowerConsumption,
                targetPowerConsumption,
                _lastPlannerStorageTargetPowerConsumption,
                _lastPlannerTargetPowerConsumption);
    bool const plannerTargetOutsideEstimateRaw = !plannerBootstrapDue
        && plannerTargetDeviationWatts > plannerTargetChangeThresholdWatts;
    auto const lastPlannerAgeMillis = _lastPlannerRunMillis == 0
        ? 0
        : nowMillis - _lastPlannerRunMillis;
    bool const plannerIntervalDue = !plannerBootstrapDue
        && lastPlannerAgeMillis >= plannerMaxIntervalMillis;
    bool const plannerGridOutsideIntegralCandidate =
        config.PowerLimiter.TargetPowerConsumptionCorridorErrorThresholdWs > 0
        && plannerGridEstimate.valid
        && plannerGridDeviationWatts > 0.0f
        && (plannerGridUncertaintyWatts <= plannerGridChangeThresholdWatts
            || plannerGridOutsideUncertaintyRatio >= sAdaptivePlannerMinOutsideUncertaintyRatio);
    auto const plannerGridOutsideIntegralThresholdWattMillis =
        wattSecondsToWattMillis(
                config.PowerLimiter.TargetPowerConsumptionCorridorErrorThresholdWs);
    if (plannerGridOutsideIntegralCandidate) {
        if (_lastPlannerGridErrorUpdateMillis != 0) {
            auto const elapsedMillis = nowMillis - _lastPlannerGridErrorUpdateMillis;
            _plannerGridOutsideErrorIntegralWattMillis = std::min(
                    _plannerGridOutsideErrorIntegralWattMillis
                        + plannerGridDeviationWatts * static_cast<float>(elapsedMillis),
                    plannerGridOutsideIntegralThresholdWattMillis);
        }
        _lastPlannerGridErrorUpdateMillis = nowMillis;
    } else {
        _lastPlannerGridErrorUpdateMillis = 0;
        _plannerGridOutsideErrorIntegralWattMillis = 0.0f;
    }
    bool const plannerGridOutsideEstimate =
        plannerGridOutsideIntegralCandidate
        && _plannerGridOutsideErrorIntegralWattMillis
            >= plannerGridOutsideIntegralThresholdWattMillis;
    auto const plannerTargetBandErrorWatts =
        Predictive::targetBandErrorWatts(
                storageTargetPowerConsumption,
                targetPowerConsumption,
                meterValue,
                _lastPredictiveGlobalForecast);
    auto const plannerTargetBandErrorThresholdWattMillis =
        wattSecondsToWattMillis(
                config.PowerLimiter.TargetPowerConsumptionBandErrorThresholdWs);
    bool const plannerTargetBandIntegralCandidate =
        plannerTargetBandErrorThresholdWattMillis > 0.0f
        && _lastPredictiveGlobalForecast.valid
        && plannerTargetBandErrorWatts > 0.0f;
    if (plannerTargetBandIntegralCandidate) {
        if (_lastPlannerTargetBandErrorUpdateMillis != 0) {
            auto const elapsedMillis = nowMillis - _lastPlannerTargetBandErrorUpdateMillis;
            _plannerTargetBandErrorIntegralWattMillis = std::min(
                    _plannerTargetBandErrorIntegralWattMillis
                        + plannerTargetBandErrorWatts * static_cast<float>(elapsedMillis),
                    plannerTargetBandErrorThresholdWattMillis);
        }
        _lastPlannerTargetBandErrorUpdateMillis = nowMillis;
    } else {
        _lastPlannerTargetBandErrorUpdateMillis = 0;
        _plannerTargetBandErrorIntegralWattMillis = 0.0f;
    }
    bool const plannerTargetBandEstimate =
        plannerTargetBandIntegralCandidate
        && _plannerTargetBandErrorIntegralWattMillis
            >= plannerTargetBandErrorThresholdWattMillis;
    auto const plannerPhysicalPending = [&]() {
        if (!_predictiveModelInitialized) { return false; }

        for (auto const& request : _predictiveModel.ledger()) {
            if (request.isPhysicalPending()) { return true; }
        }
        return false;
    }();
    bool const plannerTargetOutsideEstimateCandidate =
        plannerTargetOutsideEstimateRaw;
    bool const plannerTargetDeferredByPendingRf =
        plannerTargetOutsideEstimateCandidate
        && plannerPhysicalPending
        && plannerGridEstimate.valid
        && !plannerGridOutsideHysteresis
        && !plannerIntervalDue;
    bool const plannerTargetOutsideEstimate =
        plannerTargetOutsideEstimateCandidate && !plannerTargetDeferredByPendingRf;
    bool const plannerRunDue = powerMeterValid
        && (plannerBootstrapDue
            || stagedGridChargerTargetChanged
            || plannerGridOutsideEstimate
            || plannerTargetOutsideEstimate
            || plannerTargetBandEstimate
            || plannerIntervalDue);
    if (plannerTargetDeferredByPendingRf
            && DTU_LOG_IS_INFO
            && shouldLogDplGuard(DplGuardLog::PlannerTargetDeferredByPendingRf)) {
        DTU_LOGI("dpl.guard.v1 guard=pending_rf "
                "action=defer_planner_target_change "
                "target_deviation_w=%.0f planner_deviation_w=%.0f "
                "planner_uncertainty_w=%.0f",
                plannerTargetDeviationWatts,
                plannerGridDeviationWatts,
                plannerGridUncertaintyWatts);
    }

    if (!emergencyStopActive && isInverterRadioQueueCongested()) {
        if (DTU_LOG_IS_INFO && shouldLogDplGuard(DplGuardLog::InverterRfCongested)) {
            DTU_LOGI("dpl.guard.v1 guard=inverter_rf_queue action=defer_planner "
                    "queue_size=%u threshold=%u",
                    static_cast<unsigned>(getMaxInverterRadioQueueSize()),
                    static_cast<unsigned>(sInverterRfQueueCongestionThreshold));
        }
        return finishAfterStagedGridChargerCommit(Status::InverterCmdPending);
    }

    if (!emergencyStopActive && !powerMeterValid) {
        if (gridChargerManaged) {
            auto const gridChargerInput =
                GridCharger.getPowerLimiterExpectedInputPowerWatts();
            if (gridChargerInput > 0) {
                auto const appliedReduction = applyGridChargerInputPowerReduction(
                        gridChargerInput,
                        Predictive::RequestCause::Safety);
                if (appliedReduction > 0) {
                    if (DTU_LOG_IS_INFO && shouldLogDplGuard(
                            DplGuardLog::InvalidMeterGridChargerShed)) {
                        DTU_LOGI("dpl.guard.v1 guard=meter_validity "
                                "action=shed_grid_charger_before_meter_valid "
                                "reduction_w=%u",
                                static_cast<unsigned>(appliedReduction));
                    }
                    DTU_LOGD("turning grid charger down by %u W before returning "
                            "because power meter data is invalid",
                            appliedReduction);
                }
            }
        }
        return finishAfterStagedGridChargerCommit(Status::PowerMeterPending);
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

    if (!emergencyStopActive && autoRestartInverters()) {
        return finishAfterStagedGridChargerCommit(Status::InverterCmdPending);
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

    if (emergencyStopActive) {
        if (!emergencyFullOutputDue) {
            rememberRegulationOutcome(0.0f, false);
            return finishAfterStagedGridChargerCommit(Status::EmergencyFullOutput);
        }
        return emergencyFullOutput();
    }

    auto const totalAllowance = config.PowerLimiter.TotalUpperPowerLimit;
    auto getExpectedOutput = [this](inverter_filter_t filter) -> uint16_t {
        uint32_t res = 0;
        for (auto const& upInv : _inverters) {
            if (!upInv->isEligible()) { continue; }
            if (!filter(*upInv)) { continue; }
            res += upInv->getExpectedOutputAcWatts();
        }

        return static_cast<uint16_t>(std::min<uint32_t>(
                res,
                std::numeric_limits<uint16_t>::max()));
    };

    auto getExpectedTotalOutput = [&]() -> uint16_t {
        uint32_t total = getExpectedOutput(sSolarPoweredFilter);
        total += getExpectedOutput(sSmartBufferPoweredFilter);
        total += getExpectedOutput(sBatteryPoweredFilter);
        return static_cast<uint16_t>(std::min<uint32_t>(
                total,
                std::numeric_limits<uint16_t>::max()));
    };

    auto getStorageDecision = [&]() {
        return Predictive::decideCorrection(
                storageTargetPowerConsumption,
                _lastPredictiveStorageForecast,
                0.0f,
                smallCorrectionDampingThresholdWatts);
    };

    auto getSolarTargetDecision = [&]() {
        return Predictive::decideSolarTargetCorrection(
                targetPowerConsumption,
                _lastPredictiveGlobalForecast,
                0.0f,
                smallCorrectionDampingThresholdWatts);
    };

    auto markRegulationSampleProcessed = [&]() {
        auto const oExpectedPowerMeterValue =
            Predictive::deterministicExpectedMeterValue(_lastPredictiveGlobalForecast);
        rememberRegulationOutcome(
                oExpectedPowerMeterValue.value_or(0.0f),
                oExpectedPowerMeterValue.has_value());
    };

    auto markPlannerRun = [&]() {
        _lastPlannerRunMillis = millis();
        _lastPlannerPowerMeterUpdate = plannerInputPowerMeterUpdate;
        _lastPlannerPowerMeterValue = plannerInputPowerMeterValue;
        _lastPlannerStorageTargetPowerConsumption = storageTargetPowerConsumption;
        _lastPlannerTargetPowerConsumption = targetPowerConsumption;
        _lastPlannerGridErrorUpdateMillis = 0;
        _plannerGridOutsideErrorIntegralWattMillis = 0.0f;
        _lastPlannerTargetBandErrorUpdateMillis = 0;
        _plannerTargetBandErrorIntegralWattMillis = 0.0f;
    };

    // Keep all actuator commands in one planner pass tied to the same meter sample.
    bool plannerPowerMeterSampleChanged = false;
    auto deferStalePlannerSample = [&](char const* stage) -> bool {
        if (!powerMeterValid || plannerInputPowerMeterUpdate == 0) {
            return false;
        }

        auto const currentPowerMeterUpdate = getRegulationPowerMeterUpdate();
        if (currentPowerMeterUpdate == plannerInputPowerMeterUpdate) {
            return false;
        }

        if (!plannerPowerMeterSampleChanged) {
            plannerPowerMeterSampleChanged = true;
            _lastPlannerRunMillis = 0;
            if (DTU_LOG_IS_INFO && shouldLogDplGuard(
                    DplGuardLog::PlannerPowerMeterSampleChanged)) {
                DTU_LOGI("dpl.guard.v1 guard=power_meter_sample "
                        "action=defer_stale_planner stage=%s "
                        "planned_update_ms=%" PRIu32 " current_update_ms=%" PRIu32 " "
                        "planned_grid_w=%.0f current_grid_w=%.0f",
                        stage,
                        plannerInputPowerMeterUpdate,
                        currentPowerMeterUpdate,
                        plannerInputPowerMeterValue,
                        getRegulationPowerMeterTotal());
            }
        }

        return true;
    };

    auto shouldRunPlannerNow = [&]() {
        // Integrated errors reset after a planner run and may request another
        // correction once they exceed their configured energy budget again.
        return plannerRunDue;
    };

    if (!shouldRunPlannerNow()) {
        markRegulationSampleProcessed();
        return finishAfterStagedGridChargerCommit(Status::Stable);
    }

    auto const plannerLoggedTargetBandErrorIntegralWattMillis =
        _plannerTargetBandErrorIntegralWattMillis;

    if (DTU_LOG_IS_DEBUG) {
        auto const storageDecision = getStorageDecision();
        auto const globalDecision = getSolarTargetDecision();
        logPredictiveDecisionEvent(
                plannerBootstrapDue,
                plannerGridOutsideEstimate,
                plannerTargetOutsideEstimate,
                plannerTargetBandEstimate,
                plannerIntervalDue,
                lastPlannerAgeMillis,
                meterValue,
                storageTargetPowerConsumption,
                targetPowerConsumption,
                _lastPredictiveStorageForecast,
                storageDecision,
                _lastPredictiveGlobalForecast,
                globalDecision,
                plannerGridDeviationWatts,
                plannerGridUncertaintyWatts,
                plannerGridOutsideUncertaintyRatio,
                plannerTargetBandErrorWatts,
                plannerLoggedTargetBandErrorIntegralWattMillis,
                plannerTargetBandErrorThresholdWattMillis,
                _lastPredictiveChargerMayBeActive,
                _lastPredictiveStorageOutputMayBeActive,
                _predictiveModelInitialized ? _predictiveModel.ledger().size() : 0,
                _predictiveModelInitialized ? _predictiveModel.activeRequestCount() : 0);

        auto const& dynamicTargetForecast = dynamicTargetForecastForPlanner;
        if (dynamicTargetForecast.valid || dynamicTargetForecast.dueValid
                || dynamicTargetForecast.oracleValid) {
            DTU_LOGD("dpl.dynamic_target.v1 part=forecast horizon_ms=%u "
                    "current_w=%.0f planner_w=%.0f target_w=%.0f "
                    "feedforward=%u valid=%u min_w=%d nominal_w=%d max_w=%d",
                    static_cast<unsigned>(dynamicTargetForecast.horizonMillis),
                    currentStorageTargetPowerConsumption,
                    storageTargetPowerConsumption,
                    targetPowerConsumption,
                    dynamicStorageTargetFeedForwardActive ? 1U : 0U,
                    dynamicTargetForecast.valid ? 1U : 0U,
                    dynamicTargetForecast.valid ? dynamicTargetForecast.min : 0,
                    dynamicTargetForecast.valid ? dynamicTargetForecast.nominal : 0,
                    dynamicTargetForecast.valid ? dynamicTargetForecast.max : 0);
            DTU_LOGD("dpl.dynamic_target.v1 part=due valid=%u min_w=%d "
                    "nominal_w=%d max_w=%d age_ms=%u oracle_valid=%u "
                    "oracle_w=%d oracle_age_ms=%u",
                    dynamicTargetForecast.dueValid ? 1U : 0U,
                    dynamicTargetForecast.dueValid ? dynamicTargetForecast.dueMin : 0,
                    dynamicTargetForecast.dueValid ? dynamicTargetForecast.dueNominal : 0,
                    dynamicTargetForecast.dueValid ? dynamicTargetForecast.dueMax : 0,
                    static_cast<unsigned>(dynamicTargetForecast.dueAgeMillis),
                    dynamicTargetForecast.oracleValid ? 1U : 0U,
                    dynamicTargetForecast.oracleValid ? dynamicTargetForecast.oracle : 0,
                    static_cast<unsigned>(dynamicTargetForecast.oracleAgeMillis));
        }
    }

    if (DTU_LOG_IS_DEBUG) {
        auto const now = millis();

        DTU_LOGD("up %lu s, it is %s, next inverter restart at %d s (set to %d)",
                now/1000,
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

            auto dcVoltage = getBatteryVoltage(true/*log voltages only once per planner run*/);
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
    }

    if (deferStalePlannerSample("before_regulation")) {
        return finishAfterStagedGridChargerCommit(Status::Stable);
    }

    markPlannerRun();

    auto outputIncreaseWithinTotalLimit = [&](float requestedIncrease) -> uint16_t {
        auto const requested = ceilPositiveWattsToUint16(requestedIncrease);
        auto const total = getExpectedTotalOutput();
        auto const room = totalAllowance > total
            ? static_cast<uint16_t>(totalAllowance - total)
            : static_cast<uint16_t>(0);
        return std::min(requested, room);
    };

    uint32_t solarOutputLimitHeadroom = sSolarOutputLimitSafetyHeadroomWatts;
    if (gridChargerManaged && PowerMeter.isDataValid()) {
        auto const gridChargerMaxInput = GridCharger.getPowerLimiterMaxInputPowerWatts();
        auto const gridChargerInput = std::min(
                stagedGridChargerInput,
                gridChargerMaxInput);
        _solarOutputLimitChargerHeadroomWatts = std::min(
                _solarOutputLimitChargerHeadroomWatts,
                gridChargerMaxInput);
        if (gridChargerInput > _solarOutputLimitChargerHeadroomWatts) {
            _solarOutputLimitChargerHeadroomWatts = gridChargerInput;
        } else if (outputSettled && !inverterCommandPending && !backoffActive) {
            _solarOutputLimitChargerHeadroomWatts = gridChargerInput;
        }

        solarOutputLimitHeadroom += _solarOutputLimitChargerHeadroomWatts;
    } else {
        _solarOutputLimitChargerHeadroomWatts = 0;
    }

    uint16_t const solarOutputLimitHeadroomWatts =
        static_cast<uint16_t>(std::min<uint32_t>(
                solarOutputLimitHeadroom,
                std::numeric_limits<uint16_t>::max()));

    auto batteryBmsDischargeWatts = [&]() -> std::optional<float> {
        if (!config.Battery.Enabled) { return std::nullopt; }

        auto const stats = Battery.getStats();
        if (!stats->isVoltageValid() || !stats->isCurrentValid()) {
            return std::nullopt;
        }

        auto const power = stats->getVoltage() * stats->getChargeCurrent();
        if (power >= 0.0f) { return 0.0f; }
        return -power;
    };

    auto shouldReassertBatteryStandby = [&]() -> bool {
        auto const threshold = sBatteryStandbyReassertThresholdWatts;
        auto const bmsDischarge = batteryBmsDischargeWatts();
        if (bmsDischarge) {
            return *bmsDischarge > threshold;
        }

        return PowerMeter.isDataValid()
            && getRegulationPowerMeterTotal()
                < (currentStorageTargetPowerConsumption - threshold);
    };

    float sameStepStorageInverterMeterDeltaWatts = 0.0f;

    auto requestInverterTarget = [&](
            uint16_t target,
            inverter_filter_t filter,
            std::string const& filterExpression,
            bool allowStandby,
            bool capOutputLimit,
            uint16_t outputLimitHeadroom,
            Predictive::RequestDomain requestDomain,
            Predictive::RequestCause requestCause,
            bool forceStandbyReassertRequest) -> uint16_t {
        if (deferStalePlannerSample("inverter_target")) {
            return getExpectedOutput(filter);
        }

        auto const before = getExpectedOutput(filter);
        bool const forceStandbyReassert =
            target == 0
            && allowStandby
            && filterExpression == sBatteryPoweredExpression
            && (forceStandbyReassertRequest || shouldReassertBatteryStandby());
        auto const covered = updateInverterLimits(
                target,
                filter,
                filterExpression,
                allowStandby,
                capOutputLimit,
                outputLimitHeadroom,
                requestDomain,
                requestCause,
                forceStandbyReassert);
        if (covered != before) {
            controlUpdated = true;
            if (requestDomain == Predictive::RequestDomain::Storage) {
                sameStepStorageInverterMeterDeltaWatts +=
                    static_cast<float>(before) - static_cast<float>(covered);
            }
        }
        refreshPredictiveForecasts();
        return covered;
    };

    auto increaseInverterOutput = [&](
            inverter_filter_t filter,
            std::string const& filterExpression,
            float residualWatts,
            Predictive::RequestDomain requestDomain) -> uint16_t {
        auto const increase = outputIncreaseWithinTotalLimit(residualWatts);
        if (increase == 0) { return getExpectedOutput(filter); }

        auto const before = getExpectedOutput(filter);
        auto const target = static_cast<uint16_t>(std::min<uint32_t>(
                static_cast<uint32_t>(before) + increase,
                std::numeric_limits<uint16_t>::max()));
        return requestInverterTarget(
                target,
                filter,
                filterExpression,
                true/*allowStandby*/,
                filterExpression == sSolarPoweredExpression,
                filterExpression == sSolarPoweredExpression ? solarOutputLimitHeadroomWatts : 0,
                requestDomain,
                Predictive::RequestCause::Normal,
                false/*forceStandbyReassertRequest*/);
    };

    auto reduceInverterOutput = [&](
            inverter_filter_t filter,
            std::string const& filterExpression,
            float residualWatts,
            Predictive::RequestDomain requestDomain,
            Predictive::RequestCause requestCause = Predictive::RequestCause::Normal) -> uint16_t {
        auto const reduction = ceilPositiveWattsToUint16(residualWatts);
        auto const before = getExpectedOutput(filter);
        auto const target = before > reduction ? static_cast<uint16_t>(before - reduction) : 0;
        return requestInverterTarget(
                target,
                filter,
                filterExpression,
                true/*allowStandby*/,
                false/*capOutputLimit*/,
                0,
                requestDomain,
                requestCause,
                false/*forceStandbyReassertRequest*/);
    };

    auto applyBatteryTarget = [&](
            uint16_t requestedBatteryOutput,
            bool forceStandbyReassertRequest) -> uint16_t {
        auto const currentBatteryOutput = getExpectedOutput(sBatteryPoweredFilter);
        auto const batteryControlActive = (_batteryState == BatteryState::DISCHARGE_ALLOWED
                || _batteryState == BatteryState::DISCHARGE_NIGHT)
            && (requestedBatteryOutput > 0 || currentBatteryOutput > 0);

        auto const gridChargerExpectedInput = gridChargerManaged
            ? stagedGridChargerInput
            : static_cast<uint16_t>(0);
        if (gridChargerManaged
                && requestedBatteryOutput > currentBatteryOutput
                && predictiveChargerMayBeActive()
                && gridChargerExpectedInput > 0
                && !gridChargerShutdownRequestedThisCycle) {
            if (DTU_LOG_IS_INFO && shouldLogDplGuard(
                    DplGuardLog::BatteryIncreaseBlockedByCharger)) {
                DTU_LOGI("dpl.guard.v1 guard=storage_charger_mutex "
                        "action=block_battery_increase requested_w=%u current_w=%u "
                        "charger_active=1",
                        static_cast<unsigned>(requestedBatteryOutput),
                        static_cast<unsigned>(currentBatteryOutput));
            }
            DTU_LOGD("blocking battery-powered inverter request of %u W because grid charger "
                    "input is still active or pending-off",
                    requestedBatteryOutput);
            requestedBatteryOutput = currentBatteryOutput;
        }

        auto powerBusUsage = calcPowerBusUsage(requestedBatteryOutput);
        auto const batteryRfPending = [&]() -> bool {
            if (!_predictiveModelInitialized) { return false; }

            for (auto const& request : _predictiveModel.ledger()) {
                if (!request.isPhysicalPending()) { continue; }

                auto const* actuator =
                    _predictiveModel.findActuator(request.actuatorKey);
                if (actuator != nullptr
                        && actuator->kind == Predictive::ActuatorKind::Battery) {
                    return true;
                }
            }
            return false;
        }();
        auto const plannerGridOutsideCorrectionThreshold =
            plannerGridEstimate.valid
            && (plannerGridOutsideHysteresis
                || (plannerGridChangeThresholdWatts <= 0.0f
                    && plannerGridDeviationWatts > 0.0f));
        auto const batteryPendingCorrectionAllowed =
            plannerBootstrapDue
            || plannerGridOutsideCorrectionThreshold
            || plannerTargetOutsideEstimate;
        if (batteryControlActive
                && batteryRfPending
                && !batteryPendingCorrectionAllowed
                && powerBusUsage != currentBatteryOutput) {
            if (DTU_LOG_IS_INFO && shouldLogDplGuard(
                    DplGuardLog::StorageBatteryCorrectionDeferredByPendingRf)) {
                DTU_LOGI("dpl.guard.v1 guard=battery_rf_pending "
                        "action=defer_storage_battery_pending_forecast "
                        "requested_w=%u current_w=%u planner_deviation_w=%.0f "
                        "planner_uncertainty_w=%.0f planner_hysteresis_w=%.0f",
                        static_cast<unsigned>(powerBusUsage),
                        static_cast<unsigned>(currentBatteryOutput),
                        plannerGridDeviationWatts,
                        plannerGridUncertaintyWatts,
                        plannerGridChangeThresholdWatts);
            }
            DTU_LOGD("keeping battery-powered inverter request at %u W because "
                    "a battery RF request is still pending and the grid "
                    "measurement is within the predictive corridor",
                    static_cast<unsigned>(currentBatteryOutput));
            powerBusUsage = currentBatteryOutput;
        }

        auto const opposingPendingBatteryRfDelta = [&]() -> float {
            if (!_predictiveModelInitialized) { return 0.0f; }

            auto const requestedDelta =
                static_cast<float>(powerBusUsage)
                - static_cast<float>(currentBatteryOutput);
            if (std::fabs(requestedDelta) <= 0.5f) { return 0.0f; }

            float maxOpposingDelta = 0.0f;
            for (auto const& request : _predictiveModel.ledger()) {
                if (!request.isPhysicalPending()) { continue; }

                auto const* actuator =
                    _predictiveModel.findActuator(request.actuatorKey);
                if (actuator == nullptr
                        || actuator->kind != Predictive::ActuatorKind::Battery) {
                    continue;
                }

                auto const pendingDelta = request.effectiveDeltaWatts;
                if (std::fabs(pendingDelta) <= 0.5f) { continue; }
                if (requestedDelta * pendingDelta >= 0.0f) { continue; }

                maxOpposingDelta = std::max(
                        maxOpposingDelta,
                        std::fabs(pendingDelta));
            }
            return maxOpposingDelta;
        }();
        if (batteryControlActive && powerBusUsage > 0) {
            auto const requestedDelta =
                static_cast<float>(powerBusUsage)
                - static_cast<float>(currentBatteryOutput);
            auto const counterTargetDelta = std::fabs(
                    requestedDelta);
            auto const pendingRfCounterThreshold =
                opposingPendingBatteryRfDelta
                + sPendingRfCounterTargetDeadbandWatts;
            if (opposingPendingBatteryRfDelta > 0.0f) {
                if (counterTargetDelta <= pendingRfCounterThreshold) {
                    if (DTU_LOG_IS_INFO && shouldLogDplGuard(
                            DplGuardLog::StorageBatteryCounterTargetDeferredByPendingRf)) {
                        DTU_LOGI("dpl.guard.v1 guard=battery_rf_pending "
                                "action=defer_storage_battery_counter_target "
                                "requested_w=%u current_w=%u pending_delta_w=%.0f "
                                "counter_delta_w=%.0f threshold_w=%.0f",
                                static_cast<unsigned>(powerBusUsage),
                                static_cast<unsigned>(currentBatteryOutput),
                                opposingPendingBatteryRfDelta,
                                counterTargetDelta,
                                pendingRfCounterThreshold);
                    }
                    DTU_LOGD("keeping battery-powered inverter request at %u W because "
                            "an opposing %.0f W RF request is still pending",
                            static_cast<unsigned>(currentBatteryOutput),
                            opposingPendingBatteryRfDelta);
                    powerBusUsage = currentBatteryOutput;
                } else {
                    auto const unexplainedCounterDelta =
                        counterTargetDelta - opposingPendingBatteryRfDelta;
                    auto const limitedCounterDelta =
                        ceilPositiveWattsToUint16(unexplainedCounterDelta);
                    auto const limitedPowerBusUsage = requestedDelta < 0.0f
                        ? currentBatteryOutput > limitedCounterDelta
                            ? static_cast<uint16_t>(
                                    currentBatteryOutput - limitedCounterDelta)
                            : static_cast<uint16_t>(0)
                        : static_cast<uint16_t>(std::min<uint32_t>(
                                static_cast<uint32_t>(currentBatteryOutput)
                                    + limitedCounterDelta,
                                std::numeric_limits<uint16_t>::max()));
                    if (DTU_LOG_IS_INFO && shouldLogDplGuard(
                            DplGuardLog::StorageBatteryCounterTargetDeferredByPendingRf)) {
                        DTU_LOGI("dpl.guard.v1 guard=battery_rf_pending "
                                "action=limit_storage_battery_counter_target "
                                "requested_w=%u limited_w=%u current_w=%u "
                                "pending_delta_w=%.0f counter_delta_w=%.0f "
                                "unexplained_delta_w=%.0f",
                                static_cast<unsigned>(powerBusUsage),
                                static_cast<unsigned>(limitedPowerBusUsage),
                                static_cast<unsigned>(currentBatteryOutput),
                                opposingPendingBatteryRfDelta,
                                counterTargetDelta,
                                unexplainedCounterDelta);
                    }
                    DTU_LOGD("limiting battery-powered inverter counter request from "
                            "%u W to %u W because %.0f W of the counter change "
                            "is still pending via RF",
                            static_cast<unsigned>(powerBusUsage),
                            static_cast<unsigned>(limitedPowerBusUsage),
                            opposingPendingBatteryRfDelta);
                    powerBusUsage = limitedPowerBusUsage;
                }
            }
        }

        auto const batteryRequestCause = _batteryState == BatteryState::STOP
            ? Predictive::RequestCause::Safety
            : Predictive::RequestCause::Normal;
        return requestInverterTarget(
                powerBusUsage,
                sBatteryPoweredFilter,
                sBatteryPoweredExpression,
                true/*allowStandby*/,
                false/*capOutputLimit*/,
                0,
                Predictive::RequestDomain::Storage,
                batteryRequestCause,
                forceStandbyReassertRequest);
    };

    auto reduceGridCharger = [&](float residualWatts,
            Predictive::RequestCause cause = Predictive::RequestCause::Normal) -> uint16_t {
        if (!gridChargerManaged) { return 0; }
        if (deferStalePlannerSample("grid_charger_reduce")) { return 0; }

        auto const gridChargerInput = stagedGridChargerInput;
        auto const requestedReduction = static_cast<uint16_t>(std::min<uint32_t>(
                gridChargerInput,
                ceilPositiveWattsToUint16(residualWatts)));

        auto const before = stagedGridChargerInput;
        stageGridChargerTarget(
                gridChargerInput > requestedReduction
                    ? static_cast<uint16_t>(gridChargerInput - requestedReduction)
                    : static_cast<uint16_t>(0),
                cause);
        auto const applied = before > stagedGridChargerInput
            ? static_cast<uint16_t>(before - stagedGridChargerInput)
            : static_cast<uint16_t>(0);
        if (applied > 0) {
            if (stagedGridChargerInput == 0 || applied >= gridChargerInput) {
                gridChargerShutdownRequestedThisCycle = true;
            }
        }
        return applied;
    };

    auto getStorageExpectedOutput = [&]() -> uint16_t {
        uint32_t total = getExpectedOutput(sBatteryPoweredFilter);
        total += getExpectedOutput(sSmartBufferPoweredFilter);
        return static_cast<uint16_t>(std::min<uint32_t>(
                total,
                std::numeric_limits<uint16_t>::max()));
    };

    auto getStorageCurrentOutput = [&]() -> uint16_t {
        uint32_t total = ceilPositiveWattsToUint16(getBatteryInvertersOutputAcWatts());
        total += getCurrentInvertersOutputAcWatts(sSmartBufferPoweredFilter);
        return static_cast<uint16_t>(std::min<uint32_t>(
                total,
                std::numeric_limits<uint16_t>::max()));
    };

    bool storageBatteryShutdownRequestedThisCycle = false;
    uint16_t storageBatteryShutdownAccountedOutputThisCycle = 0;

    auto increaseGridCharger = [&](float residualWatts) -> uint16_t {
        if (!gridChargerManaged) { return 0; }
        if (deferStalePlannerSample("grid_charger_increase")) { return 0; }

        auto const gridChargerInput = stagedGridChargerInput;
        auto const gridChargerMaxInput = GridCharger.getPowerLimiterMaxInputPowerWatts();
        auto const storageOutputActive = predictiveStorageOutputMayBeActive();
        auto const storageExpectedOutput = getStorageExpectedOutput();
        auto const storageCurrentOutput = getStorageCurrentOutput();
        auto const storageStandbyPending =
            storageBatteryShutdownRequestedThisCycle
            && storageExpectedOutput == 0
            && storageCurrentOutput <= static_cast<uint32_t>(
                    storageBatteryShutdownAccountedOutputThisCycle);
        if ((storageOutputActive && !storageStandbyPending)
                || storageExpectedOutput > 0) {
            if (DTU_LOG_IS_INFO && shouldLogDplGuard(
                    DplGuardLog::ChargerIncreaseBlockedByStorageOutput)) {
                DTU_LOGI("dpl.guard.v1 guard=storage_charger_mutex "
                        "action=block_charger_increase residual_w=%.0f "
                        "storage_output_active=%u storage_expected_w=%u "
                        "storage_current_w=%u storage_standby_pending=%u",
                        residualWatts,
                        storageOutputActive ? 1 : 0,
                        static_cast<unsigned>(storageExpectedOutput),
                        static_cast<unsigned>(storageCurrentOutput),
                        storageStandbyPending ? 1 : 0);
            }
            if (residualWatts > 0.0f) {
                GridCharger.setPowerLimiterLimitedByAvailablePower(
                        Predictive::gridChargerAvailablePowerLimitedForFlexibleLoad(
                                Predictive::CorrectionAction::LessEffectiveOutput,
                                gridChargerInput,
                                gridChargerMaxInput));
            }
            DTU_LOGD("not increasing grid charger input because storage output is still "
                    "active without a same-cycle battery standby request or expected on");
            return 0;
        }
        if (storageStandbyPending) {
            DTU_LOGD("increasing grid charger input while battery-powered inverter "
                    "target is 0 W (%u W storage output still measured)",
                    static_cast<unsigned>(storageCurrentOutput));
        }

        if (gridChargerMaxInput <= gridChargerInput) { return 0; }

        auto const requestedIncrease = static_cast<uint16_t>(std::min<uint32_t>(
                gridChargerMaxInput - gridChargerInput,
                ceilPositiveWattsToUint16(residualWatts)));

        auto const before = stagedGridChargerInput;
        stageGridChargerTarget(static_cast<uint16_t>(std::min<uint32_t>(
                static_cast<uint32_t>(gridChargerInput) + requestedIncrease,
                std::numeric_limits<uint16_t>::max())));
        auto const applied = stagedGridChargerInput > before
            ? static_cast<uint16_t>(stagedGridChargerInput - before)
            : static_cast<uint16_t>(0);
        return applied;
    };

    auto storageIsInsideCorridor = [&]() {
        return getStorageDecision().action == Predictive::CorrectionAction::Hold;
    };

    auto batteryPowerStateTransitionActive = [&]() -> bool {
        if (!_predictiveModelInitialized) { return false; }

        for (auto const& upInv : _inverters) {
            if (!upInv->isBatteryPowered()) { continue; }
            if (_predictiveModel.hasActivePowerStateTransition(
                    predictiveInverterKey(*upInv))) {
                return true;
            }
        }
        return false;
    };

    auto resolveStorageChargerOverlap = [&]() {
        if (!gridChargerManaged || !PowerMeter.isDataValid()) { return; }
        if (deferStalePlannerSample("storage_charger_overlap")) { return; }
        if (batteryPowerStateTransitionActive()) { return; }

        auto const gridChargerInput = stagedGridChargerInput;
        if (gridChargerInput <= sBatteryStandbyReassertThresholdWatts) { return; }

        auto const batteryExpectedOutput = getExpectedOutput(sBatteryPoweredFilter);
        auto const batteryCurrentOutput =
            ceilPositiveWattsToUint16(getBatteryInvertersOutputAcWatts());
        auto const batteryOverlapOutput =
            std::max(batteryExpectedOutput, batteryCurrentOutput);
        if (batteryOverlapOutput <= sBatteryStandbyReassertThresholdWatts) { return; }

        auto const overlap = static_cast<uint16_t>(std::min<uint32_t>(
                gridChargerInput,
                batteryOverlapOutput));
        if (overlap <= sBatteryStandbyReassertThresholdWatts) { return; }

        auto const chargerTarget = gridChargerInput > overlap
            ? static_cast<uint16_t>(gridChargerInput - overlap)
            : static_cast<uint16_t>(0);
        auto const batteryTarget = batteryExpectedOutput > overlap
            ? static_cast<uint16_t>(batteryExpectedOutput - overlap)
            : static_cast<uint16_t>(0);

        auto const chargerReduction = stageGridChargerTarget(chargerTarget);
        auto const covered = applyBatteryTarget(
                batteryTarget,
                batteryTarget == 0/*forceStandbyReassertRequest*/);
        if (chargerReduction == 0 && covered == batteryExpectedOutput) { return; }

        if (DTU_LOG_IS_INFO && shouldLogDplGuard(
                DplGuardLog::StorageChargerOverlapResolved)) {
            DTU_LOGI("dpl.guard.v1 guard=storage_charger_mutex "
                    "action=resolve_storage_charger_overlap "
                    "overlap_w=%u charger_before_w=%u charger_target_w=%u "
                    "battery_expected_w=%u battery_current_w=%u battery_target_w=%u",
                    static_cast<unsigned>(overlap),
                    static_cast<unsigned>(gridChargerInput),
                    static_cast<unsigned>(stagedGridChargerInput),
                    static_cast<unsigned>(batteryExpectedOutput),
                    static_cast<unsigned>(batteryCurrentOutput),
                    static_cast<unsigned>(covered));
        }
    };

    auto regulateStorage = [&]() {
        auto decision = getStorageDecision();
        if (!_lastPredictiveStorageForecast.valid) {
            return;
        }

        if (decision.action == Predictive::CorrectionAction::MoreEffectiveOutput) {
            increaseInverterOutput(
                    sSmartBufferPoweredFilter,
                    sSmartBufferPoweredExpression,
                    decision.residualEffectiveWatts,
                    Predictive::RequestDomain::Storage);
            if (storageIsInsideCorridor()) {
                return;
            }

            decision = getStorageDecision();
            reduceGridCharger(decision.residualEffectiveWatts);
            if (storageIsInsideCorridor()) {
                return;
            }

            decision = getStorageDecision();
            auto const currentBatteryOutput = getExpectedOutput(sBatteryPoweredFilter);
            auto const increase = outputIncreaseWithinTotalLimit(decision.residualEffectiveWatts);
            if (increase > 0) {
                applyBatteryTarget(static_cast<uint16_t>(std::min<uint32_t>(
                        static_cast<uint32_t>(currentBatteryOutput) + increase,
                        std::numeric_limits<uint16_t>::max())),
                        false/*forceStandbyReassertRequest*/);
            }
            return;
        }

        if (decision.action == Predictive::CorrectionAction::LessEffectiveOutput) {
            auto residual = -decision.residualEffectiveWatts;

            auto const currentBatteryOutput = getExpectedOutput(sBatteryPoweredFilter);
            if (currentBatteryOutput > 0) {
                auto const reduction = ceilPositiveWattsToUint16(residual);
                auto const target = currentBatteryOutput > reduction
                    ? static_cast<uint16_t>(currentBatteryOutput - reduction)
                    : 0;
                auto const covered = applyBatteryTarget(
                        target,
                        false/*forceStandbyReassertRequest*/);
                if (target < currentBatteryOutput) {
                    if (target == 0 && covered == 0) {
                        storageBatteryShutdownRequestedThisCycle = true;
                        storageBatteryShutdownAccountedOutputThisCycle =
                            std::max(storageBatteryShutdownAccountedOutputThisCycle,
                                    currentBatteryOutput);
                        DTU_LOGD("continuing storage backoff after battery-powered "
                                "inverter target reached 0 W");
                    } else {
                        DTU_LOGD("deferring further storage backoff until battery-powered "
                                "inverter reduction has been dispatched");
                        return;
                    }
                }
                if (!storageBatteryShutdownRequestedThisCycle
                        && storageIsInsideCorridor()) {
                    return;
                }
            }

            decision = getStorageDecision();
            residual = -decision.residualEffectiveWatts;
            reduceInverterOutput(
                    sSmartBufferPoweredFilter,
                    sSmartBufferPoweredExpression,
                    residual,
                    Predictive::RequestDomain::Storage);
            if (!storageBatteryShutdownRequestedThisCycle
                    && storageIsInsideCorridor()) {
                return;
            }

            decision = getStorageDecision();
            residual = -decision.residualEffectiveWatts;
            increaseGridCharger(residual);
            if (storageIsInsideCorridor()) {
                return;
            }
        }
    };

    Predictive::ControlForecast solarTargetForecastBeforeStorage;
    float gridChargerMeterDeltaBeforeStorageWatts = 0.0f;

    auto regulateSolarTarget = [&]() {
        auto solarTargetForecast = _lastPredictiveGlobalForecast;
        auto const sameStepGridChargerMeterDeltaWatts =
            gridChargerPlannedMeterDeltaWatts()
            - gridChargerMeterDeltaBeforeStorageWatts;
        auto const sameStepStorageMeterDeltaWatts =
            sameStepStorageInverterMeterDeltaWatts
            + sameStepGridChargerMeterDeltaWatts;
        if (solarTargetForecastBeforeStorage.valid
                && std::fabs(sameStepStorageMeterDeltaWatts) > 0.001f) {
            solarTargetForecast = Predictive::forecastWithPlannedMeterDelta(
                    solarTargetForecastBeforeStorage,
                    sameStepStorageMeterDeltaWatts);
        }
        if (!solarTargetForecast.valid) {
            return;
        }

        auto decision = Predictive::decideSolarTargetCorrection(
                targetPowerConsumption,
                solarTargetForecast,
                0.0f,
                smallCorrectionDampingThresholdWatts);
        if (decision.action == Predictive::CorrectionAction::Hold) {
            return;
        }

        if (decision.action == Predictive::CorrectionAction::MoreEffectiveOutput) {
            increaseInverterOutput(
                    sSolarPoweredFilter,
                    sSolarPoweredExpression,
                    decision.residualEffectiveWatts,
                    Predictive::RequestDomain::Global);
            return;
        }

        auto residual = -decision.residualEffectiveWatts;
        reduceInverterOutput(
                sSolarPoweredFilter,
                sSolarPoweredExpression,
                residual,
                Predictive::RequestDomain::Global);
    };

    if (gridChargerManaged) {
        auto const gridChargerExpectedInput = stagedGridChargerInput;
        auto const gridChargerMaxInput = GridCharger.getPowerLimiterMaxInputPowerWatts();
        if (gridChargerExpectedInput > gridChargerMaxInput) {
            auto const appliedReduction = reduceGridCharger(
                    gridChargerExpectedInput - gridChargerMaxInput);
            if (appliedReduction > 0
                    && DTU_LOG_IS_INFO
                    && shouldLogDplGuard(DplGuardLog::GridChargerSocPlanCap)) {
                DTU_LOGI("dpl.guard.v1 guard=soc_plan "
                        "action=cap_grid_charger_to_planned_limit "
                        "expected_w=%u max_w=%u reduction_w=%u",
                        static_cast<unsigned>(gridChargerExpectedInput),
                        static_cast<unsigned>(gridChargerMaxInput),
                        static_cast<unsigned>(appliedReduction));
            }
        }
    }

    if (PowerMeter.isDataValid()) {
        solarTargetForecastBeforeStorage = _lastPredictiveGlobalForecast;
        gridChargerMeterDeltaBeforeStorageWatts =
            gridChargerPlannedMeterDeltaWatts();
        sameStepStorageInverterMeterDeltaWatts = 0.0f;
        regulateStorage();
        if (!plannerPowerMeterSampleChanged) {
            refreshPredictiveForecasts();
            regulateSolarTarget();
            refreshPredictiveForecasts();
        }
        resolveStorageChargerOverlap();
    } else if (gridChargerManaged) {
        auto const appliedReduction = reduceGridCharger(
                stagedGridChargerInput,
                Predictive::RequestCause::Safety);
        if (appliedReduction > 0) {
            if (DTU_LOG_IS_INFO && shouldLogDplGuard(DplGuardLog::InvalidMeterGridChargerShed)) {
                DTU_LOGI("dpl.guard.v1 guard=meter_validity "
                        "action=shed_grid_charger reduction_w=%u",
                        static_cast<unsigned>(appliedReduction));
            }
            DTU_LOGD("turning grid charger down by %u W because power meter data is invalid",
                    appliedReduction);
        }
    }

    if (!controlUpdated && !plannerPowerMeterSampleChanged) {
        requestInverterTarget(
                getExpectedOutput(sSolarPoweredFilter),
                sSolarPoweredFilter,
                sSolarPoweredExpression,
                true/*allowStandby*/,
                true/*capOutputLimit*/,
                solarOutputLimitHeadroomWatts,
                Predictive::RequestDomain::Global,
                Predictive::RequestCause::Normal,
                false/*forceStandbyReassertRequest*/);
        requestInverterTarget(
                getExpectedOutput(sSmartBufferPoweredFilter),
                sSmartBufferPoweredFilter,
                sSmartBufferPoweredExpression,
                true/*allowStandby*/,
                false/*capOutputLimit*/,
                0,
                Predictive::RequestDomain::Storage,
                Predictive::RequestCause::Normal,
                false/*forceStandbyReassertRequest*/);
        requestInverterTarget(
                getExpectedOutput(sBatteryPoweredFilter),
                sBatteryPoweredFilter,
                sBatteryPoweredExpression,
                true/*allowStandby*/,
                false/*capOutputLimit*/,
                0,
                Predictive::RequestDomain::Storage,
                Predictive::RequestCause::Normal,
                false/*forceStandbyReassertRequest*/);
    }

    if (gridChargerManaged && !plannerPowerMeterSampleChanged) {
        auto const gridChargerMaxInput = GridCharger.getPowerLimiterMaxInputPowerWatts();
        auto const gridChargerExpectedInput = stagedGridChargerInput;
        GridCharger.setPowerLimiterLimitedByAvailablePower(
                Predictive::gridChargerAvailablePowerLimitedForFlexibleLoad(
                        getStorageDecision().action,
                        gridChargerExpectedInput,
                        gridChargerMaxInput));
    }

    if (stagedGridChargerTargetChanged) {
        commitStagedGridChargerTargetIfNeeded();
    }

    for (auto const &upInv : _inverters) { upInv->debug(); }

    _lastExpectedInverterOutput = getExpectedTotalOutput();
    if (!plannerPowerMeterSampleChanged) {
        markRegulationSampleProcessed();
    }

    bool limitUpdated = updateInverters() || gridChargerLimitUpdated;

    _lastCalculation = millis();

    if (!limitUpdated) {
        // increase polling backoff if system seems to be stable
        _calculationBackoffMs = std::min<uint32_t>(1024, _calculationBackoffMs * 2);
        recordTraceSample(Status::Stable);
        return announceStatus(Status::Stable);
    }

    _calculationBackoffMs = _calculationBackoffMsDefault;
    recordTraceSample(Status::Stable);
    announceStatus(Status::Stable);
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

float PowerLimiterClass::estimateBatteryDischargeCurrent() const
{
    auto const& config = Configuration.get();
    auto const batteryStats = Battery.getStats();
    if (config.Battery.Enabled
            && batteryStats->isVoltageValid()
            && batteryStats->isCurrentValid()
            && batteryStats->getVoltageAgeSeconds() < 60
            && batteryStats->getChargeCurrentAgeSeconds() < 60) {
        return std::max(0.0f, -batteryStats->getChargeCurrent());
    }

    auto const voltage = getBatteryVoltage();
    if (voltage <= 0.0f) { return 0.0f; }

    float const lossesFactor = 1.00f
        - static_cast<float>(config.PowerLimiter.ConductionLosses) / 100.0f;
    float const conversionFactor = 0.95f * lossesFactor;
    if (conversionFactor <= 0.0f) { return 0.0f; }

    float const inverterDcDemand =
        getBatteryInvertersOutputAcWatts() / conversionFactor;
    float const batteryDcPower = std::max(
            0.0f,
            inverterDcDemand - static_cast<float>(getSolarPassthroughPower()));
    return batteryDcPower / voltage;
}

void PowerLimiterClass::resetBatteryDischargeCurrentLimitBudget()
{
    _batteryDischargeCurrentLimitBudgetAmpSeconds = 0.0f;
    _batteryDischargeCurrentLimitBudgetCapacityAmpSeconds = 0.0f;
    _lastBatteryDischargeCurrentLimitBudgetUpdateMillis = 0;
    _batteryDischargeCurrentLimitBudgetInitialized = false;
    _lastBatteryDischargeCurrentEstimateAmps = 0.0f;
    _lastBatteryDischargeCurrentLimitAmps = 0.0f;
    _lastBatteryDischargeCurrentLimitBudgetUsedPermille = 0;
}

std::optional<float> PowerLimiterClass::getConfiguredBatteryDischargeCurrentLimit()
{
    auto const& config = Configuration.get();
    auto const& limiter = config.PowerLimiter;
    if (!limiter.BatteryDischargeCurrentLimitEnabled) {
        resetBatteryDischargeCurrentLimitBudget();
        return std::nullopt;
    }

    float const continuousLimit = std::max(
            0.0f,
            limiter.BatteryDischargeCurrentLimit);
    if (continuousLimit <= 0.0f) {
        resetBatteryDischargeCurrentLimitBudget();
        _lastBatteryDischargeCurrentLimitAmps = 0.0f;
        return 0.0f;
    }

    float const peakLimit = std::max(
            continuousLimit,
            limiter.BatteryDischargeCurrentPeakLimit);
    uint16_t const peakDuration = limiter.BatteryDischargeCurrentPeakDuration;
    uint16_t const recoveryDuration =
        limiter.BatteryDischargeCurrentRecoveryDuration;

    _lastBatteryDischargeCurrentEstimateAmps =
        estimateBatteryDischargeCurrent();

    if (peakLimit <= continuousLimit || peakDuration == 0) {
        _batteryDischargeCurrentLimitBudgetAmpSeconds = 0.0f;
        _batteryDischargeCurrentLimitBudgetCapacityAmpSeconds = 0.0f;
        _batteryDischargeCurrentLimitBudgetInitialized = true;
        _lastBatteryDischargeCurrentLimitBudgetUpdateMillis = millis();
        _lastBatteryDischargeCurrentLimitAmps = continuousLimit;
        _lastBatteryDischargeCurrentLimitBudgetUsedPermille = 0;
        return continuousLimit;
    }

    float const peakDurationSeconds = static_cast<float>(peakDuration);
    float const budgetCapacity =
        PowerLimiterBatteryCurrentLimit::budgetCapacityAmpSeconds(
                continuousLimit,
                peakLimit,
                peakDurationSeconds);
    auto const now = millis();
    if (!_batteryDischargeCurrentLimitBudgetInitialized
            || std::fabs(_batteryDischargeCurrentLimitBudgetCapacityAmpSeconds
                - budgetCapacity) > 0.001f) {
        _batteryDischargeCurrentLimitBudgetCapacityAmpSeconds = budgetCapacity;
        _batteryDischargeCurrentLimitBudgetAmpSeconds = budgetCapacity;
        _lastBatteryDischargeCurrentLimitBudgetUpdateMillis = now;
        _batteryDischargeCurrentLimitBudgetInitialized = true;
    } else {
        auto const elapsedMillis = now
            - _lastBatteryDischargeCurrentLimitBudgetUpdateMillis;
        if (elapsedMillis > 0) {
            float const elapsedSeconds =
                static_cast<float>(elapsedMillis) / 1000.0f;
            float const dischargeCurrent =
                _lastBatteryDischargeCurrentEstimateAmps;
            if (dischargeCurrent > continuousLimit) {
                _batteryDischargeCurrentLimitBudgetAmpSeconds -=
                    (dischargeCurrent - continuousLimit) * elapsedSeconds;
            } else if (recoveryDuration == 0) {
                _batteryDischargeCurrentLimitBudgetAmpSeconds = budgetCapacity;
            } else {
                float const recoveryScale = std::clamp(
                        (continuousLimit - dischargeCurrent) / continuousLimit,
                        0.0f,
                        1.0f);
                _batteryDischargeCurrentLimitBudgetAmpSeconds +=
                    (budgetCapacity / static_cast<float>(recoveryDuration))
                    * recoveryScale
                    * elapsedSeconds;
            }

            _batteryDischargeCurrentLimitBudgetAmpSeconds = std::clamp(
                    _batteryDischargeCurrentLimitBudgetAmpSeconds,
                    0.0f,
                    budgetCapacity);
            _lastBatteryDischargeCurrentLimitBudgetUpdateMillis = now;
        }
    }

    _lastBatteryDischargeCurrentLimitAmps =
        PowerLimiterBatteryCurrentLimit::currentLimitAmps(
                continuousLimit,
                peakLimit,
                peakDurationSeconds,
                _batteryDischargeCurrentLimitBudgetAmpSeconds);
    _lastBatteryDischargeCurrentLimitBudgetUsedPermille =
        static_cast<uint16_t>(std::clamp<float>(
                std::round((1.0f
                    - (_batteryDischargeCurrentLimitBudgetAmpSeconds
                        / budgetCapacity)) * 1000.0f),
                0.0f,
                1000.0f));

    return _lastBatteryDischargeCurrentLimitAmps;
}

bool PowerLimiterClass::isFlexibleLoadEmergencyStopActive() const
{
    auto const& config = Configuration.get();
    if (!config.PowerLimiter.FlexibleLoadEmergencyStopEnabled
            || config.PowerLimiter.FlexibleLoadEmergencyStopGridPowerLimit == 0) {
        return false;
    }
    if (!PowerMeter.isDataValid()) { return false; }

    return PowerMeter.getPowerTotal()
        >= static_cast<float>(config.PowerLimiter.FlexibleLoadEmergencyStopGridPowerLimit);
}

bool PowerLimiterClass::shouldRequestEmergencyFullOutput()
{
    auto const now = millis();
    if (!_emergencyFullOutputActive) {
        _emergencyFullOutputActive = true;
        _lastEmergencyFullOutputRequestMillis = now;
        return true;
    }

    if ((now - _lastEmergencyFullOutputRequestMillis) < _emergencyFullOutputReassertIntervalMs) {
        return false;
    }

    _lastEmergencyFullOutputRequestMillis = now;
    return true;
}

void PowerLimiterClass::resetEmergencyFullOutputThrottle()
{
    _emergencyFullOutputActive = false;
    _lastEmergencyFullOutputRequestMillis = 0;
}

void PowerLimiterClass::emergencyFullOutput()
{
    bool gridChargerLimitUpdated = false;
    bool const gridChargerManaged = isGridChargerManaged()
        && GridCharger.supportsPowerLimiterControl();
    auto const predictiveModelWasInitialized = _predictiveModelInitialized;
    if (predictiveModelWasInitialized) {
        processPredictiveGridChargerDispatchEvents();
    }
    syncPredictiveActuators(gridChargerManaged);
    if (!predictiveModelWasInitialized) {
        processPredictiveGridChargerDispatchEvents();
    }
    advancePredictiveLedger();

    if (gridChargerManaged) {
        auto const gridChargerInput = GridCharger.getPowerLimiterExpectedInputPowerWatts();
        auto const appliedReduction =
            applyGridChargerInputPowerReduction(
                    gridChargerInput,
                    Predictive::RequestCause::Safety);
        if (appliedReduction > 0) {
            gridChargerLimitUpdated = true;
            if (DTU_LOG_IS_INFO && shouldLogDplGuard(DplGuardLog::EmergencyGridChargerShed)) {
                DTU_LOGI("dpl.guard.v1 guard=emergency_full_output "
                        "action=shed_grid_charger reduction_w=%u",
                        static_cast<unsigned>(appliedReduction));
            }
            DTU_LOGI("emergency grid consumption: turning grid charger down by %u W",
                    appliedReduction);
        }
        GridCharger.setPowerLimiterLimitedByAvailablePower(false);
    }

    bool const batteryOutputAllowed =
        _batteryState == BatteryState::DISCHARGE_ALLOWED
        || _batteryState == BatteryState::DISCHARGE_NIGHT;

    for (auto& upInv : _inverters) {
        if (!upInv->isEligible()) { continue; }

        auto const before = upInv->getExpectedOutputAcWatts();
        if (upInv->isSolarPowered() || upInv->isSmartBufferPowered()) {
            upInv->setMaxOutput(true);
        } else if (upInv->isBatteryPowered()) {
            if (!batteryOutputAllowed) {
                upInv->standby();
            }
        }
        auto const after = upInv->getExpectedOutputAcWatts();
        if (after != before) {
            recordPredictiveInverterRequest(
                    *upInv,
                    after,
                    upInv->isSolarPowered()
                        ? Predictive::RequestDomain::Global
                        : Predictive::RequestDomain::Storage,
                    Predictive::RequestCause::Safety);
        }
    }

    if (batteryOutputAllowed) {
        auto const target = calcPowerBusUsage(
                std::numeric_limits<uint16_t>::max());
        updateInverterLimits(
                target,
                sBatteryPoweredFilter,
                sBatteryPoweredExpression,
                true/*allowStandby*/,
                false/*capOutputLimit*/,
                0,
                Predictive::RequestDomain::Storage,
                Predictive::RequestCause::Safety);
    }

    uint16_t expectedOutput = 0;
    for (auto& upInv : _inverters) {
        if (!upInv->isEligible()) { continue; }
        expectedOutput += upInv->getExpectedOutputAcWatts();
    }

    _lastExpectedInverterOutput = expectedOutput;
    rememberRegulationOutcome(0.0f, false);
    updateInverters(false/*throttleRfQueue*/);
    _lastCalculation = millis();
    _calculationBackoffMs = _calculationBackoffMsDefault;

    (void)gridChargerLimitUpdated;
    recordTraceSample(Status::EmergencyFullOutput);
    return announceStatus(Status::EmergencyFullOutput);
}

/**
 * implements the "uncoditional full solar passthrough" mode of operation. in this mode of
 * operation, the inverters shall behave as if they were connected to the solar
 * panels directly, i.e., all solar power (and only solar power) is converted
 * to AC power, independent from the power meter reading.
 */
void PowerLimiterClass::unconditionalFullSolarPassthrough()
{
    resetRegulationState();
    bool const gridChargerManaged = isGridChargerManaged()
        && GridCharger.supportsPowerLimiterControl();
    syncPredictiveActuators(gridChargerManaged);
    processPredictiveGridChargerDispatchEvents();
    advancePredictiveLedger();

    auto now = millis();
    if ((now - _lastCalculation) < _calculationBackoffMs) {
        recordTraceSample(Status::UnconditionalSolarPassthrough);
        return;
    }
    _lastCalculation = now;

    for (auto const& upInv : _inverters) {
        if (!upInv->isEligible()) { continue; }
        if (!upInv->isBatteryPowered()) {
            auto const before = upInv->getExpectedOutputAcWatts();
            upInv->setMaxOutput();
            auto const after = upInv->getExpectedOutputAcWatts();
            if (after != before) {
                recordPredictiveInverterRequest(
                        *upInv,
                        after,
                        upInv->isSolarPowered()
                            ? Predictive::RequestDomain::Global
                            : Predictive::RequestDomain::Storage,
                        Predictive::RequestCause::Manual);
            }
        }
    }

    uint16_t targetOutput = 0;

    auto solarChargerOutput = SolarCharger.getStats()->getOutputPowerWatts();
    if (solarChargerOutput) {
        targetOutput = static_cast<uint16_t>(std::max<int32_t>(0, *solarChargerOutput));
        targetOutput = dcPowerBusToInverterAc(targetOutput);
    }

    _calculationBackoffMs = 1 * 1000;
    updateInverterLimits(targetOutput, sBatteryPoweredFilter, sBatteryPoweredExpression);
    recordTraceSample(Status::UnconditionalSolarPassthrough);
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
    resetDynamicBatteryTargetOracleProbes();
}

void PowerLimiterClass::resetDynamicBatteryTargetOracleProbes()
{
    for (auto& probe : _dynamicBatteryTargetOracleProbes) {
        probe = DynamicBatteryTargetOracleProbe();
    }
    _dynamicBatteryTargetOracleProbeWrite = 0;
    _lastDynamicBatteryTargetOracleForecastValid = false;
    _lastDynamicBatteryTargetOracleForecast = 0.0f;
    _lastDynamicBatteryTargetOracleForecastMillis = 0;
    _lastDynamicBatteryTargetOracleForecastOriginMillis = 0;
    _lastDynamicBatteryTargetDueForecastValid = false;
    _lastDynamicBatteryTargetDueForecastMin = 0;
    _lastDynamicBatteryTargetDueForecastNominal = 0;
    _lastDynamicBatteryTargetDueForecastMax = 0;
    _lastDynamicBatteryTargetDueForecastMillis = 0;
    _lastDynamicBatteryTargetDueForecastOriginMillis = 0;
}

PowerLimiterClass::DynamicBatteryTargetForecastState
PowerLimiterClass::currentDynamicBatteryTargetForecastState() const
{
    DynamicBatteryTargetForecastState state;
    state.initialized = _dynamicBatteryTargetInitialized;
    state.updateMillis = _lastDynamicBatteryTargetPowerMeterUpdate;
    state.mean = _dynamicBatteryTargetMean;
    state.variance = _dynamicBatteryTargetVariance;
    state.target = _batteryTargetPowerConsumption;
    return state;
}

void PowerLimiterClass::advanceDynamicBatteryTargetForecastState(
        DynamicBatteryTargetForecastState& state,
        float meterValue,
        uint32_t updateMillis) const
{
    auto const& config = Configuration.get();

    if (!config.PowerLimiter.BatteryTargetPowerConsumptionDynamicEnabled
            || config.PowerLimiter.BatteryTargetPowerConsumptionDynamicWindow == 0) {
        state = DynamicBatteryTargetForecastState();
        state.target = config.PowerLimiter.BatteryTargetPowerConsumption;
        state.updateMillis = updateMillis;
        return;
    }

    auto const watts = std::clamp<float>(
            meterValue - static_cast<float>(config.PowerLimiter.TargetPowerConsumption),
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max());

    if (!state.initialized) {
        state.mean = watts;
        state.variance = 0.0f;
        state.initialized = true;
        state.updateMillis = updateMillis;
        return;
    }

    auto const alpha = calcExponentialSmoothingAlpha(
            updateMillis - state.updateMillis,
            config.PowerLimiter.BatteryTargetPowerConsumptionDynamicWindow);
    state.updateMillis = updateMillis;
    if (alpha <= 0.0f) { return; }

    auto const previousMean = state.mean;
    auto const deviation = watts - previousMean;
    state.mean = previousMean + (alpha * deviation);
    state.variance = (1.0f - alpha)
        * (state.variance + (alpha * deviation * deviation));

    auto const calculatedTarget = calcBatteryTargetPowerConsumption(
            state.variance,
            state.initialized);
    state.target = std::clamp<float>(
            state.target + (alpha * (calculatedTarget - state.target)),
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max());
}

void PowerLimiterClass::advanceDynamicBatteryTargetOracleProbes(
        float meterValue,
        uint32_t powerMeterUpdate)
{
    bool hasMaturedProbe = false;
    float maturedTarget = 0.0f;
    bool maturedForecastValid = false;
    int16_t maturedForecastMin = 0;
    int16_t maturedForecastNominal = 0;
    int16_t maturedForecastMax = 0;
    uint32_t maturedOriginMillis = 0;
    uint32_t maturedTargetMillis = 0;

    for (auto& probe : _dynamicBatteryTargetOracleProbes) {
        if (!probe.active) { continue; }

        advanceDynamicBatteryTargetForecastState(
                probe.state,
                meterValue,
                powerMeterUpdate);

        if (!millisAtOrAfter(powerMeterUpdate, probe.targetMillis)) { continue; }

        if (!hasMaturedProbe || millisAtOrAfter(probe.targetMillis, maturedTargetMillis)) {
            hasMaturedProbe = true;
            maturedTarget = probe.state.target;
            maturedForecastValid = probe.forecastValid;
            maturedForecastMin = probe.forecastMin;
            maturedForecastNominal = probe.forecastNominal;
            maturedForecastMax = probe.forecastMax;
            maturedOriginMillis = probe.originMillis;
            maturedTargetMillis = probe.targetMillis;
        }
        probe.active = false;
    }

    if (!hasMaturedProbe) { return; }

    _lastDynamicBatteryTargetOracleForecastValid = true;
    _lastDynamicBatteryTargetOracleForecast = maturedTarget;
    _lastDynamicBatteryTargetOracleForecastMillis = powerMeterUpdate;
    _lastDynamicBatteryTargetOracleForecastOriginMillis = maturedOriginMillis;
    _lastDynamicBatteryTargetDueForecastValid = maturedForecastValid;
    _lastDynamicBatteryTargetDueForecastMin = maturedForecastMin;
    _lastDynamicBatteryTargetDueForecastNominal = maturedForecastNominal;
    _lastDynamicBatteryTargetDueForecastMax = maturedForecastMax;
    _lastDynamicBatteryTargetDueForecastMillis = powerMeterUpdate;
    _lastDynamicBatteryTargetDueForecastOriginMillis = maturedOriginMillis;
}

void PowerLimiterClass::enqueueDynamicBatteryTargetOracleProbe(uint32_t powerMeterUpdate)
{
    if (!_dynamicBatteryTargetInitialized
            || _dynamicBatteryTargetOracleProbeCapacity == 0) {
        return;
    }

    auto& probe = _dynamicBatteryTargetOracleProbes[
        _dynamicBatteryTargetOracleProbeWrite];
    probe.active = true;
    probe.originMillis = powerMeterUpdate;
    probe.targetMillis = powerMeterUpdate + sBatteryTargetEffectAssumptionMillis;
    probe.state = currentDynamicBatteryTargetForecastState();
    probe.forecastValid = calcDynamicStorageTargetForecastRange(
            _lastPredictiveGlobalForecast,
            probe.state,
            probe.targetMillis,
            probe.forecastMin,
            probe.forecastNominal,
            probe.forecastMax);

    _dynamicBatteryTargetOracleProbeWrite =
        (_dynamicBatteryTargetOracleProbeWrite + 1)
        % _dynamicBatteryTargetOracleProbeCapacity;
}

bool PowerLimiterClass::calcDynamicStorageTargetForecastRange(
        Predictive::ControlForecast const& gridForecast,
        DynamicBatteryTargetForecastState const& baseState,
        uint32_t targetMillis,
        int16_t& min,
        int16_t& nominal,
        int16_t& max) const
{
    if (!gridForecast.valid || !baseState.initialized) { return false; }

    auto forecastValue = [&](float gridWatts) {
        auto state = baseState;
        advanceDynamicBatteryTargetForecastState(state, gridWatts, targetMillis);
        return state.target;
    };

    auto const minTarget = forecastValue(gridForecast.gridMinWatts);
    auto const nominalTarget = forecastValue(gridForecast.gridNominalWatts);
    auto const maxTarget = forecastValue(gridForecast.gridMaxWatts);
    auto const low = std::min(minTarget, std::min(nominalTarget, maxTarget));
    auto const high = std::max(minTarget, std::max(nominalTarget, maxTarget));

    min = clampTraceWatts(low);
    nominal = clampTraceWatts(nominalTarget);
    max = clampTraceWatts(high);
    return true;
}

PowerLimiterClass::TraceStorageTargetForecastSample
PowerLimiterClass::calcDynamicStorageTargetForecast(
        Predictive::ControlForecast const& gridForecast,
        uint32_t nowMillis) const
{
    TraceStorageTargetForecastSample result;
    result.horizonMillis = static_cast<uint16_t>(std::min<uint32_t>(
            sBatteryTargetEffectAssumptionMillis,
            std::numeric_limits<uint16_t>::max()));

    auto const& config = Configuration.get();
    if (!config.PowerLimiter.BatteryTargetPowerConsumptionDynamicEnabled
            || config.PowerLimiter.BatteryTargetPowerConsumptionDynamicWindow == 0
            || !_dynamicBatteryTargetInitialized) {
        return result;
    }

    result.valid = calcDynamicStorageTargetForecastRange(
            gridForecast,
            currentDynamicBatteryTargetForecastState(),
            nowMillis + sBatteryTargetEffectAssumptionMillis,
            result.min,
            result.nominal,
            result.max);

    auto const oracleSampleAgeMillis =
        nowMillis - _lastDynamicBatteryTargetOracleForecastMillis;
    auto const dueSampleAgeMillis =
        nowMillis - _lastDynamicBatteryTargetDueForecastMillis;
    if (_lastDynamicBatteryTargetDueForecastValid
            && dueSampleAgeMillis <= sBatteryTargetEffectAssumptionMillis) {
        result.dueValid = true;
        result.dueMin = _lastDynamicBatteryTargetDueForecastMin;
        result.dueNominal = _lastDynamicBatteryTargetDueForecastNominal;
        result.dueMax = _lastDynamicBatteryTargetDueForecastMax;
        result.dueAgeMillis = static_cast<uint16_t>(std::min<uint32_t>(
                nowMillis - _lastDynamicBatteryTargetDueForecastOriginMillis,
                std::numeric_limits<uint16_t>::max()));
    }
    if (_lastDynamicBatteryTargetOracleForecastValid
            && oracleSampleAgeMillis <= sBatteryTargetEffectAssumptionMillis) {
        result.oracleValid = true;
        result.oracle = clampTraceWatts(_lastDynamicBatteryTargetOracleForecast);
        result.oracleAgeMillis = static_cast<uint16_t>(std::min<uint32_t>(
                nowMillis - _lastDynamicBatteryTargetOracleForecastOriginMillis,
                std::numeric_limits<uint16_t>::max()));
    }

    return result;
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

    auto const meterValue = getRegulationPowerMeterTotal();
    advanceDynamicBatteryTargetOracleProbes(meterValue, powerMeterUpdate);

    if (!_dynamicBatteryTargetInitialized) {
        _dynamicBatteryTargetMean = std::clamp<float>(
                meterValue - static_cast<float>(config.PowerLimiter.TargetPowerConsumption),
                std::numeric_limits<int16_t>::min(),
                std::numeric_limits<int16_t>::max());
        _dynamicBatteryTargetVariance = 0.0f;
        _dynamicBatteryTargetInitialized = true;
        enqueueDynamicBatteryTargetOracleProbe(powerMeterUpdate);
        return;
    }

    auto state = currentDynamicBatteryTargetForecastState();
    state.updateMillis = previousPowerMeterUpdate;
    advanceDynamicBatteryTargetForecastState(state, meterValue, powerMeterUpdate);
    _dynamicBatteryTargetMean = state.mean;
    _dynamicBatteryTargetVariance = state.variance;
    _batteryTargetPowerConsumption = state.target;
    enqueueDynamicBatteryTargetOracleProbe(powerMeterUpdate);
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

float PowerLimiterClass::getStaticBatteryTargetPowerConsumption() const
{
    return Configuration.get().PowerLimiter.BatteryTargetPowerConsumption;
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

float PowerLimiterClass::getStaticTargetPowerConsumption() const
{
    auto const& config = Configuration.get();

    if (!config.PowerLimiter.TargetPowerConsumptionFollowStorageTarget) {
        return config.PowerLimiter.TargetPowerConsumption;
    }

    auto const target = getStaticBatteryTargetPowerConsumption()
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

float PowerLimiterClass::getFlexibleLoadDynamicReserveWatts() const
{
    return std::max(0.0f,
            getStaticBatteryTargetPowerConsumption()
            - getBatteryTargetPowerConsumption());
}

float PowerLimiterClass::getFlexibleLoadStopTargetPowerConsumption() const
{
    return std::max(
            getStaticTargetPowerConsumption(),
            getStaticBatteryTargetPowerConsumption());
}

bool PowerLimiterClass::isGridChargerManaged() const
{
    return ownsGridChargerTarget()
        && Mode::Normal == _mode
        && !Battery.getStats()->getImmediateChargingRequest();
}

bool PowerLimiterClass::ownsGridChargerTarget() const
{
    auto const& config = Configuration.get();
    return config.PowerLimiter.Enabled
        && config.GridCharger.Enabled
        && config.GridCharger.AutoPowerEnabled;
}

float PowerLimiterClass::calcBatteryTargetPowerConsumption() const
{
    return calcBatteryTargetPowerConsumption(
            _dynamicBatteryTargetVariance,
            _dynamicBatteryTargetInitialized);
}

float PowerLimiterClass::calcBatteryTargetPowerConsumption(
        float variance,
        bool initialized) const
{
    auto const& config = Configuration.get();
    auto const staticTarget = static_cast<float>(config.PowerLimiter.BatteryTargetPowerConsumption);

    if (!config.PowerLimiter.BatteryTargetPowerConsumptionDynamicEnabled
            || !initialized) {
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
    auto calculatedBias = std::sqrt(std::max(0.0f, variance)) * multiplier;
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
    auto meterValue = getRegulationPowerMeterTotal();

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
        bool allowStandby,
        bool capOutputLimit,
        uint16_t outputLimitHeadroom,
        Predictive::RequestDomain requestDomain,
        Predictive::RequestCause requestCause,
        bool forceStandbyReassert)
{
    std::vector<PowerLimiterInverter*> matchingInverters;
    struct PredictiveBaseline {
        PowerLimiterInverter* inverter;
        uint16_t expectedOutput;
    };
    std::vector<PredictiveBaseline> predictiveBaselines;
    std::vector<PowerLimiterInverter*> predictiveRequestSuppressedInverters;
    std::vector<PowerLimiterInverter*> thermalPredictiveInverters;
    uint16_t producing = 0; // sum of AC power the matching inverters produce now
    uint16_t expected = 0; // sum of AC power the matching inverters are already expected to produce

    auto plannerBlocksInverter = [&](PowerLimiterInverter const& inverter) {
        if (requestCause == Predictive::RequestCause::Safety) { return false; }
        if (!_predictiveModelInitialized) { return false; }
        return _predictiveModel.hasActivePowerStateTransition(
                predictiveInverterKey(inverter));
    };

    uint16_t plannerBlockedInverterCount = 0;
    uint16_t plannerBlockedProducing = 0;
    uint16_t plannerBlockedExpected = 0;
    for (auto& upInv : _inverters) {
        if (!filter(*upInv)) { continue; }

        if (!upInv->isEligible()) { continue; }
        if (plannerBlocksInverter(*upInv)) {
            ++plannerBlockedInverterCount;
            plannerBlockedProducing = static_cast<uint16_t>(std::min<uint32_t>(
                    static_cast<uint32_t>(plannerBlockedProducing)
                        + upInv->getCurrentOutputAcWatts(),
                    std::numeric_limits<uint16_t>::max()));
            plannerBlockedExpected = static_cast<uint16_t>(std::min<uint32_t>(
                    static_cast<uint32_t>(plannerBlockedExpected)
                        + upInv->getExpectedOutputAcWatts(),
                    std::numeric_limits<uint16_t>::max()));
            continue;
        }

        producing += upInv->getCurrentOutputAcWatts();
        expected += upInv->getExpectedOutputAcWatts();
        matchingInverters.push_back(upInv.get());
        predictiveBaselines.push_back({
            upInv.get(),
            upInv->getExpectedOutputAcWatts(),
        });
    }

    if (plannerBlockedInverterCount > 0) {
        DTU_LOGD("planner held %u %s inverter%s producing %u W, expecting %u W "
                "while power-state transition effect is pending",
                static_cast<unsigned>(plannerBlockedInverterCount),
                filterExpression.c_str(),
                plannerBlockedInverterCount == 1 ? "" : "s",
                static_cast<unsigned>(plannerBlockedProducing),
                static_cast<unsigned>(plannerBlockedExpected));
    }

    if (matchingInverters.empty()) { return plannerBlockedExpected; }

    auto const totalPowerRequested = powerRequested;
    powerRequested = powerRequested > plannerBlockedExpected
        ? static_cast<uint16_t>(powerRequested - plannerBlockedExpected)
        : static_cast<uint16_t>(0);

    auto totalCoveredWithHeldInverters = [&](uint16_t controllableCovered) {
        return static_cast<uint16_t>(std::min<uint32_t>(
                static_cast<uint32_t>(plannerBlockedExpected)
                    + controllableCovered,
                std::numeric_limits<uint16_t>::max()));
    };

    bool plural = matchingInverters.size() != 1;

    auto suppressPredictiveRequestFor = [&](PowerLimiterInverter* inverter) {
        if (std::find(predictiveRequestSuppressedInverters.begin(),
                predictiveRequestSuppressedInverters.end(),
                inverter) != predictiveRequestSuppressedInverters.end()) {
            return;
        }
        predictiveRequestSuppressedInverters.push_back(inverter);
    };

    auto predictiveRequestSuppressed = [&](PowerLimiterInverter* inverter) {
        return std::find(predictiveRequestSuppressedInverters.begin(),
                predictiveRequestSuppressedInverters.end(),
                inverter) != predictiveRequestSuppressedInverters.end();
    };

    auto thermalPredictiveRequest = [&](PowerLimiterInverter* inverter) {
        return std::find(thermalPredictiveInverters.begin(),
                thermalPredictiveInverters.end(),
                inverter) != thermalPredictiveInverters.end();
    };

    auto recordPredictiveChanges = [&](uint16_t covered) -> uint16_t {
        for (auto const& baseline : predictiveBaselines) {
            if (predictiveRequestSuppressed(baseline.inverter)) { continue; }

            auto const target = baseline.inverter->getExpectedOutputAcWatts();
            if (target == baseline.expectedOutput) { continue; }

            auto const predictiveCause = thermalPredictiveRequest(baseline.inverter)
                ? Predictive::RequestCause::Thermal
                : requestCause;
            recordPredictiveInverterRequest(
                    *baseline.inverter,
                    target,
                    requestDomain,
                    predictiveCause);
        }
        return totalCoveredWithHeldInverters(covered);
    };

    // if we update battery-powered inverters and the battery is in the STOP state,
    // we must put all battery-powered inverters into standby mode,
    // regardless of whether the standby option is enabled or not.
    if ((matchingInverters[0]->isBatteryPowered()) && (_batteryState == BatteryState::STOP)) {
        for (auto pInv : matchingInverters) {
            pInv->standby();
            if (forceStandbyReassert) { pInv->reassertTargetPowerState(); }
        }
        DTU_LOGD("battery is in STOP state, all battery-powered inverters are put into standby.");
        return recordPredictiveChanges(0);
    }

    int32_t diff = powerRequested - expected;

    DTU_LOGD("requesting %d/%d W from %d %s inverter%s currently "
            "producing %d W, expecting %d W (held %u W, diff %i W)",
            powerRequested, totalPowerRequested,
            matchingInverters.size(), filterExpression.c_str(),
            (plural?"s":""), producing, expected,
            static_cast<unsigned>(plannerBlockedExpected), diff);

    if (powerRequested == 0
            && allowStandby
            && matchingInverters[0]->isBatteryPowered()
            && (diff < 0 || forceStandbyReassert)) {
        bool changed = false;
        bool reasserted = false;
        for (auto pInv : matchingInverters) {
            auto const before = pInv->getExpectedOutputAcWatts();
            auto const maxReductionWithoutStandby =
                pInv->getMaxReductionWatts(false/*no standby*/);
            auto const maxReductionWithStandby =
                pInv->getMaxReductionWatts(true/*allow standby*/);

            if (forceStandbyReassert) {
                pInv->standby();
                pInv->reassertTargetPowerState();
                reasserted = true;
            } else if (maxReductionWithStandby > maxReductionWithoutStandby) {
                pInv->standby();
            } else if (maxReductionWithoutStandby > 0) {
                pInv->applyReduction(
                        maxReductionWithoutStandby,
                        false/*no standby*/);
            }
            changed |= pInv->getExpectedOutputAcWatts() != before;
        }

        if (changed || reasserted) {
            uint16_t covered = 0;
            for (auto pInv : matchingInverters) {
                covered += pInv->getExpectedOutputAcWatts();
            }
            if (reasserted) {
                DTU_LOGD("reasserting standby for battery-powered inverters while target is 0 W");
            }
            DTU_LOGD("will cover %d W using %d %s inverter%s",
                    covered, matchingInverters.size(),
                    filterExpression.c_str(), (plural?"s":""));
            return recordPredictiveChanges(covered);
        }
    }

    bool limitRefreshScheduled = false;
    for (auto pInv : matchingInverters) {
        limitRefreshScheduled |= pInv->applyConfiguredLimitBounds();
    }

    if (limitRefreshScheduled) {
        uint16_t covered = 0;
        for (auto pInv : matchingInverters) {
            covered += pInv->getExpectedOutputAcWatts();
        }
        return recordPredictiveChanges(covered);
    }

    auto optimizeBatteryStandbySet = [&]() -> std::optional<uint16_t> {
        if (!allowStandby || !matchingInverters[0]->isBatteryPowered()) {
            return std::nullopt;
        }
        if (matchingInverters.size() > 16) {
            return std::nullopt;
        }

        uint32_t const candidateCount = matchingInverters.size();
        auto const& powerLimiterConfig = Configuration.get().PowerLimiter;
        bool const batteryEagerStartEnabled =
            powerLimiterConfig.BatteryEagerStartEnabled;
        bool const batteryMaximizeInverters =
            batteryEagerStartEnabled
            && powerLimiterConfig.BatteryEagerStartMaximizeInverters;
        uint32_t const acceptableSurplusWatts =
            batteryEagerStartEnabled && powerRequested > 0
                ? powerLimiterConfig.BatteryStandbyPowerMargin
                : 0;
        uint32_t currentOnlineMask = 0;
        for (uint32_t index = 0; index < candidateCount; ++index) {
            auto const* inverter = matchingInverters[index];
            if (inverter->getExpectedOutputAcWatts() > 0) {
                currentOnlineMask |= (1UL << index);
            }
        }

        auto countBits = [](uint32_t value) -> uint8_t {
            uint8_t count = 0;
            while (value != 0) {
                count += static_cast<uint8_t>(value & 1UL);
                value >>= 1;
            }
            return count;
        };

        struct BatteryStandbyChoice {
            bool valid = false;
            uint32_t mask = 0;
            uint16_t covered = 0;
            uint32_t error = std::numeric_limits<uint32_t>::max();
            uint8_t onlineCount = std::numeric_limits<uint8_t>::max();
            uint8_t transitions = std::numeric_limits<uint8_t>::max();
            uint32_t surplusCapacity = std::numeric_limits<uint32_t>::max();
        };

        auto betterChoice = [batteryMaximizeInverters](BatteryStandbyChoice const& candidate,
                BatteryStandbyChoice const& best) {
            if (!best.valid) { return true; }
            if (candidate.error != best.error) {
                return candidate.error < best.error;
            }
            if (batteryMaximizeInverters
                    && candidate.onlineCount != best.onlineCount) {
                return candidate.onlineCount > best.onlineCount;
            }
            if (candidate.transitions != best.transitions) {
                return candidate.transitions < best.transitions;
            }
            if (candidate.onlineCount != best.onlineCount) {
                return candidate.onlineCount < best.onlineCount;
            }
            return candidate.surplusCapacity < best.surplusCapacity;
        };

        BatteryStandbyChoice best;
        struct BatteryOnlineRange {
            bool valid = false;
            uint16_t minimum = 0;
            uint16_t maximum = 0;
        };

        auto calcOnlineRange = [](PowerLimiterInverter const* inverter) {
            BatteryOnlineRange range;

            auto const lowerPowerLimit = inverter->getConfiguredLowerPowerLimitWatts();
            auto const upperPowerLimit = inverter->getConfiguredMaxPowerWatts();
            if (upperPowerLimit < lowerPowerLimit) {
                return range;
            }

            uint32_t minimum = lowerPowerLimit;
            uint32_t maximum = upperPowerLimit;

            auto const expectedOutput = inverter->getExpectedOutputAcWatts();
            if (expectedOutput > 0) {
                auto const reducibleWithoutStandby =
                    inverter->getMaxReductionWatts(false/*no standby*/);
                minimum = expectedOutput > reducibleWithoutStandby
                    ? static_cast<uint32_t>(expectedOutput - reducibleWithoutStandby)
                    : static_cast<uint32_t>(0);
                minimum = std::max<uint32_t>(minimum, lowerPowerLimit);

                maximum = std::min<uint32_t>(
                        upperPowerLimit,
                        static_cast<uint32_t>(expectedOutput)
                            + inverter->getMaxIncreaseWatts());
                if (maximum < minimum) {
                    maximum = minimum;
                }
            }

            range.valid = minimum <= maximum;
            range.minimum = static_cast<uint16_t>(std::min<uint32_t>(
                    minimum,
                    std::numeric_limits<uint16_t>::max()));
            range.maximum = static_cast<uint16_t>(std::min<uint32_t>(
                    maximum,
                    std::numeric_limits<uint16_t>::max()));
            return range;
        };

        uint32_t const maskLimit = 1UL << candidateCount;
        for (uint32_t mask = 0; mask < maskLimit; ++mask) {
            if (diff < 0 && (mask & ~currentOnlineMask) != 0) {
                continue;
            }

            uint32_t minOutput = 0;
            uint32_t maxOutput = 0;
            bool valid = true;

            for (uint32_t index = 0; index < candidateCount; ++index) {
                if ((mask & (1UL << index)) == 0) { continue; }

                auto const* inverter = matchingInverters[index];
                if (inverter->getExpectedOutputAcWatts() == 0
                        && !inverter->isProducing()
                        && !batteryEagerStartEnabled) {
                    valid = false;
                    break;
                }

                auto const range = calcOnlineRange(inverter);
                if (!range.valid) {
                    valid = false;
                    break;
                }

                minOutput += range.minimum;
                maxOutput += range.maximum;
            }

            if (!valid) { continue; }
            if (mask != 0 && maxOutput == 0) { continue; }

            uint32_t covered = 0;
            if (mask != 0) {
                covered = std::clamp<uint32_t>(powerRequested, minOutput, maxOutput);
            }

            BatteryStandbyChoice candidate;
            candidate.valid = true;
            candidate.mask = mask;
            candidate.covered = static_cast<uint16_t>(std::min<uint32_t>(
                    covered,
                    std::numeric_limits<uint16_t>::max()));
            auto const targetUpper = static_cast<uint32_t>(powerRequested)
                + acceptableSurplusWatts;
            if (covered < powerRequested) {
                candidate.error = powerRequested - covered;
            } else if (covered > targetUpper) {
                candidate.error = covered - targetUpper;
            } else {
                candidate.error = 0;
            }
            candidate.onlineCount = countBits(mask);
            candidate.transitions = countBits(mask ^ currentOnlineMask);
            candidate.surplusCapacity = maxOutput > covered ? maxOutput - covered : 0;

            if (betterChoice(candidate, best)) {
                best = candidate;
            }
        }

        if (!best.valid) {
            return std::nullopt;
        }
        if (diff > 0 && best.mask == 0) {
            DTU_LOGD("battery standby optimizer defers %u W startup request "
                    "to incremental increase path",
                    static_cast<unsigned>(powerRequested));
            return std::nullopt;
        }

        std::vector<uint16_t> targetOutputs(candidateCount, 0);
        if (best.mask != 0) {
            std::vector<BatteryOnlineRange> ranges(candidateCount);
            uint32_t baselineOutput = 0;

            for (uint32_t index = 0; index < candidateCount; ++index) {
                if ((best.mask & (1UL << index)) == 0) { continue; }

                auto const range = calcOnlineRange(matchingInverters[index]);
                if (!range.valid) { continue; }

                ranges[index] = range;
                targetOutputs[index] = static_cast<uint16_t>(std::clamp<uint32_t>(
                        matchingInverters[index]->getExpectedOutputAcWatts(),
                        range.minimum,
                        range.maximum));
                baselineOutput += targetOutputs[index];
            }

            auto applySparseTargetAdjustment = [&](uint16_t requested,
                    bool increase) {
                if (requested == 0) { return; }

                std::vector<DistributedChange> changes;
                for (uint32_t index = 0; index < candidateCount; ++index) {
                    if ((best.mask & (1UL << index)) == 0) { continue; }
                    auto const range = ranges[index];
                    if (!range.valid) { continue; }

                    auto const capacity = increase
                        ? range.maximum - targetOutputs[index]
                        : targetOutputs[index] - range.minimum;
                    if (capacity == 0) { continue; }

                    changes.push_back({
                            matchingInverters[index],
                            static_cast<uint16_t>(capacity),
                    });
                }

                applyThermalWeights(changes, matchingInverters,
                        increase ? ThermalPreference::Cooler : ThermalPreference::Hotter,
                        filterExpression);
                auto const lockedLastAdjustedSerial =
                    repeatLockedLastAdjustedSerial(
                            sBatteryStandbyTargetLastAdjustedSerial,
                            sBatteryStandbyTargetLastAdjustedMillis,
                            millis());
                prioritizeDistributedChanges(changes,
                        sBatteryStandbyTargetRoundRobinCursor,
                        lockedLastAdjustedSerial);
                distributeGreedyByPriority(requested, changes);

                for (auto const& change : changes) {
                    if (change.amount == 0) { continue; }
                    sBatteryStandbyTargetLastAdjustedSerial =
                        change.inverter->getSerial();
                    sBatteryStandbyTargetLastAdjustedMillis = millis();
                    for (uint32_t index = 0; index < candidateCount; ++index) {
                        if (matchingInverters[index] != change.inverter) { continue; }
                        if (increase) {
                            targetOutputs[index] = static_cast<uint16_t>(
                                    std::min<uint32_t>(
                                            static_cast<uint32_t>(targetOutputs[index])
                                                + change.amount,
                                            std::numeric_limits<uint16_t>::max()));
                        } else {
                            targetOutputs[index] = targetOutputs[index] > change.amount
                                ? static_cast<uint16_t>(targetOutputs[index] - change.amount)
                                : static_cast<uint16_t>(0);
                        }
                        break;
                    }
                }
            };

            if (baselineOutput < best.covered) {
                applySparseTargetAdjustment(
                        static_cast<uint16_t>(best.covered - baselineOutput),
                        true/*increase*/);
            } else if (baselineOutput > best.covered) {
                applySparseTargetAdjustment(
                        static_cast<uint16_t>(std::min<uint32_t>(
                                baselineOutput - best.covered,
                                std::numeric_limits<uint16_t>::max())),
                        false/*increase*/);
            }
        }

        for (uint32_t index = 0; index < candidateCount; ++index) {
            auto* inverter = matchingInverters[index];
            auto const targetOutput = targetOutputs[index];
            auto const expectedOutput = inverter->getExpectedOutputAcWatts();

            if (targetOutput == expectedOutput) { continue; }

            if (targetOutput == 0) {
                inverter->standby();
            } else if (targetOutput > expectedOutput) {
                inverter->applyIncrease(targetOutput - expectedOutput);
            } else {
                inverter->applyReduction(
                        expectedOutput - targetOutput,
                        false/*no standby*/);
            }
        }

        std::vector<PowerLimiterInverter*> thermalBatteryInverters;
        thermalBatteryInverters.reserve(candidateCount);
        for (uint32_t index = 0; index < candidateCount; ++index) {
            if ((best.mask & (1UL << index)) == 0) { continue; }

            auto* inverter = matchingInverters[index];
            if (inverter->getExpectedOutputAcWatts() == 0) { continue; }

            thermalBatteryInverters.push_back(inverter);
        }
        rebalanceThermally(thermalBatteryInverters, filterExpression,
                sThermalRebalanceMaxStepWatts,
                &thermalPredictiveInverters);

        uint16_t covered = 0;
        uint8_t onlineCount = 0;
        for (auto const* inverter : matchingInverters) {
            auto const output = inverter->getExpectedOutputAcWatts();
            covered = static_cast<uint16_t>(std::min<uint32_t>(
                    static_cast<uint32_t>(covered) + output,
                    std::numeric_limits<uint16_t>::max()));
            if (output > 0) { ++onlineCount; }
        }

        DTU_LOGD("optimized battery standby set covers %u W using %u/%u "
                "battery-powered inverters for %u W request",
                static_cast<unsigned>(covered),
                static_cast<unsigned>(onlineCount),
                static_cast<unsigned>(candidateCount),
                static_cast<unsigned>(powerRequested));
        DTU_LOGD("will cover %d W using %d %s inverter%s",
                covered, matchingInverters.size(),
                filterExpression.c_str(), (plural?"s":""));

        return recordPredictiveChanges(covered);
    };

    if (auto const optimizedBatteryCoverage = optimizeBatteryStandbySet()) {
        return *optimizedBatteryCoverage;
    }

    auto capExcessOutputLimit = [&](uint16_t capTarget) -> bool {
        if (!capOutputLimit || diff < 0) { return false; }
        uint16_t const minimumHeadroom = outputLimitHeadroom > 0
            ? outputLimitHeadroom
            : sSolarOutputLimitSafetyHeadroomWatts;
        return capProductionLimitedOutputLimits(
                matchingInverters,
                capTarget,
                minimumHeadroom,
                &predictiveRequestSuppressedInverters);
    };

    if (diff == 0) {
        bool const outputLimitCapScheduled = capExcessOutputLimit(powerRequested);
        bool periodicRefreshScheduled = false;
        if (!outputLimitCapScheduled) {
            for (auto pInv : matchingInverters) {
                if (pInv->refreshStaleLimit()) {
                    periodicRefreshScheduled = true;
                    suppressPredictiveRequestFor(pInv);
                }
            }
        }

        if (!outputLimitCapScheduled
                && !periodicRefreshScheduled
                && !rebalanceThermally(matchingInverters, filterExpression,
                        sThermalRebalanceMaxStepWatts,
                        &thermalPredictiveInverters)) {
            return recordPredictiveChanges(expected);
        }

        uint16_t covered = 0;
        for (auto pInv : matchingInverters) {
            covered += pInv->getExpectedOutputAcWatts();
        }
        return recordPredictiveChanges(covered);
    }

    uint16_t covered = 0;
    uint32_t& distributionRoundRobinCursor = matchingInverters[0]->isSolarPowered()
        ? sSolarSparseLimitRoundRobinCursor
        : matchingInverters[0]->isBatteryPowered()
            ? sBatterySparseLimitRoundRobinCursor
            : sThermalDistributionRoundRobinCursor;
    uint64_t& distributionLastAdjustedSerial = matchingInverters[0]->isSolarPowered()
        ? sSolarSparseLastAdjustedSerial
        : matchingInverters[0]->isBatteryPowered()
            ? sBatterySparseLastAdjustedSerial
            : sThermalDistributionLastAdjustedSerial;
    uint32_t& distributionLastAdjustedMillis = matchingInverters[0]->isSolarPowered()
        ? sSolarSparseLastAdjustedMillis
        : matchingInverters[0]->isBatteryPowered()
            ? sBatterySparseLastAdjustedMillis
            : sThermalDistributionLastAdjustedMillis;

    if (diff < 0) {
        uint16_t reduction = static_cast<uint16_t>(diff * -1);
        std::vector<DistributedChange> changes;
        for (auto const pInv : matchingInverters) {
            uint16_t maxReduction = pInv->getMaxReductionWatts(false/*no standby*/);
            if (maxReduction == 0) { continue; }
            changes.push_back({ pInv, maxReduction });
        }

        applyThermalWeights(changes, matchingInverters,
                ThermalPreference::Hotter, filterExpression);
        auto const lockedLastAdjustedSerial = repeatLockedLastAdjustedSerial(
                distributionLastAdjustedSerial,
                distributionLastAdjustedMillis,
                millis());
        prioritizeDistributedChanges(changes, distributionRoundRobinCursor,
                lockedLastAdjustedSerial);
        distributeGreedyByPriority(reduction, changes);
        uint16_t appliedReduction = 0;
        for (auto& change : changes) {
            if (change.amount == 0) { continue; }
            auto const applied = change.inverter->applyReduction(
                    change.amount,
                    false/*no standby*/);
            appliedReduction += applied;
            if (applied > 0) {
                distributionLastAdjustedSerial = change.inverter->getSerial();
                distributionLastAdjustedMillis = millis();
            }
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

        rebalanceThermally(matchingInverters, filterExpression,
                sThermalRebalanceMaxStepWatts,
                &thermalPredictiveInverters);

        for (auto pInv : matchingInverters) {
            covered += pInv->getExpectedOutputAcWatts();
        }
    }
    else {
        uint16_t increase = static_cast<uint16_t>(diff);
        bool const batteryEagerStartEnabled =
            matchingInverters[0]->isBatteryPowered()
            && Configuration.get().PowerLimiter.BatteryEagerStartEnabled;

        auto applyDistributedIncrease = [&](uint16_t requested,
                std::vector<DistributedChange>& changes) -> uint16_t {
            if (requested == 0) { return 0; }

            applyThermalWeights(changes, matchingInverters,
                    ThermalPreference::Cooler, filterExpression);
            auto const lockedLastAdjustedSerial = repeatLockedLastAdjustedSerial(
                    distributionLastAdjustedSerial,
                    distributionLastAdjustedMillis,
                    millis());
            prioritizeDistributedChanges(changes, distributionRoundRobinCursor,
                    lockedLastAdjustedSerial);
            distributeGreedyByPriority(requested, changes);
            uint16_t applied = 0;
            for (auto& change : changes) {
                if (change.amount == 0) { continue; }
                auto const appliedChange = change.inverter->applyIncrease(change.amount);
                applied += appliedChange;
                if (appliedChange > 0) {
                    distributionLastAdjustedSerial = change.inverter->getSerial();
                    distributionLastAdjustedMillis = millis();
                }
            }
            return applied;
        };

        if (batteryEagerStartEnabled) {
            std::vector<DistributedChange> activeReductionChanges;
            std::vector<PowerLimiterInverter*> standbyCandidates;
            uint32_t activeReductionCapacity = 0;

            for (auto const pInv : matchingInverters) {
                uint16_t maxIncrease = pInv->getMaxIncreaseWatts();

                if (pInv->isProducing() || pInv->getExpectedOutputAcWatts() > 0) {
                    uint16_t maxReduction = pInv->getMaxReductionWatts(false/*no standby*/);
                    if (maxReduction > 0) {
                        activeReductionChanges.push_back({ pInv, maxReduction });
                        activeReductionCapacity += maxReduction;
                    }
                } else {
                    if (maxIncrease == 0) { continue; }
                    standbyCandidates.push_back(pInv);
                }
            }

            std::sort(standbyCandidates.begin(), standbyCandidates.end(),
                    [](auto const left, auto const right) {
                        return left->getConfiguredLowerPowerLimitWatts()
                            < right->getConfiguredLowerPowerLimitWatts();
                    });

            struct StandbyStartup {
                PowerLimiterInverter* inverter;
                uint16_t lowerPowerLimit;
            };
            std::vector<StandbyStartup> standbyStartups;
            uint32_t startupWatts = 0;

            for (auto pInv : standbyCandidates) {
                auto const lowerPowerLimit = pInv->getConfiguredLowerPowerLimitWatts();
                auto const maxIncrease = pInv->getMaxIncreaseWatts();
                if (maxIncrease < lowerPowerLimit) { continue; }

                uint32_t const outputAfterStartup =
                    static_cast<uint32_t>(expected) + startupWatts + lowerPowerLimit;
                uint32_t const requiredReduction = outputAfterStartup > powerRequested
                    ? outputAfterStartup - powerRequested
                    : 0;
                if (requiredReduction > activeReductionCapacity) { continue; }

                standbyStartups.push_back({ pInv, lowerPowerLimit });
                startupWatts += lowerPowerLimit;
            }

            uint16_t appliedOnlineReduction = 0;
            uint32_t const requiredOnlineReduction =
                (static_cast<uint32_t>(expected) + startupWatts) > powerRequested
                    ? static_cast<uint32_t>(expected) + startupWatts - powerRequested
                    : 0;
            if (requiredOnlineReduction > 0 && !activeReductionChanges.empty()) {
                auto const requestedReduction = static_cast<uint16_t>(std::min<uint32_t>(
                        requiredOnlineReduction,
                        std::numeric_limits<uint16_t>::max()));
                applyThermalWeights(activeReductionChanges, matchingInverters,
                        ThermalPreference::Hotter, filterExpression);
                auto const lockedLastAdjustedSerial = repeatLockedLastAdjustedSerial(
                        distributionLastAdjustedSerial,
                        distributionLastAdjustedMillis,
                        millis());
                prioritizeDistributedChanges(activeReductionChanges,
                        distributionRoundRobinCursor,
                        lockedLastAdjustedSerial);
                distributeGreedyByPriority(requestedReduction,
                        activeReductionChanges);

                for (auto& change : activeReductionChanges) {
                    if (change.amount == 0) { continue; }
                    auto const applied = change.inverter->applyReduction(
                            change.amount,
                            false/*no standby*/);
                    appliedOnlineReduction += applied;
                    if (applied > 0) {
                        distributionLastAdjustedSerial = change.inverter->getSerial();
                        distributionLastAdjustedMillis = millis();
                    }
                }
            }

            uint32_t onlineOutput = expected > appliedOnlineReduction
                ? static_cast<uint32_t>(expected - appliedOnlineReduction)
                : 0;

            for (auto const& startup : standbyStartups) {
                if ((onlineOutput + startup.lowerPowerLimit) > powerRequested) {
                    continue;
                }

                DTU_LOGD("eager-starting standby battery-powered inverter %s at %u W "
                        "to keep battery inverters online when possible",
                        startup.inverter->getSerialStr(),
                        startup.lowerPowerLimit);
                auto const applied = startup.inverter->applyIncrease(startup.lowerPowerLimit);
                onlineOutput += applied;
            }

            auto const coveredAfterOnlineStart = static_cast<uint16_t>(std::min<uint32_t>(
                    onlineOutput,
                    std::numeric_limits<uint16_t>::max()));
            uint16_t remainingIncrease = powerRequested > coveredAfterOnlineStart
                ? static_cast<uint16_t>(powerRequested - coveredAfterOnlineStart)
                : static_cast<uint16_t>(0);

            std::vector<DistributedChange> activeChanges;
            std::vector<PowerLimiterInverter*> remainingStandbyCandidates;
            uint32_t activeIncreaseCapacity = 0;
            for (auto const pInv : matchingInverters) {
                uint16_t maxIncrease = pInv->getMaxIncreaseWatts();
                if (maxIncrease == 0) { continue; }

                if (pInv->isProducing() || pInv->getExpectedOutputAcWatts() > 0) {
                    activeChanges.push_back({ pInv, maxIncrease });
                    activeIncreaseCapacity += maxIncrease;
                } else {
                    remainingStandbyCandidates.push_back(pInv);
                }
            }

            if (!activeChanges.empty()) {
                auto const activeRequest = static_cast<uint16_t>(std::min<uint32_t>(
                        remainingIncrease,
                        activeIncreaseCapacity));
                auto const appliedActive = applyDistributedIncrease(activeRequest, activeChanges);
                remainingIncrease = remainingIncrease > appliedActive
                    ? static_cast<uint16_t>(remainingIncrease - appliedActive)
                    : static_cast<uint16_t>(0);
            }

            std::sort(remainingStandbyCandidates.begin(), remainingStandbyCandidates.end(),
                    [](auto const left, auto const right) {
                        return left->getConfiguredLowerPowerLimitWatts()
                            < right->getConfiguredLowerPowerLimitWatts();
                    });

            for (auto pInv : remainingStandbyCandidates) {
                if (remainingIncrease == 0) { break; }

                auto const lowerPowerLimit = pInv->getConfiguredLowerPowerLimitWatts();
                auto const maxIncrease = pInv->getMaxIncreaseWatts();
                auto const startupRequest = static_cast<uint16_t>(std::min<uint32_t>(
                        std::max<uint32_t>(remainingIncrease, lowerPowerLimit),
                        maxIncrease));
                if (startupRequest < lowerPowerLimit) { continue; }

                DTU_LOGD("eager-starting battery-powered inverter %s at %u W "
                        "for a %u W residual request below the minimum online output",
                        pInv->getSerialStr(),
                        startupRequest,
                        remainingIncrease);
                auto const applied = pInv->applyIncrease(startupRequest);
                remainingIncrease = remainingIncrease > applied
                    ? static_cast<uint16_t>(remainingIncrease - applied)
                    : static_cast<uint16_t>(0);
            }

            if (remainingIncrease > 0) {
                std::vector<DistributedChange> changes;
                for (auto const pInv : matchingInverters) {
                    uint16_t maxIncrease = pInv->getMaxIncreaseWatts();
                    if (maxIncrease == 0) { continue; }
                    changes.push_back({ pInv, maxIncrease });
                }
                applyDistributedIncrease(remainingIncrease, changes);
            }
        } else {
            std::vector<DistributedChange> changes;
            for (auto const pInv : matchingInverters) {
                uint16_t maxIncrease = pInv->getMaxIncreaseWatts();
                if (maxIncrease == 0) { continue; }
                changes.push_back({ pInv, maxIncrease });
            }

            // When output is short, hitting the power target has priority. Thermal
            // exchange is handled separately once there is no active shortfall.
            applyDistributedIncrease(increase, changes);
        }

        // Capping excessive limits must not consume a positive demand: other
        // production-limited solar inverters may still need an increase probe.
        capExcessOutputLimit(powerRequested);
        rebalanceThermally(matchingInverters, filterExpression, 0,
                &thermalPredictiveInverters);

        for (auto pInv : matchingInverters) {
            covered += pInv->getExpectedOutputAcWatts();
        }
    }

    DTU_LOGD("will cover %d W using %d %s inverter%s",
            covered, matchingInverters.size(),
            filterExpression.c_str(), (plural?"s":""));

    return recordPredictiveChanges(covered);
}

// calculates how much power the battery-powered inverters shall draw from the
// power bus, which we call the part of the circuitry that is supplied by the
// solar charge controller(s), possibly an AC charger, as well as the battery.
uint16_t PowerLimiterClass::calcPowerBusUsage(uint16_t powerRequested)
{
    (void)getConfiguredBatteryDischargeCurrentLimit();

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

uint32_t PowerLimiterClass::getMaxInverterRadioQueueSize() const
{
    uint32_t queueSize = 0;
    for (auto const& upInv : _inverters) {
        queueSize = std::max(queueSize, upInv->getRadioQueueSize());
    }
    for (auto const& upInv : _retirees) {
        queueSize = std::max(queueSize, upInv->getRadioQueueSize());
    }
    return queueSize;
}

bool PowerLimiterClass::isInverterRadioQueueCongested() const
{
    return getMaxInverterRadioQueueSize() > sInverterRfQueueCongestionThreshold;
}

bool PowerLimiterClass::updateInverters(bool throttleRfQueue)
{
    bool busy = false;

    for (auto& upInv : _inverters) {
        bool const allowNewDispatch = !throttleRfQueue
            || getMaxInverterRadioQueueSize() <= sInverterRfQueueCongestionThreshold;
        if (upInv->update(allowNewDispatch)) { busy = true; }
        for (auto const& event : upInv->consumeTargetDispatchEvents()) {
            markPredictiveInverterRequestDispatched(*upInv, event);
        }
        if (!upInv->hasPendingTargetForTrace()) {
            discardPredictiveQueuedInverterRequests(*upInv);
        }
    }

    auto iter = _retirees.begin();
    while (iter != _retirees.end()) {
        bool const allowNewDispatch = !throttleRfQueue
            || getMaxInverterRadioQueueSize() <= sInverterRfQueueCongestionThreshold;
        if ((*iter)->retire(allowNewDispatch)) {
            for (auto const& event : (*iter)->consumeTargetDispatchEvents()) {
                markPredictiveInverterRequestDispatched(**iter, event);
            }
            if (!(*iter)->hasPendingTargetForTrace()) {
                discardPredictiveQueuedInverterRequests(**iter);
            }
            busy = true;
            ++iter;
            continue;
        }

        for (auto const& event : (*iter)->consumeTargetDispatchEvents()) {
            markPredictiveInverterRequestDispatched(**iter, event);
        }
        if (!(*iter)->hasPendingTargetForTrace()) {
            discardPredictiveQueuedInverterRequests(**iter);
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

uint16_t PowerLimiterClass::getCurrentInvertersOutputAcWatts(inverter_filter_t filter) const
{
    uint32_t res = 0;

    for (auto const& upInv : _inverters) {
        if (!upInv->isEligible()) { continue; }
        if (!filter(*upInv)) { continue; }
        res += upInv->getCurrentOutputAcWatts();
    }

    return static_cast<uint16_t>(std::min<uint32_t>(
            res,
            std::numeric_limits<uint16_t>::max()));
}

uint16_t PowerLimiterClass::getCurrentInvertersOutputAcWatts() const
{
    return getCurrentInvertersOutputAcWatts(
            [](PowerLimiterInverter const&) { return true; });
}

uint16_t PowerLimiterClass::getBatteryInvertersMinOutputAcWatts() const
{
    uint16_t res = std::numeric_limits<uint16_t>::max();

    for (auto const& upInv : _inverters) {
        if (!upInv->isBatteryPowered()) { continue; }
        if (!upInv->isEligible()) { continue; }

        auto const lowerPowerLimit = upInv->getConfiguredLowerPowerLimitWatts();
        if (lowerPowerLimit == 0) { continue; }
        res = std::min(res, lowerPowerLimit);
    }

    return res == std::numeric_limits<uint16_t>::max() ? 0 : res;
}

std::optional<uint16_t> PowerLimiterClass::getBatteryDischargeLimit()
{
    if ((_batteryState == BatteryState::STOP) || (_batteryState == BatteryState::NO_DISCHARGE)) { return 0; }

    auto effectiveLimit = getConfiguredBatteryDischargeCurrentLimit();

    auto batteryCurrentLimit = Battery.getDischargeCurrentLimit();
    if (batteryCurrentLimit != FLT_MAX) {
        if (batteryCurrentLimit <= 0) { batteryCurrentLimit = -batteryCurrentLimit; }
        effectiveLimit = effectiveLimit
            ? std::min(*effectiveLimit, batteryCurrentLimit)
            : batteryCurrentLimit;
    }

    if (!effectiveLimit) { return std::nullopt; }
    if (*effectiveLimit <= 0.0f) { return 0; }

    // Prefer inverter voltage since there is a voltage drop between battery
    // and inverter. During startup the inverter statistics may not be available
    // yet; use the battery bus voltage as a conservative fallback so a valid
    // discharge limit does not collapse to zero just because RF stats are late.
    auto inverter = getInverterDcVoltage();
    auto voltage = inverter.first;
    if (voltage <= 0.0f) {
        voltage = getBatteryVoltage();
        if (voltage <= 0.0f) {
            DTU_LOGE("could not determine inverter or battery voltage");
            return 0;
        }
        DTU_LOGW("could not determine inverter voltage, using battery voltage %.2f V",
                voltage);
    }

    if (voltage <= 0.0f) {
        return 0;
    }

    float const limitWatts = voltage * *effectiveLimit;
    return static_cast<uint16_t>(std::clamp<float>(
            limitWatts,
            0.0f,
            std::numeric_limits<uint16_t>::max()));
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
