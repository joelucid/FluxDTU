// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace FlexibleLoadAccounting {

inline float allocateSharedPowerToLoad(
        std::optional<float> measuredLoadPowerWatts,
        float measuredRunningLoadPowerWatts,
        float totalSharedPowerWatts)
{
    if (totalSharedPowerWatts <= 0.0f || !std::isfinite(totalSharedPowerWatts)) {
        return 0.0f;
    }

    if (!measuredLoadPowerWatts) {
        return measuredRunningLoadPowerWatts > 0.0f ? 0.0f : totalSharedPowerWatts;
    }

    auto const loadPower = std::max(0.0f, *measuredLoadPowerWatts);
    if (loadPower <= 0.0f || measuredRunningLoadPowerWatts <= 0.0f) {
        return 0.0f;
    }

    auto const loadShare = loadPower / measuredRunningLoadPowerWatts;
    return std::min(loadPower, totalSharedPowerWatts * loadShare);
}

} // namespace FlexibleLoadAccounting
