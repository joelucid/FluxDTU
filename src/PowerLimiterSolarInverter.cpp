#include "PowerLimiterSolarInverter.h"
#include <algorithm>

namespace {

static constexpr uint16_t sProductionLimitBindingMarginWatts = 20;

}

PowerLimiterSolarInverter::PowerLimiterSolarInverter(PowerLimiterInverterConfig const& config)
    : PowerLimiterOverscalingInverter(config) { }

bool PowerLimiterSolarInverter::isProductionLimitBinding(uint16_t expectedLimitWatts) const
{
    auto const measuredOutput = getMeasuredOutputAcWatts();
    if (static_cast<float>(measuredOutput) + sProductionLimitBindingMarginWatts
            >= static_cast<float>(expectedLimitWatts)) {
        return true;
    }

    return isPerChannelLimitBinding(expectedLimitWatts);
}

uint16_t PowerLimiterSolarInverter::getExpectedOutputAcWatts() const
{
    if (hasPendingTarget() || isUsingAssumedOutput()) {
        return PowerLimiterInverter::getExpectedOutputAcWatts();
    }

    // If the inverter is not producing, it cannot cover any of the requested
    // output. This is especially important at night, where otherwise the DPL
    // may reserve power for solar inverters and under-request battery output.
    if (!isProducing()) { return 0; }

    return PowerLimiterInverter::getExpectedOutputAcWatts();
}

uint16_t PowerLimiterSolarInverter::getMaxReductionWatts(bool) const
{
    if (!isEligible()) { return 0; }

    if (!isProducing()) { return 0; }

    auto low = std::min(getExpectedLimitWatts(), getExpectedOutputAcWatts());
    if (low <= _config.LowerPowerLimit) { return 0; }

    return low - _config.LowerPowerLimit;
}

uint16_t PowerLimiterSolarInverter::getMaxIncreaseWatts() const
{
    if (!isEligible()) { return 0; }

    if (!isProducing()) {
        return 0;
    }

    auto const expectedOutput = getExpectedOutputAcWatts();
    if (expectedOutput >= getConfiguredMaxPowerWatts()) { return 0; }

    auto const expectedLimit = getExpectedLimitWatts();
    // Only real telemetry proves that the inverter is production-limited.
    // Assumed output after a probe must not trigger the next probe.
    if (!isProductionLimitBinding(expectedLimit)) {
        if (hasPendingTarget() || isUsingAssumedOutput()) { return 0; }
        if (!hasCompensableLimitHeadroom(expectedLimit)) { return 0; }
    }

    auto inverterMaxLimit = getConfiguredMaxPowerWatts();

    if (overscalingEnabled() || _spInverter->supportsPowerDistributionLogic()) {
        // we use the inverter's max power, because each MPPT can deliver its max power individually
        inverterMaxLimit = getInverterMaxPowerWatts();
    }

    if (expectedLimit >= inverterMaxLimit) { return 0; }

    // A solar increase is a limit/setpoint probe. MPPT power cannot prove the
    // available headroom before the higher limit has been sent; the predictive
    // ledger records these requests as SolarCapacityLimited and keeps the
    // resulting grid effect uncertain until the meter confirms it.
    auto const maxOutputIncrease = getConfiguredMaxPowerWatts() - expectedOutput;
    if (overscalingEnabled()) {
        auto const maxOverscaledOutput = getMaxOverscaledOutputWatts(expectedLimit);
        if (maxOverscaledOutput <= expectedOutput) { return 0; }

        return std::min<uint16_t>(
                maxOutputIncrease,
                maxOverscaledOutput - expectedOutput);
    }

    auto const maxLimitIncrease = inverterMaxLimit - expectedLimit;
    return std::min(maxOutputIncrease, maxLimitIncrease);
}

uint16_t PowerLimiterSolarInverter::getMaxNonProbingIncreaseWatts() const
{
    if (!isEligible()) { return 0; }
    if (!isProducing()) { return 0; }
    if (hasPendingTarget() || isUsingAssumedOutput()) { return 0; }

    auto const expectedLimit = getExpectedLimitWatts();
    if (!isProductionLimitBinding(expectedLimit)
            && !hasCompensableLimitHeadroom(expectedLimit)) {
        return 0;
    }

    return getMaxIncreaseWatts();
}

uint16_t PowerLimiterSolarInverter::applyReduction(uint16_t reduction, bool)
{
    if (!isEligible()) { return 0; }

    if (reduction == 0) { return 0; }

    auto const expectedOutput = getExpectedOutputAcWatts();
    if (expectedOutput <= _config.LowerPowerLimit) { return 0; }

    auto const reducibleOutput = static_cast<uint16_t>(
            expectedOutput - _config.LowerPowerLimit);
    auto const actualReduction = std::min(reduction, reducibleOutput);
    auto const targetOutput = static_cast<uint16_t>(
            expectedOutput - actualReduction);
    auto const expectedLimit = getExpectedLimitWatts();
    auto const targetLimit = static_cast<uint16_t>(
            expectedLimit > actualReduction
                ? expectedLimit - actualReduction
                : _config.LowerPowerLimit);

    setAcOutputAndLimit(targetOutput, targetLimit);
    return actualReduction;
}

bool PowerLimiterSolarInverter::capOutputLimit(uint16_t outputLimitWatts)
{
    if (!isEligible() || !isProducing() || hasPendingTarget() || isUsingAssumedOutput()) {
        return false;
    }

    auto currentOutput = getExpectedOutputAcWatts();
    auto outputLimit = std::max(outputLimitWatts, currentOutput);
    outputLimit = std::min(outputLimit, getConfiguredMaxPowerWatts());
    outputLimit = std::max(outputLimit, _config.LowerPowerLimit);

    auto targetLimit = overscalingEnabled()
        ? scaleLimitForOutputCap(outputLimit)
        : outputLimit;
    if (targetLimit >= getExpectedLimitWatts()) { return false; }

    setExpectedOutputAcWatts(currentOutput);
    setTargetPowerLimitWatts(targetLimit);
    setTargetPowerState(true);
    return true;
}

uint16_t PowerLimiterSolarInverter::standby()
{
    // solar-powered inverters are never actually put into standby (by the
    // DPL), but only set to the configured lower power limit instead.
    setAcOutput(_config.LowerPowerLimit);
    return getCurrentOutputAcWatts() - _config.LowerPowerLimit;
}
