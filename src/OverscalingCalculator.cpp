#include "OverscalingCalculator.h"
#include <algorithm>
#include <cmath>

namespace {

float sumChannelPower(const std::vector<OverscalingCalculator::ChannelData>& channelData)
{
    float output = 0.0f;
    for (auto const& channel : channelData) {
        output += channel.powerAC;
    }
    return output;
}

std::vector<bool> collectLimitBoundChannels(
        uint16_t currentLimitWatts,
        const std::vector<OverscalingCalculator::ChannelData>& channelData,
        float limitBindingThreshold)
{
    std::vector<bool> limitBoundChannels;
    limitBoundChannels.reserve(channelData.size());

    auto const totalChannels = channelData.size();
    if (totalChannels == 0) {
        return limitBoundChannels;
    }
    if (currentLimitWatts == 0) {
        limitBoundChannels.resize(totalChannels, false);
        return limitBoundChannels;
    }

    auto const threshold = std::clamp(limitBindingThreshold, 0.0f, 1.0f);
    auto const currentPowerPerChannel =
        static_cast<float>(currentLimitWatts) / static_cast<float>(totalChannels);
    auto const requiredPower = currentPowerPerChannel * threshold;
    for (auto const& channel : channelData) {
        limitBoundChannels.push_back(channel.powerAC >= requiredPower);
    }

    return limitBoundChannels;
}

float calculatePerChannelLimitOutputFloat(
        uint16_t currentLimitWatts,
        uint16_t outputLimitWatts,
        const std::vector<OverscalingCalculator::ChannelData>& channelData,
        float limitBindingThreshold)
{
    auto const totalChannels = channelData.size();
    if (totalChannels == 0) { return 0.0f; }

    auto const limitBoundChannels =
        collectLimitBoundChannels(currentLimitWatts, channelData, limitBindingThreshold);
    if (limitBoundChannels.size() != totalChannels) {
        return sumChannelPower(channelData);
    }

    auto const powerPerChannel =
        static_cast<float>(outputLimitWatts) / static_cast<float>(totalChannels);
    float output = 0.0f;
    for (size_t i = 0; i < totalChannels; i++) {
        output += limitBoundChannels[i]
            ? powerPerChannel
            : std::min(channelData[i].powerAC, powerPerChannel);
    }

    return std::clamp<float>(
            output,
            0.0f,
            static_cast<float>(outputLimitWatts));
}

} // namespace

uint16_t OverscalingCalculator::calculateOverscaledLimit(uint16_t currentLimitWatts, uint16_t newExpectedOutputWatts,
                                                        const std::vector<MpptData>& mpptData, uint16_t inverterMaxPower,
                                                        float currentThreshold, float newThreshold) {
    // ============================================================================
    // OVERSCALING ALGORITHM
    // ============================================================================
    // Purpose: Compensate for shaded panels by increasing the total power limit
    //
    // How it works:
    // 1. Detect which MPPTs are shaded (producing less than expected)
    // 2. Calculate how much extra power non-shaded MPPTs can provide
    // 3. Distribute this extra power across all MPPTs to increase the total limit
    //
    // Note: This should not be used when Hoymiles 'Power Distribution Logic'
    // is available, as the inverter handles power distribution itself.
    // ============================================================================

    const size_t totalMppts = mpptData.size();

    // Calculate current actual output by summing MPPT power
    float currentActualOutput = 0.0f;
    for (const auto& mppt : mpptData) {
        currentActualOutput += mppt.powerAC;
    }

    // Calculate expected power per MPPT for both current and new limits
    const float currentExpectedPerMppt = (currentLimitWatts / totalMppts) * currentThreshold;
    const float newExpectedPerMppt = (newExpectedOutputWatts / totalMppts) * newThreshold;

    // ============================================================================
    // STEP 1: DETECT SHADING
    // ============================================================================
    // Determine which MPPTs are shaded based on power direction:
    // - Increasing power: Check current shading state (we know there's shading now)
    // - Decreasing power: Check new shading state (what we're transitioning to)
    //
    // Use actual current output to determine direction (handles overscaled limits)
    // Use current limit to calculate expected power per MPPT (for shading detection)

    const bool isIncreasingPower = newExpectedOutputWatts > currentActualOutput;
    size_t shadedMpptCount;
    float shadedMpptPowerSum;

    if (isIncreasingPower) {
        // When increasing power: only check current shading
        auto [count, powerSum] = countShadedMppts(mpptData, currentExpectedPerMppt);
        shadedMpptCount = count;
        shadedMpptPowerSum = powerSum;
    } else {
        // When decreasing power: only check new shading state
        auto [count, powerSum] = countShadedMppts(mpptData, newExpectedPerMppt);
        shadedMpptCount = count;
        shadedMpptPowerSum = powerSum;
    }

    // ============================================================================
    // STEP 2: VALIDATE SHADING CONDITIONS
    // ============================================================================

    // No shading detected - no overscaling needed
    if (shadedMpptCount == 0) {
        return newExpectedOutputWatts;
    }

    // All MPPTs are shaded - cannot compensate with overscaling
    const size_t nonShadedMpptCount = totalMppts - shadedMpptCount;
    if (nonShadedMpptCount == 0) {
        // If new limit is higher, apply it anyway (MPPTs might still produce more)
        // If new limit is lower, keep current limit (don't reduce unnecessarily)
        return (newExpectedOutputWatts > currentLimitWatts) ? newExpectedOutputWatts : currentLimitWatts;
    }

    // ============================================================================
    // STEP 3: CALCULATE OVERSCALED LIMIT
    // ============================================================================
    // Formula:
    // 1. Calculate how much power each non-shaded MPPT should provide
    // 2. Multiply by total MPPTs to get the new limit
    // 3. Apply inverter maximum power constraint

    // Calculate power per non-shaded MPPT
    const uint16_t powerPerNonShadedMppt = (newExpectedOutputWatts - shadedMpptPowerSum) / nonShadedMpptCount;

    // Calculate total overscaled limit
    uint16_t overscaledLimit = powerPerNonShadedMppt * totalMppts;

    // Apply inverter maximum power constraint
    overscaledLimit = std::min(overscaledLimit, inverterMaxPower);

    // ============================================================================
    // STEP 4: RETURN RESULT
    // ============================================================================
    // Only return overscaled limit if it's actually higher than expected
    return (overscaledLimit > newExpectedOutputWatts) ? overscaledLimit : newExpectedOutputWatts;
}

uint16_t OverscalingCalculator::calculateOverscaledLimitForExpectedOutput(
        uint16_t expectedOutputWatts,
        const std::vector<MpptData>& mpptData,
        uint16_t inverterMaxPower,
        float threshold) {
    const size_t totalMppts = mpptData.size();
    if (totalMppts <= 1) {
        return expectedOutputWatts;
    }

    const float expectedPowerPerMppt = (expectedOutputWatts / totalMppts) * threshold;
    auto const [shadedMpptCount, shadedMpptPowerSum] =
        countShadedMppts(mpptData, expectedPowerPerMppt);
    if (shadedMpptCount == 0 || shadedMpptCount == totalMppts) {
        return expectedOutputWatts;
    }

    if (shadedMpptPowerSum >= expectedOutputWatts) {
        return expectedOutputWatts;
    }

    const size_t nonShadedMpptCount = totalMppts - shadedMpptCount;
    const uint16_t powerPerNonShadedMppt =
        (expectedOutputWatts - shadedMpptPowerSum) / nonShadedMpptCount;
    uint16_t overscaledLimit = powerPerNonShadedMppt * totalMppts;
    overscaledLimit = std::min(overscaledLimit, inverterMaxPower);
    return (overscaledLimit > expectedOutputWatts) ? overscaledLimit : expectedOutputWatts;
}

uint16_t OverscalingCalculator::calculateMaxCompensatedOutput(
        const std::vector<MpptData>& mpptData,
        uint16_t inverterMaxPower,
        float expectedPowerPerMppt) {
    const size_t totalMppts = mpptData.size();
    if (totalMppts <= 1 || inverterMaxPower == 0) {
        return inverterMaxPower;
    }

    auto const [shadedMpptCount, shadedMpptPowerSum] =
        countShadedMppts(mpptData, expectedPowerPerMppt);
    if (shadedMpptCount == 0 || shadedMpptCount == totalMppts) {
        return inverterMaxPower;
    }

    const size_t nonShadedMpptCount = totalMppts - shadedMpptCount;
    auto const maxPowerPerMppt =
        static_cast<float>(inverterMaxPower) / static_cast<float>(totalMppts);
    auto const maxOutput =
        shadedMpptPowerSum + (maxPowerPerMppt * nonShadedMpptCount);

    return static_cast<uint16_t>(
            std::clamp<float>(maxOutput, 0.0f, inverterMaxPower));
}

std::pair<size_t, float> OverscalingCalculator::countShadedMppts(const std::vector<MpptData>& mpptData,
                                                                float expectedPowerPerMppt) {
    // Count MPPTs that are producing less power than expected (shaded)
    size_t shadedCount = 0;
    float shadedPowerSum = 0.0f;

    for (const auto& mppt : mpptData) {
        if (mppt.powerAC < expectedPowerPerMppt) {
            shadedCount++;
            shadedPowerSum += mppt.powerAC;
        }
    }

    return {shadedCount, shadedPowerSum};
}

bool OverscalingCalculator::hasCompensableShading(
        const std::vector<MpptData>& mpptData,
        float expectedPowerPerMppt) {
    if (mpptData.size() <= 1) {
        return false;
    }

    auto const shaded = countShadedMppts(mpptData, expectedPowerPerMppt);
    auto const shadedCount = shaded.first;

    return shadedCount > 0 && shadedCount < mpptData.size();
}

uint16_t OverscalingCalculator::calculatePerChannelLimitOutput(
        uint16_t currentLimitWatts,
        uint16_t outputLimitWatts,
        const std::vector<ChannelData>& channelData,
        float limitBindingThreshold)
{
    return static_cast<uint16_t>(calculatePerChannelLimitOutputFloat(
            currentLimitWatts,
            outputLimitWatts,
            channelData,
            limitBindingThreshold));
}

uint16_t OverscalingCalculator::calculatePerChannelOverscaledLimit(
        uint16_t currentLimitWatts,
        uint16_t expectedOutputWatts,
        const std::vector<ChannelData>& channelData,
        uint16_t inverterMaxPower,
        float limitBindingThreshold)
{
    if (channelData.size() <= 1 || inverterMaxPower == 0) {
        return expectedOutputWatts;
    }

    if (!hasLimitBoundChannel(currentLimitWatts, channelData, limitBindingThreshold)) {
        return expectedOutputWatts;
    }

    if (expectedOutputWatts >= inverterMaxPower) {
        return inverterMaxPower;
    }

    if (calculatePerChannelLimitOutputFloat(
                currentLimitWatts,
                expectedOutputWatts,
                channelData,
                limitBindingThreshold)
            >= static_cast<float>(expectedOutputWatts)) {
        return expectedOutputWatts;
    }

    if (calculatePerChannelLimitOutputFloat(
                currentLimitWatts,
                inverterMaxPower,
                channelData,
                limitBindingThreshold)
            < static_cast<float>(expectedOutputWatts)) {
        return inverterMaxPower;
    }

    uint16_t low = expectedOutputWatts;
    uint16_t high = inverterMaxPower;
    while (low < high) {
        auto const mid = static_cast<uint16_t>(
                low + ((high - low) / 2));
        if (calculatePerChannelLimitOutputFloat(
                    currentLimitWatts,
                    mid,
                    channelData,
                    limitBindingThreshold)
                >= static_cast<float>(expectedOutputWatts)) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }

    return low;
}

uint16_t OverscalingCalculator::calculateMaxPerChannelOutput(
        uint16_t currentLimitWatts,
        const std::vector<ChannelData>& channelData,
        uint16_t inverterMaxPower,
        float limitBindingThreshold)
{
    if (channelData.size() <= 1 || inverterMaxPower == 0) {
        return inverterMaxPower;
    }

    if (!hasLimitBoundChannel(currentLimitWatts, channelData, limitBindingThreshold)) {
        return static_cast<uint16_t>(
                std::clamp<float>(
                        sumChannelPower(channelData),
                        0.0f,
                        static_cast<float>(inverterMaxPower)));
    }

    return calculatePerChannelLimitOutput(
            currentLimitWatts,
            inverterMaxPower,
            channelData,
            limitBindingThreshold);
}

bool OverscalingCalculator::hasLimitBoundChannel(
        uint16_t currentLimitWatts,
        const std::vector<ChannelData>& channelData,
        float limitBindingThreshold)
{
    return countLimitBoundChannels(
            currentLimitWatts,
            channelData,
            limitBindingThreshold).first > 0;
}

std::pair<size_t, float> OverscalingCalculator::countLimitBoundChannels(
        uint16_t currentLimitWatts,
        const std::vector<ChannelData>& channelData,
        float limitBindingThreshold)
{
    auto const limitBoundChannels =
        collectLimitBoundChannels(currentLimitWatts, channelData, limitBindingThreshold);
    if (limitBoundChannels.size() != channelData.size()) {
        return { 0, 0.0f };
    }

    size_t boundCount = 0;
    float boundPowerSum = 0.0f;
    for (size_t i = 0; i < channelData.size(); i++) {
        if (!limitBoundChannels[i]) { continue; }

        boundCount++;
        boundPowerSum += channelData[i].powerAC;
    }

    return { boundCount, boundPowerSum };
}
