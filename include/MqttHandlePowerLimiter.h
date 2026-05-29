// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Configuration.h"
#include <espMqttClient.h>
#include <TaskSchedulerDeclarations.h>
#include <mutex>
#include <deque>
#include <functional>
#include <frozen/map.h>
#include <frozen/string.h>

class MqttHandlePowerLimiterClass {
public:
    void init(Scheduler& scheduler);

    void forceUpdate();

    void subscribeTopics();
    void unsubscribeTopics();

private:
    void loop();

    enum class MqttPowerLimiterCommand : unsigned {
        Mode,
        BatterySoCStartThreshold,
        BatterySoCStopThreshold,
        FullSolarPassthroughSoC,
        VoltageStartThreshold,
        VoltageStopThreshold,
        FullSolarPassThroughStartVoltage,
        FullSolarPassThroughStopVoltage,
        UpperPowerLimit,
        TargetPowerConsumption,
        TargetPowerConsumptionFollowStorageTarget,
        TargetPowerConsumptionStorageOffset,
        BatteryTargetPowerConsumption,
        BatteryStandbyPowerMargin,
        BatteryTargetPowerConsumptionDynamicEnabled,
        BatteryTargetPowerConsumptionDynamicMax,
        BatteryTargetPowerConsumptionDynamicMultiplier,
        BatteryTargetPowerConsumptionDynamicWindow
    };

    static constexpr frozen::string _cmdtopic = "powerlimiter/cmd/";
    static constexpr frozen::map<frozen::string, MqttPowerLimiterCommand, 18> _subscriptions = {
        { "threshold/soc/start",                            MqttPowerLimiterCommand::BatterySoCStartThreshold },
        { "threshold/soc/stop",                             MqttPowerLimiterCommand::BatterySoCStopThreshold },
        { "threshold/soc/full_solar_passthrough",           MqttPowerLimiterCommand::FullSolarPassthroughSoC },
        { "threshold/voltage/start",                        MqttPowerLimiterCommand::VoltageStartThreshold },
        { "threshold/voltage/stop",                         MqttPowerLimiterCommand::VoltageStopThreshold },
        { "threshold/voltage/full_solar_passthrough_start", MqttPowerLimiterCommand::FullSolarPassThroughStartVoltage },
        { "threshold/voltage/full_solar_passthrough_stop",  MqttPowerLimiterCommand::FullSolarPassThroughStopVoltage },
        { "mode",                                           MqttPowerLimiterCommand::Mode },
        { "upper_power_limit",                              MqttPowerLimiterCommand::UpperPowerLimit },
        { "target_power_consumption",                       MqttPowerLimiterCommand::TargetPowerConsumption },
        { "target_power_consumption_follow_storage_target",  MqttPowerLimiterCommand::TargetPowerConsumptionFollowStorageTarget },
        { "target_power_consumption_storage_offset",         MqttPowerLimiterCommand::TargetPowerConsumptionStorageOffset },
        { "battery_target_power_consumption",               MqttPowerLimiterCommand::BatteryTargetPowerConsumption },
        { "battery_standby_power_margin",                   MqttPowerLimiterCommand::BatteryStandbyPowerMargin },
        { "battery_target_power_consumption_dynamic_enabled", MqttPowerLimiterCommand::BatteryTargetPowerConsumptionDynamicEnabled },
        { "battery_target_power_consumption_dynamic_max",    MqttPowerLimiterCommand::BatteryTargetPowerConsumptionDynamicMax },
        { "battery_target_power_consumption_dynamic_multiplier", MqttPowerLimiterCommand::BatteryTargetPowerConsumptionDynamicMultiplier },
        { "battery_target_power_consumption_dynamic_window", MqttPowerLimiterCommand::BatteryTargetPowerConsumptionDynamicWindow },
    };

    void onMqttCmd(MqttPowerLimiterCommand command, const espMqttClientTypes::MessageProperties& properties, const char* topic, const uint8_t* payload, size_t len);

    Task _loopTask;

    uint32_t _lastPublishStats;
    uint32_t _lastPublish;

    // MQTT callbacks to process updates on subscribed topics are executed in
    // the MQTT thread's context. we use this queue to switch processing the
    // user requests into the main loop's context (TaskScheduler context).
    mutable std::mutex _mqttMutex;
    std::deque<std::function<void()>> _mqttCallbacks;
};

extern MqttHandlePowerLimiterClass MqttHandlePowerLimiter;
