// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>

namespace PowerLimiterBatteryCurrentLimit {

inline float peakHeadroomAmps(float continuousLimit, float peakLimit)
{
    return std::max(0.0f, peakLimit - continuousLimit);
}

inline float budgetCapacityAmpSeconds(
        float continuousLimit,
        float peakLimit,
        float peakDurationSeconds)
{
    return peakHeadroomAmps(continuousLimit, peakLimit)
        * std::max(0.0f, peakDurationSeconds);
}

inline float currentLimitAmps(
        float continuousLimit,
        float peakLimit,
        float peakDurationSeconds,
        float budgetAmpSeconds)
{
    auto const peakHeadroom = peakHeadroomAmps(continuousLimit, peakLimit);
    if (peakHeadroom <= 0.0f || peakDurationSeconds <= 0.0f) {
        return continuousLimit;
    }

    auto const budgetCapacity = budgetCapacityAmpSeconds(
            continuousLimit,
            peakLimit,
            peakDurationSeconds);
    auto const budget = std::clamp(
            budgetAmpSeconds,
            0.0f,
            budgetCapacity);
    if (budget <= 0.0f) {
        return continuousLimit;
    }

    // Project the remaining peak budget over the configured peak duration.
    // This keeps the data-sheet amp-second budget but avoids a hard final edge.
    auto const projectedHeadroom = budget / peakDurationSeconds;
    return std::clamp(
            continuousLimit + projectedHeadroom,
            continuousLimit,
            peakLimit);
}

} // namespace PowerLimiterBatteryCurrentLimit
