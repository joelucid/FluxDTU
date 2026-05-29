// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Configuration.h"
#include <Arduino.h>
#include <MqttSubscribeParser.h>
#include <optional>
#include <mutex>
#include <vector>

class FlexibleLoadStatsClass {
public:
    void init();
    void updateSettings();

    std::optional<float> getPowerWatts(uint8_t index) const;
    uint32_t getLastUpdateMillis() const;

private:
    using MsgProperties = espMqttClientTypes::MessageProperties;

    struct LoadState {
        String topic;
        float powerWatts = 0.0f;
        uint32_t lastUpdate = 0;
    };

    void onMqttMessage(MsgProperties const& properties,
            char const* topic,
            uint8_t const* payload,
            size_t len,
            uint8_t index);

    mutable std::mutex _mutex;
    LoadState _loads[POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT];
    std::vector<String> _subscribedTopics;
};

extern FlexibleLoadStatsClass FlexibleLoadStats;
