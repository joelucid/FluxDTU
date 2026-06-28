// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace GridChargers {

constexpr uint32_t PowerLimiterNormalTargetEffectAssumptionMillis = 2 * 1000;
constexpr uint32_t PowerLimiterStartupTargetEffectAssumptionMillis = 5 * 1000;

constexpr uint32_t powerLimiterTargetEffectAssumptionMillis(
        uint16_t previousInputPowerWatts,
        uint16_t targetInputPowerWatts)
{
    auto const crossesStandbyBoundary =
        (previousInputPowerWatts == 0 && targetInputPowerWatts > 0)
        || (previousInputPowerWatts > 0 && targetInputPowerWatts == 0);
    return crossesStandbyBoundary
        ? PowerLimiterStartupTargetEffectAssumptionMillis
        : PowerLimiterNormalTargetEffectAssumptionMillis;
}

} // namespace GridChargers
