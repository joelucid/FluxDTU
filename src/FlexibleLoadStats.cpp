// SPDX-License-Identifier: GPL-2.0-or-later

#include "FlexibleLoadStats.h"
#include "MqttSettings.h"
#include "Utils.h"
#include <LogHelper.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <string>

#undef TAG
static const char* TAG = "flexibleLoadStats";
static const char* SUBTAG = "MQTT";

FlexibleLoadStatsClass FlexibleLoadStats;

void FlexibleLoadStatsClass::init()
{
    updateSettings();
}

void FlexibleLoadStatsClass::updateSettings()
{
    std::lock_guard<std::mutex> lock(_mutex);

    for (auto const& topic : _subscribedTopics) {
        MqttSettings.unsubscribe(topic);
    }
    _subscribedTopics.clear();

    for (auto& load : _loads) {
        load = LoadState();
    }

    auto const& config = Configuration.get().PowerLimiter;
    for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
        auto const& loadConfig = config.FlexibleLoads[i];
        if (std::strlen(loadConfig.PowerMqttTopic) == 0) { continue; }

        String topic = loadConfig.PowerMqttTopic;
        _loads[i].topic = topic;
        _subscribedTopics.push_back(topic);

        MqttSettings.subscribe(topic, 0,
                std::bind(&FlexibleLoadStatsClass::onMqttMessage,
                    this,
                    std::placeholders::_1,
                    std::placeholders::_2,
                    std::placeholders::_3,
                    std::placeholders::_4,
                    i));

        DTU_LOGI("subscribed to '%s' for flexible load %u power readings", topic.c_str(), i + 1);
    }
}

std::optional<float> FlexibleLoadStatsClass::getPowerWatts(uint8_t index) const
{
    if (index >= POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT) { return std::nullopt; }

    std::lock_guard<std::mutex> lock(_mutex);
    auto const& load = _loads[index];
    if (load.lastUpdate == 0 || (millis() - load.lastUpdate) > (60 * 1000)) { return std::nullopt; }

    return load.powerWatts;
}

uint32_t FlexibleLoadStatsClass::getLastUpdateMillis() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    uint32_t lastUpdate = 0;
    for (auto const& load : _loads) {
        if ((load.lastUpdate - lastUpdate) < (std::numeric_limits<uint32_t>::max() / 2)) {
            lastUpdate = load.lastUpdate;
        }
    }

    return lastUpdate;
}

void FlexibleLoadStatsClass::onMqttMessage(MsgProperties const& properties,
        char const* topic,
        uint8_t const* payload,
        size_t len,
        uint8_t index)
{
    (void)properties;

    if (index >= POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT) { return; }

    auto const& config = Configuration.get().PowerLimiter.FlexibleLoads[index];
    auto extracted = Utils::getNumericValueFromMqttPayload<float>("FlexibleLoadStats",
            std::string(reinterpret_cast<char const*>(payload), len), topic,
            config.PowerMqttJsonPath);

    if (!extracted.has_value()) { return; }

    float powerWatts = *extracted;
    switch (config.PowerMqttUnit) {
        case PowerLimiterFlexibleLoadConfig::PowerUnit::MilliWatts:
            powerWatts /= 1000.0f;
            break;
        case PowerLimiterFlexibleLoadConfig::PowerUnit::KiloWatts:
            powerWatts *= 1000.0f;
            break;
        default:
            break;
    }

    if (config.PowerMqttSignInverted) { powerWatts *= -1.0f; }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _loads[index].powerWatts = powerWatts;
        _loads[index].lastUpdate = millis();
    }

    DTU_LOGD("flexible load %u topic '%s': %.1f W", index + 1, topic, powerWatts);
}
