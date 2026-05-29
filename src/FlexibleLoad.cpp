// SPDX-License-Identifier: GPL-2.0-or-later

#include "FlexibleLoad.h"
#include "FlexibleLoadStats.h"
#include "MqttSettings.h"
#include "PowerLimiter.h"
#include <battery/Controller.h>
#include <battery/Stats.h>
#include <gridcharger/Controller.h>
#include <powermeter/Controller.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <LittleFS.h>
#include <LogHelper.h>

#undef TAG
static const char* TAG = "dynamicPowerLimiter";
static const char* SUBTAG = "FlexibleLoad";
static const char* STATE_FILENAME = "/flexible_load_state.json";

FlexibleLoadClass FlexibleLoad;

void FlexibleLoadClass::init(Scheduler& scheduler)
{
    restoreState();

    scheduler.addTask(_loopTask);
    _loopTask.setCallback(std::bind(&FlexibleLoadClass::loop, this));
    _loopTask.setIterations(TASK_FOREVER);
    _loopTask.setInterval(1 * TASK_SECOND);
    _loopTask.enable();
}

FlexibleLoadClass::State FlexibleLoadClass::getState(size_t index) const
{
    if (index >= MaxLoadCount) { return State::Disabled; }
    return _runtime[index].CurrentState;
}

char const* FlexibleLoadClass::getStateText(size_t index) const
{
    if (index >= MaxLoadCount) { return "unknown"; }
    return getStateText(_runtime[index].CurrentState);
}

char const* FlexibleLoadClass::getStateText(State state) const
{
    switch (state) {
        case State::Disabled: return "disabled";
        case State::Off: return "off";
        case State::WaitingForStart: return "waiting_for_start";
        case State::Starting: return "starting";
        case State::RunningMinTime: return "running_min_time";
        case State::Running: return "running";
        case State::Cooldown: return "cooldown";
    }

    return "unknown";
}

char const* FlexibleLoadClass::getStartReasonText(size_t index) const
{
    if (index >= MaxLoadCount) { return "unknown"; }
    return getStartReasonText(_runtime[index].CurrentStartReason);
}

char const* FlexibleLoadClass::getStartReasonText(StartReason reason) const
{
    switch (reason) {
        case StartReason::None: return "none";
        case StartReason::BatterySoC: return "battery_soc";
        case StartReason::BmsChargeCurrentLimit: return "bms_charge_current_limit";
    }

    return "unknown";
}

char const* FlexibleLoadClass::getStartBlockReasonText(size_t index) const
{
    if (index >= MaxLoadCount) { return "unknown"; }
    return getStartBlockReasonText(_runtime[index].CurrentStartBlockReason);
}

char const* FlexibleLoadClass::getStartBlockReasonText(StartBlockReason reason) const
{
    switch (reason) {
        case StartBlockReason::None: return "none";
        case StartBlockReason::PowerLimiterDisabled: return "power_limiter_disabled";
        case StartBlockReason::Disabled: return "disabled";
        case StartBlockReason::MqttTopicMissing: return "mqtt_topic_missing";
        case StartBlockReason::BatteryDisabled: return "battery_disabled";
        case StartBlockReason::PowerMeterInvalid: return "power_meter_invalid";
        case StartBlockReason::MqttDisconnected: return "mqtt_disconnected";
        case StartBlockReason::FeedInBelowStartLimit: return "feed_in_below_start_limit";
        case StartBlockReason::StartConditionMissing: return "start_condition_missing";
        case StartBlockReason::StartDelay: return "start_delay";
        case StartBlockReason::StartSlotBusy: return "start_slot_busy";
        case StartBlockReason::Cooldown: return "cooldown";
    }

    return "unknown";
}

char const* FlexibleLoadClass::getStopReason(size_t index) const
{
    if (index >= MaxLoadCount) { return "unknown"; }
    return _runtime[index].StopReason;
}

float FlexibleLoadClass::getBatterySupportEnergyWh(size_t index) const
{
    if (index >= MaxLoadCount) { return 0.0f; }
    return _runtime[index].BatterySupportEnergyWh;
}

bool FlexibleLoadClass::isRunning(size_t index) const
{
    if (index >= MaxLoadCount) { return false; }
    return isRunning(_runtime[index]);
}

bool FlexibleLoadClass::isRunning(Runtime const& runtime) const
{
    return runtime.CurrentState == State::Starting
        || runtime.CurrentState == State::RunningMinTime
        || runtime.CurrentState == State::Running;
}

bool FlexibleLoadClass::hasStartingLoad() const
{
    for (auto const& runtime : _runtime) {
        if (runtime.CurrentState == State::Starting) { return true; }
    }

    return false;
}

void FlexibleLoadClass::setState(size_t index, PowerLimiterFlexibleLoadConfig const& config, Runtime& runtime, State state)
{
    if (runtime.CurrentState == state) { return; }

    runtime.CurrentState = state;
    runtime.StateChangedMillis = millis();
    DTU_LOGI("flexible load %u '%s' state: %s",
            static_cast<unsigned>(index),
            config.Name,
            getStateText(runtime.CurrentState));
}

void FlexibleLoadClass::resetRunAccounting(Runtime& runtime)
{
    runtime.StartCandidateSince = 0;
    runtime.StopCandidateSince = 0;
    runtime.GridChargerLimitStopCandidateSince = 0;
    runtime.BmsChargeLimitStopCandidateSince = 0;
    runtime.LoadStartedMillis = 0;
    runtime.LastBatteryEnergyUpdate = 0;
    runtime.BatterySupportEnergyWh = 0.0f;
    runtime.StartCandidateReason = StartReason::None;
    runtime.CurrentStartReason = StartReason::None;
    runtime.CurrentStartBlockReason = StartBlockReason::None;
}

void FlexibleLoadClass::restoreState()
{
    auto const now = millis();
    for (auto& runtime : _runtime) {
        runtime.LastCommandPublish = now;
    }

    File f = LittleFS.open(STATE_FILENAME, "r", false);
    if (!f) {
        DTU_LOGD("no persisted flexible load state found");
        return;
    }

    JsonDocument doc;
    auto const error = deserializeJson(doc, f);
    f.close();

    if (error) {
        DTU_LOGW("failed to read persisted flexible load state: %s", error.c_str());
        return;
    }

    if (!doc["flexible_loads"].is<JsonArray>()) {
        DTU_LOGW("persisted flexible load state has no load array");
        return;
    }

    auto const& powerLimiterConfig = Configuration.get().PowerLimiter;
    JsonArray flexibleLoads = doc["flexible_loads"].as<JsonArray>();

    for (size_t i = 0; i < MaxLoadCount; ++i) {
        JsonObject persisted = flexibleLoads[i];
        if (persisted.isNull()) { continue; }

        auto const& config = powerLimiterConfig.FlexibleLoads[i];
        auto const* persistedTopic = persisted["mqtt_topic"] | "";
        if (!config.Enabled
                || std::strlen(config.MqttTopic) == 0
                || std::strcmp(persistedTopic, config.MqttTopic) != 0) {
            continue;
        }

        auto& runtime = _runtime[i];
        if (persisted["running"] | false) {
            runtime.CurrentState = State::Running;
            runtime.StateChangedMillis = now;
            runtime.LoadStartedMillis = now - static_cast<uint32_t>(config.MinRuntime) * 1000;
            runtime.LastBatteryEnergyUpdate = now;
            runtime.StopReason = "none";
            DTU_LOGI("restored flexible load %u '%s' as running",
                    static_cast<unsigned>(i),
                    config.Name);
        } else {
            runtime.CurrentState = State::Off;
            runtime.StateChangedMillis = now;
            runtime.StopReason = "restored_off";
        }
    }
}

bool FlexibleLoadClass::persistState() const
{
    File f = LittleFS.open(STATE_FILENAME, "w");
    if (!f) {
        DTU_LOGW("failed to open persisted flexible load state for writing");
        return false;
    }

    JsonDocument doc;
    doc["version"] = 1;
    JsonArray flexibleLoads = doc["flexible_loads"].to<JsonArray>();
    auto const& powerLimiterConfig = Configuration.get().PowerLimiter;

    for (size_t i = 0; i < MaxLoadCount; ++i) {
        auto const& config = powerLimiterConfig.FlexibleLoads[i];
        JsonObject persisted = flexibleLoads.add<JsonObject>();
        persisted["mqtt_topic"] = config.MqttTopic;
        persisted["running"] = isRunning(_runtime[i]);
    }

    auto const bytesWritten = serializeJson(doc, f);
    f.close();

    if (bytesWritten == 0) {
        DTU_LOGW("failed to write persisted flexible load state");
        return false;
    }

    return true;
}

float FlexibleLoadClass::getBatteryDischargePowerWatts() const
{
    auto const& config = Configuration.get();
    if (!config.Battery.Enabled) { return 0.0f; }

    auto stats = Battery.getStats();
    if (!stats->isVoltageValid() || !stats->isCurrentValid() || stats->getAgeSeconds() > 60) {
        return 0.0f;
    }

    return std::max(0.0f, stats->getVoltage() * -stats->getChargeCurrent());
}

bool FlexibleLoadClass::hasGridPowerReachedStartLimit(PowerLimiterFlexibleLoadConfig const& config) const
{
    auto const requiredFeedIn = -static_cast<float>(config.StartPowerDemand);
    return PowerMeter.getPowerTotal() <= requiredFeedIn;
}

bool FlexibleLoadClass::hasBatteryReachedStartSoC(PowerLimiterFlexibleLoadConfig const& config) const
{
    auto stats = Battery.getStats();
    if (!stats->isSoCValid() || stats->getSoCAgeSeconds() > 60) { return false; }

    return stats->getSoC() >= config.StartBatterySoCThreshold;
}

bool FlexibleLoadClass::isBmsChargeCurrentLimited(PowerLimiterFlexibleLoadConfig const& config) const
{
    auto const& globalConfig = Configuration.get();
    if (!config.StartOnBmsChargeCurrentLimit) { return false; }
    if (!globalConfig.GridCharger.Enabled || !globalConfig.GridCharger.AutoPowerEnabled) { return false; }
    if (!GridCharger.getAutoPowerStatus()) { return false; }

    auto stats = Battery.getStats();
    if (!stats->isCurrentValid() || !stats->isChargeCurrentLimitValid()) { return false; }
    if (stats->getAgeSeconds() > 60 || stats->getChargeCurrentLimitAgeSeconds() > 60) { return false; }

    auto const chargeCurrentLimit = stats->getChargeCurrentLimit();
    auto const margin = std::max(0.0f, config.BmsChargeCurrentLimitMargin);
    if (chargeCurrentLimit <= margin) { return false; }

    return stats->getChargeCurrent() >= (chargeCurrentLimit - margin);
}

FlexibleLoadClass::StartReason FlexibleLoadClass::getStartReason(
        PowerLimiterConfig const& powerLimiterConfig,
        PowerLimiterFlexibleLoadConfig const& config,
        Runtime& runtime)
{
    auto const& globalConfig = Configuration.get();

    runtime.CurrentStartBlockReason = StartBlockReason::None;

    if (!powerLimiterConfig.Enabled) {
        runtime.CurrentStartBlockReason = StartBlockReason::PowerLimiterDisabled;
        return StartReason::None;
    }
    if (!config.Enabled) {
        runtime.CurrentStartBlockReason = StartBlockReason::Disabled;
        return StartReason::None;
    }
    if (std::strlen(config.MqttTopic) == 0) {
        runtime.CurrentStartBlockReason = StartBlockReason::MqttTopicMissing;
        return StartReason::None;
    }
    if (!globalConfig.Battery.Enabled) {
        runtime.CurrentStartBlockReason = StartBlockReason::BatteryDisabled;
        return StartReason::None;
    }
    if (!PowerMeter.isDataValid()) {
        runtime.CurrentStartBlockReason = StartBlockReason::PowerMeterInvalid;
        return StartReason::None;
    }
    if (!MqttSettings.getConnected()) {
        runtime.CurrentStartBlockReason = StartBlockReason::MqttDisconnected;
        return StartReason::None;
    }
    if (!hasGridPowerReachedStartLimit(config)) {
        runtime.CurrentStartBlockReason = StartBlockReason::FeedInBelowStartLimit;
        return StartReason::None;
    }

    if (hasBatteryReachedStartSoC(config)) { return StartReason::BatterySoC; }
    if (isBmsChargeCurrentLimited(config)) { return StartReason::BmsChargeCurrentLimit; }

    runtime.CurrentStartBlockReason = StartBlockReason::StartConditionMissing;
    return StartReason::None;
}

bool FlexibleLoadClass::shouldStopForGridPower(PowerLimiterFlexibleLoadConfig const& config) const
{
    if (!PowerMeter.isDataValid()) { return true; }

    auto const stopTarget = std::max<int16_t>(
            PowerLimiter.getTargetPowerConsumption(),
            PowerLimiter.getStorageTargetPowerConsumption());
    auto const stopLimit = static_cast<float>(stopTarget + config.StopPowerMargin);

    return PowerMeter.getPowerTotal() > stopLimit;
}

bool FlexibleLoadClass::shouldStopForGridChargerLimit(PowerLimiterFlexibleLoadConfig const& config) const
{
    auto const& globalConfig = Configuration.get();
    return config.StopOnGridChargerLimit
        && globalConfig.GridCharger.Enabled
        && globalConfig.GridCharger.AutoPowerEnabled
        && GridCharger.getAutoPowerStatus()
        && GridCharger.isAutoPowerLimitedByAvailablePower();
}

bool FlexibleLoadClass::handleGridChargerLimitStop(
        size_t index,
        PowerLimiterFlexibleLoadConfig const& config,
        Runtime& runtime,
        uint32_t now)
{
    if (!shouldStopForGridChargerLimit(config)) {
        runtime.GridChargerLimitStopCandidateSince = 0;
        return false;
    }

    if (runtime.GridChargerLimitStopCandidateSince == 0) {
        runtime.GridChargerLimitStopCandidateSince = now;
        return true;
    }

    if ((now - runtime.GridChargerLimitStopCandidateSince) >= static_cast<uint32_t>(config.StopDelay) * 1000) {
        stopLoad(index, config, runtime, "grid_charger_power_limited");
    }

    return true;
}

bool FlexibleLoadClass::hasBatterySupportBudgetExceeded(
        PowerLimiterFlexibleLoadConfig const& config,
        Runtime const& runtime) const
{
    return runtime.BatterySupportEnergyWh > static_cast<float>(config.MaxBatterySupportEnergy);
}

bool FlexibleLoadClass::handleBmsChargeCurrentLimitStop(
        size_t index,
        PowerLimiterFlexibleLoadConfig const& config,
        Runtime& runtime,
        uint32_t now)
{
    if (runtime.CurrentStartReason != StartReason::BmsChargeCurrentLimit
            || isBmsChargeCurrentLimited(config)) {
        runtime.BmsChargeLimitStopCandidateSince = 0;
        return false;
    }

    if (runtime.BmsChargeLimitStopCandidateSince == 0) {
        runtime.BmsChargeLimitStopCandidateSince = now;
        return true;
    }

    if ((now - runtime.BmsChargeLimitStopCandidateSince) >= static_cast<uint32_t>(config.StopDelay) * 1000) {
        stopLoad(index, config, runtime, "bms_charge_current_below_limit");
    }

    return true;
}

std::optional<float> FlexibleLoadClass::getMeasuredLoadPowerWatts(size_t index) const
{
    auto const power = FlexibleLoadStats.getPowerWatts(index);
    if (!power || !std::isfinite(*power)) { return std::nullopt; }

    return std::max(0.0f, *power);
}

float FlexibleLoadClass::getMeasuredRunningLoadPowerWatts() const
{
    float totalPower = 0.0f;

    for (size_t i = 0; i < MaxLoadCount; ++i) {
        if (_runtime[i].CurrentState != State::RunningMinTime
                && _runtime[i].CurrentState != State::Running) {
            continue;
        }

        auto const power = getMeasuredLoadPowerWatts(i);
        if (!power) { continue; }

        totalPower += *power;
    }

    return totalPower;
}

float FlexibleLoadClass::getBatterySupportPowerWatts(size_t index, PowerLimiterFlexibleLoadConfig const& config) const
{
    auto const dischargePower = getBatteryDischargePowerWatts();
    if (dischargePower <= config.BatterySupportPowerThreshold) { return 0.0f; }

    auto const billableDischargePower = dischargePower - config.BatterySupportPowerThreshold;
    auto const measuredLoadPower = getMeasuredLoadPowerWatts(index);
    auto const measuredRunningLoadPower = getMeasuredRunningLoadPowerWatts();

    if (!measuredLoadPower) {
        // Keep the legacy global-discharge accounting only when no active load
        // has a fresh real-power measurement to allocate against.
        return measuredRunningLoadPower > 0.0f ? 0.0f : billableDischargePower;
    }

    if (*measuredLoadPower <= 0.0f || measuredRunningLoadPower <= 0.0f) { return 0.0f; }

    auto const loadShare = *measuredLoadPower / measuredRunningLoadPower;
    return std::min(*measuredLoadPower, billableDischargePower * loadShare);
}

void FlexibleLoadClass::updateBatterySupportEnergy(size_t index, PowerLimiterFlexibleLoadConfig const& config, Runtime& runtime)
{
    auto const now = millis();
    if (runtime.LastBatteryEnergyUpdate == 0) {
        runtime.LastBatteryEnergyUpdate = now;
        return;
    }

    auto const elapsedMillis = now - runtime.LastBatteryEnergyUpdate;
    runtime.LastBatteryEnergyUpdate = now;

    auto const billablePower = getBatterySupportPowerWatts(index, config);
    if (billablePower <= 0.0f) { return; }

    runtime.BatterySupportEnergyWh += billablePower * (static_cast<float>(elapsedMillis) / (60.0f * 60.0f * 1000.0f));
}

bool FlexibleLoadClass::publishCommand(char const* payload, PowerLimiterFlexibleLoadConfig const& config, Runtime& runtime)
{
    if (!MqttSettings.getConnected()) {
        runtime.CurrentStartBlockReason = StartBlockReason::MqttDisconnected;
        return false;
    }
    if (std::strlen(config.MqttTopic) == 0) {
        runtime.CurrentStartBlockReason = StartBlockReason::MqttTopicMissing;
        return false;
    }

    MqttSettings.publishGeneric(config.MqttTopic, payload, config.MqttRetain);
    runtime.LastCommandPublish = millis();
    DTU_LOGI("sent flexible load command '%s' to MQTT topic '%s'", payload, config.MqttTopic);
    return true;
}

char const* FlexibleLoadClass::getPeriodicCommandPayload(
        PowerLimiterFlexibleLoadConfig const& config,
        Runtime const& runtime) const
{
    switch (runtime.CurrentState) {
        case State::Starting:
        case State::RunningMinTime:
        case State::Running:
            return config.MqttOnPayload;

        case State::Off:
        case State::WaitingForStart:
        case State::Cooldown:
            return config.MqttOffPayload;

        case State::Disabled:
            return nullptr;
    }

    return nullptr;
}

void FlexibleLoadClass::publishPeriodicCommand(
        PowerLimiterFlexibleLoadConfig const& config,
        Runtime& runtime,
        uint32_t now)
{
    auto const* payload = getPeriodicCommandPayload(config, runtime);
    if (payload == nullptr) { return; }

    auto const intervalMs = std::max<uint32_t>(1, Configuration.get().Mqtt.PublishInterval) * 1000;
    if (runtime.LastCommandPublish != 0 && (now - runtime.LastCommandPublish) < intervalMs) { return; }

    publishCommand(payload, config, runtime);
}

bool FlexibleLoadClass::startLoad(
        size_t index,
        PowerLimiterFlexibleLoadConfig const& config,
        Runtime& runtime,
        StartReason reason)
{
    if (!publishCommand(config.MqttOnPayload, config, runtime)) { return false; }

    resetRunAccounting(runtime);
    runtime.CurrentStartReason = reason;
    runtime.CurrentStartBlockReason = StartBlockReason::None;
    runtime.LoadStartedMillis = millis();
    runtime.StopReason = "none";
    setState(index, config, runtime, State::Starting);
    persistState();
    return true;
}

bool FlexibleLoadClass::stopLoad(
        size_t index,
        PowerLimiterFlexibleLoadConfig const& config,
        Runtime& runtime,
        char const* reason)
{
    if (!publishCommand(config.MqttOffPayload, config, runtime)) { return false; }

    runtime.StopReason = reason;
    runtime.StartCandidateSince = 0;
    runtime.StopCandidateSince = 0;
    runtime.GridChargerLimitStopCandidateSince = 0;
    runtime.BmsChargeLimitStopCandidateSince = 0;
    runtime.LastBatteryEnergyUpdate = 0;
    runtime.StartCandidateReason = StartReason::None;
    runtime.CurrentStartReason = StartReason::None;
    setState(index, config, runtime, State::Cooldown);
    persistState();
    DTU_LOGI("stopped flexible load %u '%s': %s",
            static_cast<unsigned>(index),
            config.Name,
            reason);
    return true;
}

bool FlexibleLoadClass::loopLoad(
        size_t index,
        Runtime& runtime,
        PowerLimiterConfig const& powerLimiterConfig,
        PowerLimiterFlexibleLoadConfig const& config,
        uint32_t now,
        bool canStart)
{
    if (!powerLimiterConfig.Enabled || !config.Enabled || std::strlen(config.MqttTopic) == 0) {
        if (isRunning(runtime)) {
            if (!stopLoad(index, config, runtime, "disabled")) { return false; }
        }
        resetRunAccounting(runtime);
        if (!powerLimiterConfig.Enabled) {
            runtime.CurrentStartBlockReason = StartBlockReason::PowerLimiterDisabled;
        } else if (!config.Enabled) {
            runtime.CurrentStartBlockReason = StartBlockReason::Disabled;
        } else {
            runtime.CurrentStartBlockReason = StartBlockReason::MqttTopicMissing;
        }
        setState(index, config, runtime, State::Disabled);
        return false;
    }

    switch (runtime.CurrentState) {
        case State::Disabled:
        case State::Off:
        case State::WaitingForStart:
        {
            auto const startReason = getStartReason(powerLimiterConfig, config, runtime);
            if (startReason == StartReason::None) {
                runtime.StartCandidateSince = 0;
                runtime.StartCandidateReason = StartReason::None;
                setState(index, config, runtime, State::Off);
                publishPeriodicCommand(config, runtime, now);
                return false;
            }

            if (runtime.StartCandidateSince == 0 || runtime.StartCandidateReason != startReason) {
                runtime.StartCandidateSince = now;
                runtime.StartCandidateReason = startReason;
                runtime.CurrentStartBlockReason = StartBlockReason::StartDelay;
                setState(index, config, runtime, State::WaitingForStart);
                publishPeriodicCommand(config, runtime, now);
                return false;
            }

            if ((now - runtime.StartCandidateSince) >= static_cast<uint32_t>(config.StartDelay) * 1000) {
                if (!canStart) {
                    runtime.CurrentStartBlockReason = StartBlockReason::StartSlotBusy;
                    publishPeriodicCommand(config, runtime, now);
                    return false;
                }

                return startLoad(index, config, runtime, startReason);
            }

            runtime.CurrentStartBlockReason = StartBlockReason::StartDelay;
            return false;
        }

        case State::Starting:
            runtime.CurrentStartBlockReason = StartBlockReason::None;
            if ((now - runtime.LoadStartedMillis) < static_cast<uint32_t>(config.StartupGrace) * 1000) {
                publishPeriodicCommand(config, runtime, now);
                return false;
            }

            runtime.LastBatteryEnergyUpdate = now;
            setState(index, config, runtime, (now - runtime.LoadStartedMillis) < static_cast<uint32_t>(config.MinRuntime) * 1000
                    ? State::RunningMinTime
                    : State::Running);
            publishPeriodicCommand(config, runtime, now);
            return false;

        case State::RunningMinTime:
            runtime.CurrentStartBlockReason = StartBlockReason::None;
            updateBatterySupportEnergy(index, config, runtime);
            if (handleBmsChargeCurrentLimitStop(index, config, runtime, now)) {
                publishPeriodicCommand(config, runtime, now);
                return false;
            }

            if (handleGridChargerLimitStop(index, config, runtime, now)) {
                publishPeriodicCommand(config, runtime, now);
                return false;
            }

            if ((now - runtime.LoadStartedMillis) >= static_cast<uint32_t>(config.MinRuntime) * 1000) {
                runtime.StopCandidateSince = 0;
                setState(index, config, runtime, State::Running);
            }
            publishPeriodicCommand(config, runtime, now);
            return false;

        case State::Running:
            runtime.CurrentStartBlockReason = StartBlockReason::None;
            updateBatterySupportEnergy(index, config, runtime);
            if (handleBmsChargeCurrentLimitStop(index, config, runtime, now)) {
                publishPeriodicCommand(config, runtime, now);
                return false;
            }

            if (hasBatterySupportBudgetExceeded(config, runtime)) {
                stopLoad(index, config, runtime, "battery_support_budget_exhausted");
                return false;
            }

            if (handleGridChargerLimitStop(index, config, runtime, now)) {
                publishPeriodicCommand(config, runtime, now);
                return false;
            }

            if (shouldStopForGridPower(config)) {
                if (runtime.StopCandidateSince == 0) {
                    runtime.StopCandidateSince = now;
                    publishPeriodicCommand(config, runtime, now);
                    return false;
                }

                if ((now - runtime.StopCandidateSince) >= static_cast<uint32_t>(config.StopDelay) * 1000) {
                    stopLoad(index, config, runtime, "grid_power_above_stop_limit");
                    return false;
                }
                publishPeriodicCommand(config, runtime, now);
                return false;
            }

            runtime.StopCandidateSince = 0;
            publishPeriodicCommand(config, runtime, now);
            return false;

        case State::Cooldown:
            runtime.CurrentStartBlockReason = StartBlockReason::Cooldown;
            if ((now - runtime.StateChangedMillis) >= static_cast<uint32_t>(config.MinOffTime) * 1000) {
                resetRunAccounting(runtime);
                setState(index, config, runtime, State::Off);
            }
            publishPeriodicCommand(config, runtime, now);
            return false;
    }

    return false;
}

void FlexibleLoadClass::loop()
{
    auto const& powerLimiterConfig = Configuration.get().PowerLimiter;
    auto const now = millis();
    bool startBlocked = hasStartingLoad();
    std::array<size_t, MaxLoadCount> startOrder = {};

    for (size_t i = 0; i < MaxLoadCount; ++i) {
        startOrder[i] = i;
    }

    std::sort(startOrder.begin(), startOrder.end(),
            [&powerLimiterConfig](size_t left, size_t right) {
                auto const& leftConfig = powerLimiterConfig.FlexibleLoads[left];
                auto const& rightConfig = powerLimiterConfig.FlexibleLoads[right];
                if (leftConfig.Priority == rightConfig.Priority) {
                    return left < right;
                }
                return leftConfig.Priority < rightConfig.Priority;
            });

    for (auto const index : startOrder) {
        auto const& config = powerLimiterConfig.FlexibleLoads[index];
        auto& runtime = _runtime[index];
        auto const started = loopLoad(index, runtime, powerLimiterConfig, config, now, !startBlocked);
        if (started || runtime.CurrentState == State::WaitingForStart) { startBlocked = true; }
    }
}
