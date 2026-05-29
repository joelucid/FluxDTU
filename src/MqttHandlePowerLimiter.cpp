// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022 Thomas Basler, Malte Schmidt and others
 */
#include "MqttSettings.h"
#include "MqttHandlePowerLimiter.h"
#include "FlexibleLoad.h"
#include "PowerLimiter.h"
#include <algorithm>
#include <ctime>
#include <limits>
#include <string>
#include <LogHelper.h>

#undef TAG
static const char* TAG = "dynamicPowerLimiter";
static const char* SUBTAG = "MQTT";

MqttHandlePowerLimiterClass MqttHandlePowerLimiter;

void MqttHandlePowerLimiterClass::init(Scheduler& scheduler)
{
    scheduler.addTask(_loopTask);
    _loopTask.setCallback(std::bind(&MqttHandlePowerLimiterClass::loop, this));
    _loopTask.setIterations(TASK_FOREVER);
    _loopTask.enable();

    using std::placeholders::_1;
    using std::placeholders::_2;
    using std::placeholders::_3;
    using std::placeholders::_4;

    subscribeTopics();

    _lastPublish = millis();
}

void MqttHandlePowerLimiterClass::forceUpdate()
{
    _lastPublish = 0;
}

void MqttHandlePowerLimiterClass::subscribeTopics()
{
    String const& prefix = MqttSettings.getPrefix();

    auto subscribe = [&prefix, this](char const* subTopic, MqttPowerLimiterCommand command) {
        String fullTopic(prefix + _cmdtopic.data() + subTopic);
        MqttSettings.subscribe(fullTopic.c_str(), 0,
                std::bind(&MqttHandlePowerLimiterClass::onMqttCmd, this, command,
                    std::placeholders::_1, std::placeholders::_2,
                    std::placeholders::_3, std::placeholders::_4));
    };

    for (auto const& s : _subscriptions) {
        subscribe(s.first.data(), s.second);
    }
}

void MqttHandlePowerLimiterClass::unsubscribeTopics()
{
    String const prefix = MqttSettings.getPrefix() + _cmdtopic.data();
    for (auto const& s : _subscriptions) {
        MqttSettings.unsubscribe(prefix + s.first.data());
    }
}

void MqttHandlePowerLimiterClass::loop()
{
    std::unique_lock<std::mutex> mqttLock(_mqttMutex);

    auto const& config = Configuration.get();

    if (!config.PowerLimiter.Enabled) {
        _mqttCallbacks.clear();
        return;
    }

    for (auto& callback : _mqttCallbacks) { callback(); }
    _mqttCallbacks.clear();

    mqttLock.unlock();

    if (!MqttSettings.getConnected() ) { return; }

    if ((millis() - _lastPublish) < (config.Mqtt.PublishInterval * 1000)) {
        return;
    }

    _lastPublish = millis();

    auto val = static_cast<unsigned>(PowerLimiter.getMode());
    MqttSettings.publish("powerlimiter/status/mode", String(val));

    MqttSettings.publish("powerlimiter/status/upper_power_limit", String(config.PowerLimiter.TotalUpperPowerLimit));

    MqttSettings.publish("powerlimiter/status/target_power_consumption", String(config.PowerLimiter.TargetPowerConsumption));
    MqttSettings.publish("powerlimiter/status/effective_target_power_consumption", String(PowerLimiter.getTargetPowerConsumption()));
    MqttSettings.publish("powerlimiter/status/target_power_consumption_follow_storage_target", String(config.PowerLimiter.TargetPowerConsumptionFollowStorageTarget));
    MqttSettings.publish("powerlimiter/status/target_power_consumption_storage_offset", String(config.PowerLimiter.TargetPowerConsumptionStorageOffset));

    MqttSettings.publish("powerlimiter/status/battery_target_power_consumption", String(config.PowerLimiter.BatteryTargetPowerConsumption));
    MqttSettings.publish("powerlimiter/status/battery_standby_power_margin", String(config.PowerLimiter.BatteryStandbyPowerMargin));
    MqttSettings.publish("powerlimiter/status/battery_target_power_consumption_dynamic_enabled", String(config.PowerLimiter.BatteryTargetPowerConsumptionDynamicEnabled));
    MqttSettings.publish("powerlimiter/status/battery_target_power_consumption_dynamic_max", String(config.PowerLimiter.BatteryTargetPowerConsumptionDynamicMax));
    MqttSettings.publish("powerlimiter/status/battery_target_power_consumption_dynamic_multiplier", String(config.PowerLimiter.BatteryTargetPowerConsumptionDynamicMultiplier));
    MqttSettings.publish("powerlimiter/status/battery_target_power_consumption_dynamic_window", String(config.PowerLimiter.BatteryTargetPowerConsumptionDynamicWindow));

    MqttSettings.publish("powerlimiter/status/inverter_update_timeouts", String(PowerLimiter.getInverterUpdateTimeouts()));

    auto publishFlexibleLoadStatus = [](String const& baseTopic, PowerLimiterFlexibleLoadConfig const& flexibleLoadConfig, size_t index) {
        MqttSettings.publish(baseTopic + "/name", flexibleLoadConfig.Name);
        MqttSettings.publish(baseTopic + "/enabled", String(flexibleLoadConfig.Enabled));
        MqttSettings.publish(baseTopic + "/priority", String(flexibleLoadConfig.Priority));
        MqttSettings.publish(baseTopic + "/configured", String(flexibleLoadConfig.MqttTopic[0] != '\0'));
        MqttSettings.publish(baseTopic + "/state", FlexibleLoad.getStateText(index));
        MqttSettings.publish(baseTopic + "/start_reason", FlexibleLoad.getStartReasonText(index));
        MqttSettings.publish(baseTopic + "/start_block_reason", FlexibleLoad.getStartBlockReasonText(index));
        MqttSettings.publish(baseTopic + "/stop_reason", FlexibleLoad.getStopReason(index));
        MqttSettings.publish(baseTopic + "/battery_support_energy", String(FlexibleLoad.getBatterySupportEnergyWh(index)));
        MqttSettings.publish(baseTopic + "/battery_discharge_power", String(FlexibleLoad.getBatteryDischargePowerWatts()));
    };

    for (size_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
        auto const& flexibleLoadConfig = config.PowerLimiter.FlexibleLoads[i];
        if (!flexibleLoadConfig.Enabled && flexibleLoadConfig.MqttTopic[0] == '\0') { continue; }

        publishFlexibleLoadStatus("powerlimiter/status/flexible_loads/" + String(i), flexibleLoadConfig, i);
        if (i == 0) {
            publishFlexibleLoadStatus("powerlimiter/status/flexible_load", flexibleLoadConfig, i);
        }
    }

    // no thresholds are relevant for setups without a battery
    if (!PowerLimiter.usesBatteryPoweredInverter()) { return; }

    MqttSettings.publish("powerlimiter/status/threshold/voltage/start", String(config.PowerLimiter.VoltageStartThreshold));
    MqttSettings.publish("powerlimiter/status/threshold/voltage/stop", String(config.PowerLimiter.VoltageStopThreshold));

    if (config.SolarCharger.Enabled) {
        MqttSettings.publish("powerlimiter/status/full_solar_passthrough_active", String(PowerLimiter.isFullSolarPassthroughActive()));
        MqttSettings.publish("powerlimiter/status/threshold/voltage/full_solar_passthrough_start", String(config.PowerLimiter.FullSolarPassThroughStartVoltage));
        MqttSettings.publish("powerlimiter/status/threshold/voltage/full_solar_passthrough_stop", String(config.PowerLimiter.FullSolarPassThroughStopVoltage));
    }

    if (!config.Battery.Enabled || config.PowerLimiter.IgnoreSoc) { return; }

    MqttSettings.publish("powerlimiter/status/threshold/soc/start", String(config.PowerLimiter.BatterySocStartThreshold));
    MqttSettings.publish("powerlimiter/status/threshold/soc/stop", String(config.PowerLimiter.BatterySocStopThreshold));

    if (config.SolarCharger.Enabled) {
        MqttSettings.publish("powerlimiter/status/threshold/soc/full_solar_passthrough", String(config.PowerLimiter.FullSolarPassThroughSoc));
    }
}

void MqttHandlePowerLimiterClass::onMqttCmd(MqttPowerLimiterCommand command, const espMqttClientTypes::MessageProperties& properties, const char* topic, const uint8_t* payload, size_t len)
{
    std::string strValue(reinterpret_cast<const char*>(payload), len);
    float payload_val = -1;
    try {
        payload_val = std::stof(strValue);
    }
    catch (std::invalid_argument const& e) {
        DTU_LOGE("cannot parse payload of topic '%s' as float: %s", topic, strValue.c_str());
        return;
    }
    const int intValue = static_cast<int>(payload_val);

    if (command == MqttPowerLimiterCommand::Mode) {
        std::lock_guard<std::mutex> mqttLock(_mqttMutex);
        using Mode = PowerLimiterClass::Mode;
        Mode mode = static_cast<Mode>(intValue);
        if (mode == Mode::UnconditionalFullSolarPassthrough) {
            DTU_LOGI("Power limiter unconditional full solar PT");
            _mqttCallbacks.push_back(std::bind(&PowerLimiterClass::setMode,
                        &PowerLimiter, Mode::UnconditionalFullSolarPassthrough));
        } else if (mode == Mode::Disabled) {
            DTU_LOGI("Power limiter disabled (override)");
            _mqttCallbacks.push_back(std::bind(&PowerLimiterClass::setMode,
                        &PowerLimiter, Mode::Disabled));
        } else if (mode == Mode::Normal) {
            DTU_LOGI("Power limiter normal operation");
            _mqttCallbacks.push_back(std::bind(&PowerLimiterClass::setMode,
                        &PowerLimiter, Mode::Normal));
        } else {
            DTU_LOGE("PowerLimiter - unknown mode %d", intValue);
        }
        return;
    }

    auto guard = Configuration.getWriteGuard();
    auto& config = guard.getConfig();

    switch (command) {
        case MqttPowerLimiterCommand::Mode:
            // handled separately above to avoid locking two mutexes
            break;
        case MqttPowerLimiterCommand::BatterySoCStartThreshold:
            if (config.PowerLimiter.BatterySocStartThreshold == intValue) { return; }
            DTU_LOGI("Setting battery SoC start threshold to: %d %%", intValue);
            config.PowerLimiter.BatterySocStartThreshold = intValue;
            break;
        case MqttPowerLimiterCommand::BatterySoCStopThreshold:
            if (config.PowerLimiter.BatterySocStopThreshold == intValue) { return; }
            DTU_LOGI("Setting battery SoC stop threshold to: %d %%", intValue);
            config.PowerLimiter.BatterySocStopThreshold = intValue;
            break;
        case MqttPowerLimiterCommand::FullSolarPassthroughSoC:
            if (config.PowerLimiter.FullSolarPassThroughSoc == intValue) { return; }
            DTU_LOGI("Setting full solar passthrough SoC to: %d %%", intValue);
            config.PowerLimiter.FullSolarPassThroughSoc = intValue;
            break;
        case MqttPowerLimiterCommand::VoltageStartThreshold:
            if (config.PowerLimiter.VoltageStartThreshold == payload_val) { return; }
            DTU_LOGI("Setting voltage start threshold to: %.2f V", payload_val);
            config.PowerLimiter.VoltageStartThreshold = payload_val;
            break;
        case MqttPowerLimiterCommand::VoltageStopThreshold:
            if (config.PowerLimiter.VoltageStopThreshold == payload_val) { return; }
            DTU_LOGI("Setting voltage stop threshold to: %.2f V", payload_val);
            config.PowerLimiter.VoltageStopThreshold = payload_val;
            break;
        case MqttPowerLimiterCommand::FullSolarPassThroughStartVoltage:
            if (config.PowerLimiter.FullSolarPassThroughStartVoltage == payload_val) { return; }
            DTU_LOGI("Setting full solar passthrough start voltage to: %.2f V", payload_val);
            config.PowerLimiter.FullSolarPassThroughStartVoltage = payload_val;
            break;
        case MqttPowerLimiterCommand::FullSolarPassThroughStopVoltage:
            if (config.PowerLimiter.FullSolarPassThroughStopVoltage == payload_val) { return; }
            DTU_LOGI("Setting full solar passthrough stop voltage to: %.2f V", payload_val);
            config.PowerLimiter.FullSolarPassThroughStopVoltage = payload_val;
            break;
        case MqttPowerLimiterCommand::UpperPowerLimit:
            if (config.PowerLimiter.TotalUpperPowerLimit == intValue) { return; }
            DTU_LOGI("Setting total upper power limit to: %d W", intValue);
            config.PowerLimiter.TotalUpperPowerLimit = intValue;
            break;
        case MqttPowerLimiterCommand::TargetPowerConsumption:
            if (config.PowerLimiter.TargetPowerConsumption == intValue) { return; }
            DTU_LOGI("Setting target power consumption to: %d W", intValue);
            config.PowerLimiter.TargetPowerConsumption = intValue;
            break;
        case MqttPowerLimiterCommand::TargetPowerConsumptionFollowStorageTarget:
            if (config.PowerLimiter.TargetPowerConsumptionFollowStorageTarget == static_cast<bool>(intValue)) { return; }
            DTU_LOGI("Setting target power consumption follow storage target to: %s", intValue ? "enabled" : "disabled");
            config.PowerLimiter.TargetPowerConsumptionFollowStorageTarget = intValue != 0;
            break;
        case MqttPowerLimiterCommand::TargetPowerConsumptionStorageOffset:
        {
            auto const offset = std::clamp(intValue, 0, static_cast<int>(std::numeric_limits<int16_t>::max()));
            if (config.PowerLimiter.TargetPowerConsumptionStorageOffset == offset) { return; }
            DTU_LOGI("Setting target power consumption storage offset to: %d W", offset);
            config.PowerLimiter.TargetPowerConsumptionStorageOffset = offset;
            break;
        }
        case MqttPowerLimiterCommand::BatteryTargetPowerConsumption:
            if (config.PowerLimiter.BatteryTargetPowerConsumption == intValue) { return; }
            DTU_LOGI("Setting battery target power consumption to: %d W", intValue);
            config.PowerLimiter.BatteryTargetPowerConsumption = intValue;
            break;
        case MqttPowerLimiterCommand::BatteryStandbyPowerMargin:
        {
            auto const margin = std::clamp(intValue, 0, static_cast<int>(std::numeric_limits<int16_t>::max()));
            if (config.PowerLimiter.BatteryStandbyPowerMargin == margin) { return; }
            DTU_LOGI("Setting battery standby power margin to: %d W", margin);
            config.PowerLimiter.BatteryStandbyPowerMargin = margin;
            break;
        }
        case MqttPowerLimiterCommand::BatteryTargetPowerConsumptionDynamicEnabled:
            if (config.PowerLimiter.BatteryTargetPowerConsumptionDynamicEnabled == static_cast<bool>(intValue)) { return; }
            DTU_LOGI("Setting dynamic battery target power consumption to: %s", intValue ? "enabled" : "disabled");
            config.PowerLimiter.BatteryTargetPowerConsumptionDynamicEnabled = intValue != 0;
            break;
        case MqttPowerLimiterCommand::BatteryTargetPowerConsumptionDynamicMax:
        {
            auto const maxValue = std::clamp(intValue, 0, static_cast<int>(std::numeric_limits<int16_t>::max()));
            if (config.PowerLimiter.BatteryTargetPowerConsumptionDynamicMax == maxValue) { return; }
            DTU_LOGI("Setting dynamic battery target power consumption max to: %d W", maxValue);
            config.PowerLimiter.BatteryTargetPowerConsumptionDynamicMax = maxValue;
            break;
        }
        case MqttPowerLimiterCommand::BatteryTargetPowerConsumptionDynamicMultiplier:
            if (config.PowerLimiter.BatteryTargetPowerConsumptionDynamicMultiplier == std::max(0.0f, payload_val)) { return; }
            DTU_LOGI("Setting dynamic battery target power consumption multiplier to: %.2f", std::max(0.0f, payload_val));
            config.PowerLimiter.BatteryTargetPowerConsumptionDynamicMultiplier = std::max(0.0f, payload_val);
            break;
        case MqttPowerLimiterCommand::BatteryTargetPowerConsumptionDynamicWindow:
            if (config.PowerLimiter.BatteryTargetPowerConsumptionDynamicWindow == std::max(0, intValue)) { return; }
            DTU_LOGI("Setting dynamic battery target power consumption window to: %d s", std::max(0, intValue));
            config.PowerLimiter.BatteryTargetPowerConsumptionDynamicWindow = std::max(0, intValue);
            break;
    }

    // not reached if the value did not change
    Configuration.write();
}
