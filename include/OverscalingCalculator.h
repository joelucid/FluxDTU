// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

/**
 * Calculates overscaling for power limiter inverters
 * Extracted from PowerLimiterOverscalingInverter to make it testable
 */
class OverscalingCalculator {
public:
    struct MpptData {
        float powerAC;  // AC power output of this MPPT
    };

    struct ChannelData {
        float powerAC;  // AC-equivalent power output of this DC input channel
    };

    /**
     * Calculate the overscaled limit based on MPPT shading
     * @param currentLimitWatts Current power limit in watts
     * @param newExpectedOutputWatts New expected output power in watts
     * @param mpptData Vector of MPPT data with AC power outputs
     * @param inverterMaxPower Maximum inverter power in watts
     * @param currentThreshold Threshold factor for current limit (0.8 for low power, 0.97 for high power)
     * @param newThreshold Threshold factor for new limit (0.8 for low power, 0.97 for high power)
     * @return The calculated overscaled limit
     */
    static uint16_t calculateOverscaledLimit(uint16_t currentLimitWatts, uint16_t newExpectedOutputWatts,
                                           const std::vector<MpptData>& mpptData, uint16_t inverterMaxPower,
                                           float currentThreshold, float newThreshold);

    /**
     * Calculate an overscaled limit for a desired output cap using that cap's
     * own MPPT threshold. This is used when reducing an already-overscaled
     * limit: current limit thresholds can mark every MPPT as shaded and would
     * keep the old high limit instead of calculating the scaled cap.
     * @param expectedOutputWatts Desired AC output cap
     * @param mpptData Vector of MPPT data with AC power outputs
     * @param inverterMaxPower Maximum inverter power in watts
     * @param threshold Threshold factor for expected output
     * @return The calculated overscaled limit
     */
    static uint16_t calculateOverscaledLimitForExpectedOutput(
            uint16_t expectedOutputWatts,
            const std::vector<MpptData>& mpptData,
            uint16_t inverterMaxPower,
            float threshold);

    /**
     * Calculate the maximum AC output that can still be expected from the
     * current MPPT shading pattern once the total inverter limit reaches its
     * absolute maximum.
     * @param mpptData Vector of MPPT data
     * @param inverterMaxPower Maximum inverter power in watts
     * @param expectedPowerPerMppt Expected power per MPPT for shading detection
     */
    static uint16_t calculateMaxCompensatedOutput(
            const std::vector<MpptData>& mpptData,
            uint16_t inverterMaxPower,
            float expectedPowerPerMppt);

    /**
     * Return true when at least one MPPT is below the expected power and at
     * least one other MPPT is still above it, so overscaling can shift usable
     * limit to the stronger inputs.
     * @param mpptData Vector of MPPT data
     * @param expectedPowerPerMppt Expected power per MPPT
     */
    static bool hasCompensableShading(const std::vector<MpptData>& mpptData,
                                      float expectedPowerPerMppt);

    /**
     * Calculate the expected AC output for inverters where an AC limit is
     * applied as the same percentage to every DC input channel.
     * @param currentLimitWatts Current total AC limit used to detect bound channels
     * @param outputLimitWatts Total AC limit to simulate
     * @param channelData Vector of channel data with AC-equivalent power outputs
     * @param limitBindingThreshold Threshold factor for detecting a bound channel
     */
    static uint16_t calculatePerChannelLimitOutput(
            uint16_t currentLimitWatts,
            uint16_t outputLimitWatts,
            const std::vector<ChannelData>& channelData,
            float limitBindingThreshold);

    /**
     * Calculate an overscaled AC limit for inverters where the configured AC
     * limit is applied uniformly to every DC input channel.
     * @param currentLimitWatts Current total AC limit used to detect bound channels
     * @param expectedOutputWatts Desired AC output
     * @param channelData Vector of channel data with AC-equivalent power outputs
     * @param inverterMaxPower Maximum inverter power in watts
     * @param limitBindingThreshold Threshold factor for detecting a bound channel
     */
    static uint16_t calculatePerChannelOverscaledLimit(
            uint16_t currentLimitWatts,
            uint16_t expectedOutputWatts,
            const std::vector<ChannelData>& channelData,
            uint16_t inverterMaxPower,
            float limitBindingThreshold);

    /**
     * Calculate the maximum AC output that can be expected from the current
     * per-channel limit binding pattern once the total inverter limit reaches
     * its absolute maximum.
     */
    static uint16_t calculateMaxPerChannelOutput(
            uint16_t currentLimitWatts,
            const std::vector<ChannelData>& channelData,
            uint16_t inverterMaxPower,
            float limitBindingThreshold);

    /**
     * Return true when at least one DC input channel is currently close enough
     * to its per-channel share of the AC limit to treat it as limit-bound.
     */
    static bool hasLimitBoundChannel(
            uint16_t currentLimitWatts,
            const std::vector<ChannelData>& channelData,
            float limitBindingThreshold);

    /**
     * Count DC input channels that are currently close enough to their
     * per-channel share of the AC limit to be treated as limit-bound.
     * @return Pair of (bound count, bound power sum)
     */
    static std::pair<size_t, float> countLimitBoundChannels(
            uint16_t currentLimitWatts,
            const std::vector<ChannelData>& channelData,
            float limitBindingThreshold);

    /**
     * Count shaded MPPTs based on expected power per MPPT
     * @param mpptData Vector of MPPT data
     * @param expectedPowerPerMppt Expected power per MPPT
     * @return Pair of (shaded count, total shaded power)
     */
    static std::pair<size_t, float> countShadedMppts(const std::vector<MpptData>& mpptData,
                                                     float expectedPowerPerMppt);
};
