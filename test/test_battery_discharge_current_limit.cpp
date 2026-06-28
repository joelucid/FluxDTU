// SPDX-License-Identifier: GPL-2.0-or-later

#include "PowerLimiterBatteryCurrentLimit.h"

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

constexpr float Eps = 0.001f;

void expectNear(float actual, float expected, std::string const& label)
{
    if (std::fabs(actual - expected) > Eps) {
        std::ostringstream out;
        out << label << ": expected " << expected << ", got " << actual;
        throw std::runtime_error(out.str());
    }
}

void testBudgetCapacityUsesPeakHeadroom()
{
    auto const capacity =
        PowerLimiterBatteryCurrentLimit::budgetCapacityAmpSeconds(
                26.0f,
                50.0f,
                60.0f);

    expectNear(capacity, 1440.0f, "capacity");
}

void testFullBudgetAllowsPeakLimit()
{
    auto const limit = PowerLimiterBatteryCurrentLimit::currentLimitAmps(
            26.0f,
            50.0f,
            60.0f,
            1440.0f);

    expectNear(limit, 50.0f, "full budget limit");
}

void testHalfBudgetAllowsHalfPeakHeadroom()
{
    auto const limit = PowerLimiterBatteryCurrentLimit::currentLimitAmps(
            26.0f,
            50.0f,
            60.0f,
            720.0f);

    expectNear(limit, 38.0f, "half budget limit");
}

void testSmallBudgetAllowsSmallHeadroom()
{
    auto const limit = PowerLimiterBatteryCurrentLimit::currentLimitAmps(
            26.0f,
            50.0f,
            60.0f,
            120.0f);

    expectNear(limit, 28.0f, "small budget limit");
}

void testEmptyBudgetFallsBackToContinuousLimit()
{
    auto const limit = PowerLimiterBatteryCurrentLimit::currentLimitAmps(
            26.0f,
            50.0f,
            60.0f,
            0.0f);

    expectNear(limit, 26.0f, "empty budget limit");
}

void testDisabledPeakFallsBackToContinuousLimit()
{
    auto const limit = PowerLimiterBatteryCurrentLimit::currentLimitAmps(
            26.0f,
            26.0f,
            60.0f,
            100.0f);

    expectNear(limit, 26.0f, "disabled peak limit");
}

void testShortPeakUsesShorterProjectionHorizon()
{
    auto const limit = PowerLimiterBatteryCurrentLimit::currentLimitAmps(
            10.0f,
            20.0f,
            5.0f,
            25.0f);

    expectNear(limit, 15.0f, "short peak projection limit");
}

} // namespace

int main()
{
    try {
        testBudgetCapacityUsesPeakHeadroom();
        testFullBudgetAllowsPeakLimit();
        testHalfBudgetAllowsHalfPeakHeadroom();
        testSmallBudgetAllowsSmallHeadroom();
        testEmptyBudgetFallsBackToContinuousLimit();
        testDisabledPeakFallsBackToContinuousLimit();
        testShortPeakUsesShorterProjectionHorizon();
    } catch (std::exception const& e) {
        std::cerr << "FAILED: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Battery discharge current limit tests passed" << std::endl;
    return 0;
}
