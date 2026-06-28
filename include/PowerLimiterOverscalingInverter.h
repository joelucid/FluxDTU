// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "PowerLimiterInverter.h"
#include "OverscalingCalculator.h"

class PowerLimiterOverscalingInverter : public PowerLimiterInverter {
public:
    explicit PowerLimiterOverscalingInverter(PowerLimiterInverterConfig const& config);

    uint16_t applyIncrease(uint16_t increase) final;

protected:
    void setAcOutput(uint16_t expectedOutputWatts) final;
    void setAcOutputAndLimit(uint16_t expectedOutputWatts, uint16_t targetLimitWatts);
    bool overscalingEnabled() const;
    bool usesPerChannelLimitModel() const;
    bool isPerChannelLimitBinding(uint16_t limitWatts) const;
    bool hasCompensableLimitHeadroom(uint16_t limitWatts) const;
    float calculateRequiredOutputThreshold(uint16_t limitWatts) const;
    float calculateDcChannelPowerAC(ChannelNum_t channel) const;
    float calculateMpptPowerAC(MpptNum_t mppt) const;
    std::vector<OverscalingCalculator::ChannelData> collectChannelData() const;
    std::vector<OverscalingCalculator::MpptData> collectMpptData() const;
    uint16_t scaleLimit(uint16_t newExpectedOutputWatts);
    uint16_t scaleLimitForOutputCap(uint16_t expectedOutputWatts);
    uint16_t getMaxOverscaledOutputWatts(uint16_t limitWatts) const;
};
