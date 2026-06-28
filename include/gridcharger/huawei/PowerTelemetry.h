// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <optional>

namespace GridChargers::Huawei {

constexpr float HuaweiDefaultEfficiency = 0.9f;
constexpr float HuaweiValidEfficiencyPercentMin = 50.0f;
constexpr float HuaweiPowerActiveThresholdWatts = 1.0f;

inline float normalizeEfficiency(std::optional<float> efficiencyPercent)
{
    if (efficiencyPercent && *efficiencyPercent > HuaweiValidEfficiencyPercentMin) {
        return std::clamp(*efficiencyPercent / 100.0f, 0.5f, 1.0f);
    }

    return HuaweiDefaultEfficiency;
}

inline std::optional<float> estimateOutputPowerWatts(
        std::optional<float> outputPowerWatts,
        std::optional<float> outputVoltage,
        std::optional<float> outputCurrent)
{
    if (outputVoltage
            && *outputVoltage > 0.0f
            && outputCurrent) {
        return std::max(0.0f, *outputVoltage * std::max(0.0f, *outputCurrent));
    }

    auto const measuredOutputPower = outputPowerWatts
        ? std::max(0.0f, *outputPowerWatts)
        : 0.0f;
    if (measuredOutputPower > HuaweiPowerActiveThresholdWatts) {
        return measuredOutputPower;
    }

    if (outputPowerWatts) { return measuredOutputPower; }

    return std::nullopt;
}

inline std::optional<float> estimateInputPowerWatts(
        std::optional<float> inputPowerWatts,
        std::optional<float> outputPowerWatts,
        std::optional<float> outputVoltage,
        std::optional<float> outputCurrent,
        std::optional<float> efficiencyPercent)
{
    auto const outputPower = estimateOutputPowerWatts(
            outputPowerWatts,
            outputVoltage,
            outputCurrent);
    auto const derivedInputPower = outputPower
        ? std::optional<float>(*outputPower / normalizeEfficiency(efficiencyPercent))
        : std::nullopt;

    if (!inputPowerWatts) { return derivedInputPower; }

    auto const measuredInputPower = std::max(0.0f, *inputPowerWatts);
    if (measuredInputPower > HuaweiPowerActiveThresholdWatts
            || !derivedInputPower
            || *derivedInputPower <= HuaweiPowerActiveThresholdWatts) {
        return measuredInputPower;
    }

    return derivedInputPower;
}

} // namespace GridChargers::Huawei
