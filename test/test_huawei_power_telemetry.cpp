#include "gridcharger/huawei/PowerTelemetry.h"

#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

using namespace GridChargers::Huawei;

namespace {

constexpr float Eps = 0.001f;

void fail(std::string const& message)
{
    throw std::runtime_error(message);
}

void expectNear(float actual, float expected, std::string const& label)
{
    if (std::fabs(actual - expected) > Eps) {
        fail(label + ": expected " + std::to_string(expected)
                + ", got " + std::to_string(actual));
    }
}

float require(std::optional<float> value, std::string const& label)
{
    if (!value) { fail(label + ": expected value"); }
    return *value;
}

void testMeasuredInputWinsWhenNonZero()
{
    auto const inputPower = estimateInputPowerWatts(
            42.0f,
            0.0f,
            50.0f,
            0.6f,
            0.0f);

    expectNear(require(inputPower, "input power"), 42.0f, "measured input");
}

void testOutputCurrentFallbackWhenPowerRegistersAreZero()
{
    auto const inputPower = estimateInputPowerWatts(
            0.0f,
            0.0f,
            49.95313f,
            0.537109f,
            0.0f);

    expectNear(
            require(inputPower, "input power"),
            49.95313f * 0.537109f / HuaweiDefaultEfficiency,
            "derived input");
}

void testOutputPowerFallbackUsesEfficiency()
{
    auto const inputPower = estimateInputPowerWatts(
            0.0f,
            40.0f,
            std::nullopt,
            std::nullopt,
            80.0f);

    expectNear(require(inputPower, "input power"), 50.0f, "output power input");
}

void testOutputVoltageCurrentWinsWhenAvailable()
{
    auto const inputPower = estimateInputPowerWatts(
            0.0f,
            40.0f,
            50.0f,
            0.6f,
            80.0f);

    expectNear(require(inputPower, "input power"), 37.5f, "voltage current input");
}

void testInactiveOutputKeepsMeasuredZero()
{
    auto const inputPower = estimateInputPowerWatts(
            0.0f,
            0.0f,
            50.0f,
            0.01f,
            90.0f);

    expectNear(require(inputPower, "input power"), 0.0f, "inactive output");
}

} // namespace

int main()
{
    try {
        testMeasuredInputWinsWhenNonZero();
        testOutputCurrentFallbackWhenPowerRegistersAreZero();
        testOutputPowerFallbackUsesEfficiency();
        testOutputVoltageCurrentWinsWhenAvailable();
        testInactiveOutputKeepsMeasuredZero();
    } catch (std::exception const& exc) {
        std::cerr << "FAILED: " << exc.what() << std::endl;
        return 1;
    }

    std::cout << "Huawei power telemetry tests passed" << std::endl;
    return 0;
}
