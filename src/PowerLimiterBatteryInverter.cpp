#include "PowerLimiterBatteryInverter.h"

#include <algorithm>

PowerLimiterBatteryInverter::PowerLimiterBatteryInverter(PowerLimiterInverterConfig const& config)
    : PowerLimiterInverter(config) { }

uint16_t PowerLimiterBatteryInverter::getMaxReductionWatts(bool allowStandby) const
{
    if (!isEligible()) { return 0; }

    auto const expectedOutput = getExpectedOutputAcWatts();
    auto const currentOutput = getCurrentOutputAcWatts();
    if (!isProducing() && expectedOutput == 0 && currentOutput == 0) { return 0; }

    if (allowStandby && _config.AllowStandby) {
        return std::max(expectedOutput, currentOutput);
    }

    auto low = std::min(getExpectedLimitWatts(), expectedOutput);
    if (low <= _config.LowerPowerLimit) { return 0; }

    return low - _config.LowerPowerLimit;
}

uint16_t PowerLimiterBatteryInverter::getMaxIncreaseWatts() const
{
    if (!isEligible()) { return 0; }

    auto const expectedOutput = getExpectedOutputAcWatts();
    auto const currentOutput = getCurrentOutputAcWatts();
    if (expectedOutput == 0 && currentOutput == 0) {
        return getConfiguredMaxPowerWatts();
    }

    auto const baseline = std::max(getExpectedLimitWatts(), expectedOutput);
    if (baseline >= getConfiguredMaxPowerWatts()) { return 0; }

    return getConfiguredMaxPowerWatts() - baseline;
}

uint16_t PowerLimiterBatteryInverter::applyReduction(uint16_t reduction, bool allowStandby)
{
    if (!isEligible()) { return 0; }

    if (reduction == 0) { return 0; }

    auto const expectedOutput = getExpectedOutputAcWatts();
    auto const reducibleOutput = std::max(expectedOutput, getCurrentOutputAcWatts());
    if (!isProducing() && reducibleOutput == 0) { return 0; }

    auto low = std::min(getExpectedLimitWatts(), expectedOutput);
    if (low <= _config.LowerPowerLimit) {
        if (allowStandby && _config.AllowStandby) {
            standby();
            return std::min(reduction, reducibleOutput);
        }
        return 0;
    }

    auto const reducibleWithoutStandby = static_cast<uint16_t>(
            low - _config.LowerPowerLimit);
    if (reducibleWithoutStandby >= reduction) {
        setAcOutputAndLimit(
                static_cast<uint16_t>(expectedOutput - reduction),
                static_cast<uint16_t>(getExpectedLimitWatts() - reduction));
        return reduction;
    }

    if (allowStandby && _config.AllowStandby) {
        standby();
        return std::min(reduction, reducibleOutput);
    }

    setAcOutputAndLimit(
            static_cast<uint16_t>(expectedOutput - reducibleWithoutStandby),
            static_cast<uint16_t>(getExpectedLimitWatts() - reducibleWithoutStandby));
    return reducibleWithoutStandby;
}

uint16_t PowerLimiterBatteryInverter::applyIncrease(uint16_t increase)
{
    if (!isEligible()) { return 0; }

    if (increase == 0) { return 0; }

    auto const expectedOutput = getExpectedOutputAcWatts();
    auto const currentOutput = getCurrentOutputAcWatts();
    bool const startsFromZeroOutput = expectedOutput == 0 && currentOutput == 0;

    // do not wake inverter up if it would produce too much power
    if (startsFromZeroOutput && _config.LowerPowerLimit > increase) { return 0; }

    auto const expectedLimit = getExpectedLimitWatts();
    auto baseline = std::max(expectedLimit, expectedOutput);

    // Battery-powered inverters in standby can retain an arbitrary stale limit,
    // and some units still report "producing" briefly after standby. If DPL's
    // output basis is zero, the next target is an absolute startup target.
    if (startsFromZeroOutput) { baseline = 0; }

    auto actualIncrease = std::min(increase, getMaxIncreaseWatts());
    if (startsFromZeroOutput) {
        setAcOutput(baseline + actualIncrease);
    } else {
        setAcOutputAndLimit(
                static_cast<uint16_t>(expectedOutput + actualIncrease),
                static_cast<uint16_t>(expectedLimit + actualIncrease));
    }
    if (startsFromZeroOutput) {
        reassertTargetPowerState();
    }
    return actualIncrease;
}

uint16_t PowerLimiterBatteryInverter::standby()
{
    setTargetPowerState(false);
    setExpectedOutputAcWatts(0);
    return getCurrentOutputAcWatts();
}

void PowerLimiterBatteryInverter::setAcOutputAndLimit(
        uint16_t expectedOutputWatts,
        uint16_t targetLimitWatts)
{
    // Keep the RF limit movement relative to the previous limit. The expected
    // output is the planner's AC effect estimate and may differ from the RF cap.
    expectedOutputWatts = std::min(expectedOutputWatts, getConfiguredMaxPowerWatts());
    expectedOutputWatts = std::max(expectedOutputWatts, _config.LowerPowerLimit);

    targetLimitWatts = std::min(targetLimitWatts, getConfiguredMaxPowerWatts());
    targetLimitWatts = std::max(targetLimitWatts, _config.LowerPowerLimit);

    setExpectedOutputAcWatts(expectedOutputWatts);
    setTargetPowerLimitWatts(targetLimitWatts);
    setTargetPowerState(true);
}

void PowerLimiterBatteryInverter::setAcOutput(uint16_t expectedOutputWatts)
{
    setAcOutputAndLimit(expectedOutputWatts, expectedOutputWatts);
}
