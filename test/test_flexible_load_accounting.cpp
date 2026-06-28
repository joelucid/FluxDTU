// SPDX-License-Identifier: GPL-2.0-or-later

#include "FlexibleLoadAccounting.h"

#include <cmath>
#include <iostream>
#include <optional>
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

void testMeasuredLoadGetsProportionalBatteryShare()
{
    auto const support = FlexibleLoadAccounting::allocateSharedPowerToLoad(
            381.0f,
            381.0f,
            381.0f);

    expectNear(support, 381.0f, "single measured load gets full battery discharge");
}

void testSupportIsCappedByMeasuredLoadPower()
{
    auto const support = FlexibleLoadAccounting::allocateSharedPowerToLoad(
            120.0f,
            120.0f,
            300.0f);

    expectNear(support, 120.0f, "support cannot exceed measured load power");
}

void testMeasuredLoadsShareBatteryDischarge()
{
    auto const support = FlexibleLoadAccounting::allocateSharedPowerToLoad(
            300.0f,
            600.0f,
            240.0f);

    expectNear(support, 120.0f, "battery discharge is shared by measured load power");
}

void testMissingPowerDoesNotStealMeasuredLoadShare()
{
    auto const support = FlexibleLoadAccounting::allocateSharedPowerToLoad(
            std::nullopt,
            300.0f,
            240.0f);

    expectNear(support, 0.0f, "missing measurement is ignored when other load power is known");
}

void testMissingOnlyLoadConservativelyGetsFullSupport()
{
    auto const support = FlexibleLoadAccounting::allocateSharedPowerToLoad(
            std::nullopt,
            0.0f,
            240.0f);

    expectNear(support, 240.0f, "missing only measurement gets full shared power");
}

} // namespace

int main()
{
    try {
        testMeasuredLoadGetsProportionalBatteryShare();
        testSupportIsCappedByMeasuredLoadPower();
        testMeasuredLoadsShareBatteryDischarge();
        testMissingPowerDoesNotStealMeasuredLoadShare();
        testMissingOnlyLoadConservativelyGetsFullSupport();
    } catch (std::exception const& e) {
        std::cerr << "FAILED: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Flexible load accounting tests passed" << std::endl;
    return 0;
}
