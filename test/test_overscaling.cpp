#include <iostream>
#include <cassert>
#include <vector>

// Include the actual OverscalingCalculator
#include "../include/OverscalingCalculator.h"

// Fixed thresholds as they don't have any effect on the tests
const float currentThreshold = 0.97f;
const float newThreshold = 0.97f;

const uint16_t inverterMaxPower = 2000;

void testShadedMpptCounting() {
    std::cout << "Testing: Shaded MPPT counting" << std::endl;

    std::vector<OverscalingCalculator::MpptData> mpptData = {
        {100.0f}, {250.0f}, {300.0f}, {400.0f}
    };

    // Test with threshold of 250W
    auto [count, powerSum] = OverscalingCalculator::countShadedMppts(mpptData, 250.0f);

    assert(count == 1);  // First MPPT is below threshold
    assert(powerSum == 100.0f);

    std::cout << "✓ PASSED: Shaded MPPT counting correct" << std::endl;
}

void testCompensableShadingDetection() {
    std::cout << "Testing: Compensable MPPT shading detection" << std::endl;

    std::vector<OverscalingCalculator::MpptData> oneWeakInput = {
        {369.2f}, {298.3f}
    };
    std::vector<OverscalingCalculator::MpptData> allWeakInputs = {
        {290.0f}, {298.3f}
    };
    std::vector<OverscalingCalculator::MpptData> noWeakInputs = {
        {369.2f}, {350.0f}
    };

    assert(OverscalingCalculator::hasCompensableShading(oneWeakInput, 337.0f));
    assert(!OverscalingCalculator::hasCompensableShading(allWeakInputs, 337.0f));
    assert(!OverscalingCalculator::hasCompensableShading(noWeakInputs, 337.0f));

    std::cout << "✓ PASSED: Compensable MPPT shading detected correctly" << std::endl;
}

void testMaxCompensatedOutputCapsIncreaseCapacity() {
    std::cout << "Testing: Max compensated output caps overscaling capacity" << std::endl;

    std::vector<OverscalingCalculator::MpptData> mpptData = {
        {369.2f}, {298.3f}
    };

    auto const expectedPowerPerMppt = (695 / 2) * currentThreshold;
    uint16_t result = OverscalingCalculator::calculateMaxCompensatedOutput(
            mpptData,
            1000,
            expectedPowerPerMppt);

    assert(result >= 798);
    assert(result < 800);

    std::cout << "✓ PASSED: Max compensated output capped at " << result << "W)" << std::endl;
}

// Tests for scenarios where new expected output is HIGHER than current limit
void testHigherOutputNoShading() {
    std::cout << "Testing: Higher output, no shading - no overscaling needed" << std::endl;

    std::vector<OverscalingCalculator::MpptData> mpptData = {
        {250.0f}, {250.0f}, {250.0f}, {250.0f}  // All MPPTs at current limit (not shaded)
    };

    uint16_t result = OverscalingCalculator::calculateOverscaledLimit(1000, 1500, mpptData, inverterMaxPower, currentThreshold, newThreshold);
    assert(result == 1500);  // Should return new expected output

    std::cout << "✓ PASSED: Higher output with no shading returns expected output" << std::endl;
}

void testHigherOutputOneShaded() {
    std::cout << "Testing: Higher output, one MPPT shaded - moderate overscaling" << std::endl;

    std::vector<OverscalingCalculator::MpptData> mpptData = {
        {0.0f}, {125.0f}, {125.0f}, {125.0f}  // One MPPT shaded, others at current limit
    };

    uint16_t result = OverscalingCalculator::calculateOverscaledLimit(500, 750, mpptData, inverterMaxPower, currentThreshold, newThreshold);
    assert(result == 1000);

    std::cout << "✓ PASSED: Higher output with 1 shaded MPPT scales to " << result << "W)" << std::endl;
}

void testHigherOutputMultipleShaded() {
    std::cout << "Testing: Higher output, multiple MPPTs shaded - significant overscaling" << std::endl;

    std::vector<OverscalingCalculator::MpptData> mpptData = {
        {0.0f}, {0.0f}, {125.0f}, {125.0f}  // Two MPPTs shaded, others at current limit
    };

    uint16_t result = OverscalingCalculator::calculateOverscaledLimit(500, 750, mpptData, inverterMaxPower, currentThreshold, newThreshold);
    assert(result == 1500);

    std::cout << "✓ PASSED: Higher output with multiple shaded MPPTs scales to " << result << "W)" << std::endl;
}

void testHigherOutputAllShaded() {
    std::cout << "Testing: Higher output, all MPPTs shaded - significant overscaling" << std::endl;

    std::vector<OverscalingCalculator::MpptData> mpptData = {
        {0.0f}, {0.0f}, {0.0f}, {0.0f}  // All MPPTs shaded
    };

    uint16_t result = OverscalingCalculator::calculateOverscaledLimit(500, 750, mpptData, inverterMaxPower, currentThreshold, newThreshold);
    std::cout << "Result: " << result << "W)" << std::endl;
    assert(result == 750); // Use new limit anyway

    std::cout << "✓ PASSED: Higher output with multiple shaded MPPTs applies new limit" << std::endl;
}

// Tests for scenarios where new expected output is LOWER than current limit
void testLowerOutputNoShading() {
    std::cout << "Testing: Lower output, no shading - no overscaling needed" << std::endl;

    std::vector<OverscalingCalculator::MpptData> mpptData = {
        {250.0f}, {250.0f}, {250.0f}, {250.0f}  // All MPPTs producing well, according to the new limit
    };

    uint16_t result = OverscalingCalculator::calculateOverscaledLimit(1500, 1000, mpptData, inverterMaxPower, currentThreshold, newThreshold);
    assert(result == 1000);  // Should return new expected output

    std::cout << "✓ PASSED: Lower output with no shading returns expected output" << std::endl;
}

void testLowerOutputOneShaded() {
    std::cout << "Testing: Lower output, one MPPT shaded - moderate overscaling" << std::endl;

    std::vector<OverscalingCalculator::MpptData> mpptData = {
        {0.0f}, {400.0f}, {400.0f}, {400.0f}  // One MPPT shaded, according to the new limit
    };

    uint16_t result = OverscalingCalculator::calculateOverscaledLimit(1500, 1000, mpptData, inverterMaxPower, currentThreshold, newThreshold);
    assert(result == 1332);  // Should scale up moderately

    std::cout << "✓ PASSED: Lower output with 1 shaded MPPT scales moderately (result: " << result << "W)" << std::endl;
}

void testLowerOutputMultipleShaded() {
    std::cout << "Testing: Lower output, multiple MPPTs shaded - significant overscaling" << std::endl;

    std::vector<OverscalingCalculator::MpptData> mpptData = {
        {0.0f}, {0.0f}, {500.0f}, {500.0f}  // Two MPPTs shaded, according to the new limit
    };

    uint16_t result = OverscalingCalculator::calculateOverscaledLimit(1500, 800, mpptData, inverterMaxPower, currentThreshold, newThreshold);
    assert(result == 1600);

    std::cout << "✓ PASSED: Lower output with multiple shaded MPPTs scales to max (result: " << result << "W)" << std::endl;
}

// ============================================================================
// REALISTIC SHADING TESTS WITH NON-ZERO VALUES
// ============================================================================

void testRealisticShadingScenarios() {
    std::cout << "Testing: Realistic shading scenarios with partial power output" << std::endl;

    // Scenario 1: One MPPT partially shaded (producing 60% of expected) - increasing power
    std::vector<OverscalingCalculator::MpptData> mpptData1 = {
        {300.0f}, {180.0f}, {300.0f}, {300.0f}  // One MPPT at 60% capacity
    };

    // Current limit: 1200W, Actual output: 1080W, New demand: 1300W (increasing)
    uint16_t result1 = OverscalingCalculator::calculateOverscaledLimit(1200, 1300, mpptData1, inverterMaxPower, currentThreshold, newThreshold);
    assert(result1 > 1300);  // Should apply overscaling since there's shading and increasing power

    // Scenario 2: Two MPPTs partially shaded (producing 70% and 50% of expected) - increasing power
    std::vector<OverscalingCalculator::MpptData> mpptData2 = {
        {300.0f}, {210.0f}, {150.0f}, {300.0f}  // Two MPPTs at 70% and 50% capacity
    };

    // Current limit: 1200W, Actual output: 960W, New demand: 1400W (increasing)
    uint16_t result2 = OverscalingCalculator::calculateOverscaledLimit(1200, 1400, mpptData2, inverterMaxPower, currentThreshold, newThreshold);
    assert(result2 > 1400);  // Should apply overscaling since there's shading and increasing power

    // Scenario 3: All MPPTs partially shaded (producing 40-60% of expected) - increasing power
    std::vector<OverscalingCalculator::MpptData> mpptData3 = {
        {120.0f}, {150.0f}, {180.0f}, {200.0f}  // All MPPTs at 40-60% capacity
    };

    // Current limit: 1200W, Actual output: 650W, New demand: 1000W (increasing)
    uint16_t result3 = OverscalingCalculator::calculateOverscaledLimit(1200, 1000, mpptData3, inverterMaxPower, currentThreshold, newThreshold);
    assert(result3 >= 1000);  // Should return at least the expected output

    std::cout << "✓ PASSED: Realistic shading scenarios handled correctly" << std::endl;
}

void testPartialShadingScenarios() {
    std::cout << "Testing: Partial shading scenarios with increasing power" << std::endl;

    // Scenario: One MPPT heavily shaded (30% capacity), others normal
    std::vector<OverscalingCalculator::MpptData> mpptData = {
        {400.0f}, {120.0f}, {400.0f}, {400.0f}  // One MPPT at 30% capacity
    };

    // Current limit: 1600W, Actual output: 1320W, New demand: 1800W
    uint16_t result = OverscalingCalculator::calculateOverscaledLimit(1600, 1800, mpptData, inverterMaxPower, currentThreshold, newThreshold);

    // Should apply overscaling since there's shading and we're increasing power
    assert(result > 1800);  // Should be overscaled
    assert(result <= 2000);  // Should not exceed inverter max

    std::cout << "✓ PASSED: Partial shading with increasing power scales correctly (result: " << result << "W)" << std::endl;
}

void testTwoMpptShedScenarioRequiresLargeOverscaling() {
    std::cout << "Testing: Two-MPPT shed scenario needs more than 1:1 limit increase" << std::endl;

    std::vector<OverscalingCalculator::MpptData> mpptData = {
        {369.2f}, {298.3f}
    };

    uint16_t result = OverscalingCalculator::calculateOverscaledLimit(
            695,
            795,
            mpptData,
            1000,
            currentThreshold,
            newThreshold);

    assert(result >= 990);
    assert(result > 895);

    std::cout << "✓ PASSED: Two-MPPT shed scenario overscales to " << result << "W)" << std::endl;
}

void testOverscaledOutputCapUsesTargetThreshold() {
    std::cout << "Testing: Overscaled output cap uses target threshold" << std::endl;

    std::vector<OverscalingCalculator::MpptData> mpptData = {
        {350.3f}, {264.1f}
    };

    uint16_t result = OverscalingCalculator::calculateOverscaledLimitForExpectedOutput(
            699,
            mpptData,
            1500,
            newThreshold);

    assert(result >= 860);
    assert(result < 900);

    std::cout << "✓ PASSED: Overscaled output cap stays scaled at " << result << "W)" << std::endl;
}

void testEdgeCaseShadingScenarios() {
    std::cout << "Testing: Edge case shading scenarios" << std::endl;

    // Scenario 1: MPPTs producing exactly at threshold (borderline shading) - increasing power
    std::vector<OverscalingCalculator::MpptData> mpptData1 = {
        {291.0f}, {291.0f}, {291.0f}, {291.0f}  // Exactly at 97% of 300W expected
    };

    // Current limit: 1200W, Actual output: 1164W, New demand: 1300W (increasing)
    uint16_t result1 = OverscalingCalculator::calculateOverscaledLimit(1200, 1300, mpptData1, inverterMaxPower, currentThreshold, newThreshold);
    assert(result1 == 1300);  // Should not be considered shaded (exactly at threshold)

    // Scenario 2: MPPTs just below threshold (minimal shading) - increasing power
    std::vector<OverscalingCalculator::MpptData> mpptData2 = {
        {290.0f}, {290.0f}, {290.0f}, {290.0f}  // Just below 97% of 300W expected
    };

    // Current limit: 1200W, Actual output: 1160W, New demand: 1300W (increasing)
    uint16_t result2 = OverscalingCalculator::calculateOverscaledLimit(1200, 1300, mpptData2, inverterMaxPower, currentThreshold, newThreshold);
    assert(result2 == 1300);  // Should return new limit since it's higher than current limit

    // Scenario 3: Mixed shading levels (some heavily shaded, some moderately) - increasing power
    std::vector<OverscalingCalculator::MpptData> mpptData3 = {
        {400.0f}, {100.0f}, {250.0f}, {350.0f}  // Mixed shading levels
    };

    // Current limit: 1600W, Actual output: 1100W, New demand: 1400W (increasing)
    uint16_t result3 = OverscalingCalculator::calculateOverscaledLimit(1600, 1400, mpptData3, inverterMaxPower, currentThreshold, newThreshold);
    assert(result3 > 1400);  // Should apply overscaling since there's shading and increasing power

    std::cout << "✓ PASSED: Edge case shading scenarios handled correctly" << std::endl;
}

void testPerChannelLimitBindingForHm1500StyleLimit() {
    std::cout << "Testing: HM-1500 style per-channel limit binding" << std::endl;

    std::vector<OverscalingCalculator::ChannelData> channelData = {
        {38.0f}, {46.74f}, {35.625f}, {48.735f}
    };

    auto [boundCount, boundPowerSum] =
        OverscalingCalculator::countLimitBoundChannels(
                198,
                channelData,
                0.95f);
    assert(boundCount == 1);
    assert(boundPowerSum > 48.0f);
    assert(boundPowerSum < 49.0f);

    auto const modeledCurrentOutput =
        OverscalingCalculator::calculatePerChannelLimitOutput(
                198,
                198,
                channelData,
                0.95f);
    assert(modeledCurrentOutput >= 169);
    assert(modeledCurrentOutput <= 170);

    auto const scaledLimit =
        OverscalingCalculator::calculatePerChannelOverscaledLimit(
                198,
                232,
                channelData,
                1500,
                0.95f);
    assert(scaledLimit == 447);

    auto const maxOutput =
        OverscalingCalculator::calculateMaxPerChannelOutput(
                198,
                channelData,
                1500,
                0.95f);
    assert(maxOutput == 495);

    std::cout << "✓ PASSED: HM-1500 channel limit scales to " << scaledLimit << "W)" << std::endl;
}

void testPerChannelLimitAllChannelsBoundNeedsNoOverscale() {
    std::cout << "Testing: Per-channel limit with all channels bound" << std::endl;

    std::vector<OverscalingCalculator::ChannelData> channelData = {
        {145.0f}, {146.0f}, {144.0f}, {145.0f}
    };

    auto [boundCount, boundPowerSum] =
        OverscalingCalculator::countLimitBoundChannels(
                600,
                channelData,
                0.95f);
    assert(boundCount == 4);
    assert(boundPowerSum > 579.0f);
    assert(boundPowerSum < 581.0f);

    auto const scaledLimit =
        OverscalingCalculator::calculatePerChannelOverscaledLimit(
                600,
                700,
                channelData,
                1500,
                0.95f);
    assert(scaledLimit == 700);

    auto const maxOutput =
        OverscalingCalculator::calculateMaxPerChannelOutput(
                600,
                channelData,
                1500,
                0.95f);
    assert(maxOutput == 1500);

    std::cout << "✓ PASSED: All bound channels scale 1:1" << std::endl;
}

void testPerChannelLimitDoesNotInventUnboundCapacity() {
    std::cout << "Testing: Per-channel limit without bound channels" << std::endl;

    std::vector<OverscalingCalculator::ChannelData> channelData = {
        {80.0f}, {60.0f}, {40.0f}, {20.0f}
    };

    assert(!OverscalingCalculator::hasLimitBoundChannel(
            600,
            channelData,
            0.95f));

    auto const scaledLimit =
        OverscalingCalculator::calculatePerChannelOverscaledLimit(
                600,
                250,
                channelData,
                1500,
                0.95f);
    assert(scaledLimit == 250);

    auto const maxOutput =
        OverscalingCalculator::calculateMaxPerChannelOutput(
                600,
                channelData,
                1500,
                0.95f);
    assert(maxOutput == 200);

    std::cout << "✓ PASSED: Unbound channels do not create non-probing headroom" << std::endl;
}

int main() {
    std::cout << "=== FluxDTU Overscaling Calculator Tests ===" << std::endl;
    std::cout << "This tests the actual overscaling logic in isolation" << std::endl;
    std::cout << std::endl;

    try {
        testShadedMpptCounting();
        testCompensableShadingDetection();
        testMaxCompensatedOutputCapsIncreaseCapacity();

        testHigherOutputNoShading();
        testHigherOutputOneShaded();
        testHigherOutputMultipleShaded();
        testHigherOutputAllShaded();

        testLowerOutputNoShading();
        testLowerOutputOneShaded();
        testLowerOutputMultipleShaded();
        // This test case is missing because its not valid, when all MPPTs are shaded, it will be an increase, not a decrease
        // testLowerOutputAllShaded();

        // Realistic shading tests with non-zero values
        testRealisticShadingScenarios();
        testPartialShadingScenarios();
        testTwoMpptShedScenarioRequiresLargeOverscaling();
        testOverscaledOutputCapUsesTargetThreshold();
        testEdgeCaseShadingScenarios();
        testPerChannelLimitBindingForHm1500StyleLimit();
        testPerChannelLimitAllChannelsBoundNeedsNoOverscale();
        testPerChannelLimitDoesNotInventUnboundCapacity();

        std::cout << std::endl;
        std::cout << "✓ ALL TESTS PASSED!" << std::endl;

        return 0;
    } catch (const std::exception& e) {
        std::cout << "❌ TEST FAILED: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cout << "❌ TEST FAILED: Unknown error" << std::endl;
        return 1;
    }
}
