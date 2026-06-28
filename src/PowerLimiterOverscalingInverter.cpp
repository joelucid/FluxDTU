#include "PowerLimiterOverscalingInverter.h"
#include "OverscalingCalculator.h"
#include <LogHelper.h>
#include <algorithm>

#undef TAG
static const char* TAG = "dynamicPowerLimiter";
#define SUBTAG _logPrefix

namespace {

static constexpr float sPerChannelLimitBindingThreshold = 0.95f;

}

PowerLimiterOverscalingInverter::PowerLimiterOverscalingInverter(PowerLimiterInverterConfig const& config)
    : PowerLimiterInverter(config) { }

float PowerLimiterOverscalingInverter::calculateRequiredOutputThreshold(uint16_t limitWatts) const
{
    // above 15% we can apply our simple 97% percent rule
    float threshold = 0.97;

    // if the limit is 15% or below, we use a lower threshold
    // of 80% to compensate for the lower efficiency at low power.
    if (limitWatts <= getInverterMaxPowerWatts() * 0.15) {
        threshold = 0.8;
    }

    return threshold;
}

float PowerLimiterOverscalingInverter::calculateDcChannelPowerAC(ChannelNum_t channel) const
{
    auto pStats = _spInverter->Statistics();
    if (!pStats->hasChannelFieldValue(TYPE_INV, CH0, FLD_EFF)) {
        return 0.0f;
    }

    if (!pStats->hasChannelFieldValue(TYPE_DC, channel, FLD_PDC)) {
        return 0.0f;
    }

    auto const inverterEfficiencyFactor = std::clamp(
            pStats->getChannelFieldValue(TYPE_INV, CH0, FLD_EFF) / 100.0f,
            0.0f,
            1.0f);
    return pStats->getChannelFieldValue(TYPE_DC, channel, FLD_PDC)
        * inverterEfficiencyFactor;
}

float PowerLimiterOverscalingInverter::calculateMpptPowerAC(MpptNum_t mppt) const
{
    float mpptPowerAC = 0.0;

    std::vector<ChannelNum_t> mpptChnls = _spInverter->getChannelsDCByMppt(mppt);
    for (auto& c : mpptChnls) {
        mpptPowerAC += calculateDcChannelPowerAC(c);
    }

    return mpptPowerAC;
}

std::vector<OverscalingCalculator::ChannelData> PowerLimiterOverscalingInverter::collectChannelData() const
{
    std::vector<OverscalingCalculator::ChannelData> channelData;
    for (auto& channel : _spInverter->getChannelsDC()) {
        channelData.push_back({ calculateDcChannelPowerAC(channel) });
    }
    return channelData;
}

std::vector<OverscalingCalculator::MpptData> PowerLimiterOverscalingInverter::collectMpptData() const
{
    std::vector<OverscalingCalculator::MpptData> mpptData;
    for (auto& mppt : _spInverter->getMppts()) {
        mpptData.push_back({ calculateMpptPowerAC(mppt) });
    }
    return mpptData;
}

bool PowerLimiterOverscalingInverter::usesPerChannelLimitModel() const
{
    if (!overscalingEnabled()) { return false; }

    auto const dcTotalChnls = _spInverter->getChannelsDC().size();
    auto const dcTotalMppts = _spInverter->getMppts().size();
    return dcTotalChnls > 1 && dcTotalChnls > dcTotalMppts;
}

bool PowerLimiterOverscalingInverter::isPerChannelLimitBinding(uint16_t limitWatts) const
{
    if (!usesPerChannelLimitModel()) { return false; }
    if (!isProducing()) { return false; }

    auto const channelData = collectChannelData();
    return OverscalingCalculator::hasLimitBoundChannel(
            limitWatts,
            channelData,
            sPerChannelLimitBindingThreshold);
}

bool PowerLimiterOverscalingInverter::hasCompensableLimitHeadroom(uint16_t limitWatts) const
{
    if (!overscalingEnabled()) { return false; }
    if (!isProducing()) { return false; }

    auto const dcTotalChnls = _spInverter->getChannelsDC().size();
    if (limitWatts < dcTotalChnls * 10) { return false; }

    if (usesPerChannelLimitModel()) {
        auto const channelData = collectChannelData();
        if (channelData.size() <= 1) { return false; }
        if (!OverscalingCalculator::hasLimitBoundChannel(
                    limitWatts,
                    channelData,
                    sPerChannelLimitBindingThreshold)) {
            return false;
        }

        return OverscalingCalculator::calculateMaxPerChannelOutput(
                limitWatts,
                channelData,
                getInverterMaxPowerWatts(),
                sPerChannelLimitBindingThreshold) > getExpectedOutputAcWatts();
    }

    auto const mpptData = collectMpptData();
    auto const dcTotalMppts = mpptData.size();
    if (dcTotalMppts <= 1) { return false; }

    auto const currentThreshold = calculateRequiredOutputThreshold(limitWatts);
    auto const expectedPowerPerMppt = (limitWatts / dcTotalMppts) * currentThreshold;
    return OverscalingCalculator::hasCompensableShading(
            mpptData,
            expectedPowerPerMppt);
}

uint16_t PowerLimiterOverscalingInverter::applyIncrease(uint16_t increase)
{
    if (!isEligible()) { return 0; }

    if (increase == 0) { return 0; }

    // do not wake inverter up if it would produce too much power
    if (!isProducing() && _config.LowerPowerLimit > increase) { return 0; }

    auto const expectedOutput = getExpectedOutputAcWatts();
    auto const actualIncrease = std::min(increase, getMaxIncreaseWatts());
    if (!isProducing() && expectedOutput == 0) {
        setAcOutput(actualIncrease);
    } else {
        auto const targetOutput = static_cast<uint16_t>(expectedOutput + actualIncrease);
        auto const targetLimit = overscalingEnabled()
            ? std::max(scaleLimit(targetOutput), getExpectedLimitWatts())
            : static_cast<uint16_t>(getExpectedLimitWatts() + actualIncrease);
        setAcOutputAndLimit(
                targetOutput,
                targetLimit);
    }
    return actualIncrease;
}

uint16_t PowerLimiterOverscalingInverter::scaleLimit(uint16_t newExpectedOutputWatts)
{
    // overscalling allows us to compensate for shaded panels by increasing the
    // total power limit, if the inverter is solar powered.
    // this feature should not be used when homyiles 'Power Distribution Logic' is available
    // as the inverter will take care of the power distribution across the MPPTs itself.
    // (added in inverter firmware 01.01.12 on supported models (HMS-1600/1800/2000))
    // When disabled we return the expected output.
    if (!overscalingEnabled()) { return newExpectedOutputWatts; }

    // prevent scaling if inverter is not producing, as input channels are not
    // producing energy and hence are detected as not-producing, causing
    // unreasonable scaling.
    if (!isProducing()) { return newExpectedOutputWatts; }

    std::vector<ChannelNum_t> dcChnls = _spInverter->getChannelsDC();
    std::vector<MpptNum_t> dcMppts = _spInverter->getMppts();
    size_t dcTotalChnls = dcChnls.size();
    size_t dcTotalMppts = dcMppts.size();

    // test for a reasonable power limit that allows us to assume that an input
    // channel with little energy is actually not producing, rather than
    // producing very little due to the very low limit.
    if (getCurrentLimitWatts() < dcTotalChnls * 10) { return newExpectedOutputWatts; }

    if (usesPerChannelLimitModel()) {
        auto const channelData = collectChannelData();
        auto const overScaledLimit =
            OverscalingCalculator::calculatePerChannelOverscaledLimit(
                    getCurrentLimitWatts(),
                    newExpectedOutputWatts,
                    channelData,
                    getInverterMaxPowerWatts(),
                    sPerChannelLimitBindingThreshold);
        if (overScaledLimit <= newExpectedOutputWatts) {
            return newExpectedOutputWatts;
        }

        auto const boundChannels = OverscalingCalculator::countLimitBoundChannels(
                getCurrentLimitWatts(),
                channelData,
                sPerChannelLimitBindingThreshold);
        DTU_LOGD("%d/%d input channels are limit-bound, channel-scaling %d W",
                static_cast<int>(boundChannels.first),
                static_cast<int>(dcTotalChnls),
                overScaledLimit);

        return overScaledLimit;
    }

    // if there is only one MPPT available, there is nothing we can do
    if (dcTotalMppts <= 1) { return newExpectedOutputWatts; }

    auto const mpptData = collectMpptData();

    // Calculate thresholds for current and new limits
    float currentThreshold = calculateRequiredOutputThreshold(getCurrentLimitWatts());
    float newThreshold = calculateRequiredOutputThreshold(newExpectedOutputWatts);

    // Calculate overscaled limit using the extracted calculator
    uint16_t overScaledLimit = OverscalingCalculator::calculateOverscaledLimit(
        getCurrentLimitWatts(), newExpectedOutputWatts, mpptData, getInverterMaxPowerWatts(),
        currentThreshold, newThreshold);

    if (overScaledLimit <= newExpectedOutputWatts) {
        return newExpectedOutputWatts;
    }

    // Count newly shaded MPPTs for logging
    float newExpectedMpptPowerAc = (newExpectedOutputWatts / dcTotalMppts) * newThreshold;

    size_t newlyShadedMpptCount = 0;
    for (auto& m : dcMppts) {
        float mpptPowerAC = calculateMpptPowerAC(m);
        if (mpptPowerAC < newExpectedMpptPowerAc) {
            newlyShadedMpptCount++;
        }
    }

    DTU_LOGD("%d/%d mppts are not-producing/shaded, scaling %d W",
            static_cast<int>(newlyShadedMpptCount),
            static_cast<int>(dcTotalMppts),
            overScaledLimit);

    return overScaledLimit;
}

uint16_t PowerLimiterOverscalingInverter::scaleLimitForOutputCap(uint16_t expectedOutputWatts)
{
    if (!overscalingEnabled()) { return expectedOutputWatts; }
    if (!isProducing()) { return expectedOutputWatts; }

    auto const dcTotalChnls = _spInverter->getChannelsDC().size();
    auto const dcTotalMppts = _spInverter->getMppts().size();
    if (getCurrentLimitWatts() < dcTotalChnls * 10) { return expectedOutputWatts; }

    if (usesPerChannelLimitModel()) {
        return OverscalingCalculator::calculatePerChannelOverscaledLimit(
                getCurrentLimitWatts(),
                expectedOutputWatts,
                collectChannelData(),
                getInverterMaxPowerWatts(),
                sPerChannelLimitBindingThreshold);
    }

    if (dcTotalMppts <= 1) { return expectedOutputWatts; }

    auto const mpptData = collectMpptData();
    auto const threshold = calculateRequiredOutputThreshold(expectedOutputWatts);
    return OverscalingCalculator::calculateOverscaledLimitForExpectedOutput(
            expectedOutputWatts,
            mpptData,
            getInverterMaxPowerWatts(),
            threshold);
}

uint16_t PowerLimiterOverscalingInverter::getMaxOverscaledOutputWatts(uint16_t limitWatts) const
{
    auto const configuredMaxPower = getConfiguredMaxPowerWatts();
    if (!overscalingEnabled()) { return configuredMaxPower; }
    if (!isProducing()) { return configuredMaxPower; }

    auto const dcTotalChnls = _spInverter->getChannelsDC().size();
    auto const dcTotalMppts = _spInverter->getMppts().size();
    if (limitWatts < dcTotalChnls * 10) { return configuredMaxPower; }

    if (usesPerChannelLimitModel()) {
        auto const maxOutput = OverscalingCalculator::calculateMaxPerChannelOutput(
                limitWatts,
                collectChannelData(),
                getInverterMaxPowerWatts(),
                sPerChannelLimitBindingThreshold);
        return std::min(maxOutput, configuredMaxPower);
    }

    if (dcTotalMppts <= 1) { return configuredMaxPower; }

    auto const mpptData = collectMpptData();
    auto const threshold = calculateRequiredOutputThreshold(limitWatts);
    auto const expectedPowerPerMppt = (limitWatts / dcTotalMppts) * threshold;
    auto const maxOutput = OverscalingCalculator::calculateMaxCompensatedOutput(
            mpptData,
            getInverterMaxPowerWatts(),
            expectedPowerPerMppt);
    return std::min(maxOutput, configuredMaxPower);
}

void PowerLimiterOverscalingInverter::setAcOutput(uint16_t expectedOutputWatts)
{
    // make sure to enforce the lower and upper bounds
    expectedOutputWatts = std::min(expectedOutputWatts, getConfiguredMaxPowerWatts());
    expectedOutputWatts = std::max(expectedOutputWatts, _config.LowerPowerLimit);

    setAcOutputAndLimit(expectedOutputWatts, scaleLimit(expectedOutputWatts));
}

void PowerLimiterOverscalingInverter::setAcOutputAndLimit(
        uint16_t expectedOutputWatts,
        uint16_t targetLimitWatts)
{
    expectedOutputWatts = std::min(expectedOutputWatts, getConfiguredMaxPowerWatts());
    expectedOutputWatts = std::max(expectedOutputWatts, _config.LowerPowerLimit);

    targetLimitWatts = std::min(targetLimitWatts, getInverterMaxPowerWatts());
    targetLimitWatts = std::max(targetLimitWatts, _config.LowerPowerLimit);

    setExpectedOutputAcWatts(expectedOutputWatts);
    setTargetPowerLimitWatts(targetLimitWatts);
    setTargetPowerState(true);
}

bool PowerLimiterOverscalingInverter::overscalingEnabled() const
{
    return _config.UseOverscaling && !_spInverter->supportsPowerDistributionLogic();
}
