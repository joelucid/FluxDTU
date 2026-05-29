// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Malte Schmidt and others
 */
#include <battery/Controller.h>
#include <gridcharger/Controller.h>
#include <gridcharger/huawei/Provider.h>
#include <gridcharger/huawei/MCP2515.h>
#include <gridcharger/huawei/TWAI.h>
#include <powermeter/Controller.h>
#include <PowerLimiter.h>
#include <Configuration.h>
#include <LogHelper.h>
#include <MqttSettings.h>

#undef TAG
static const char* TAG = "gridCharger";
static const char* SUBTAG = "Huawei";

#include <functional>
#include <algorithm>
#include <cmath>
#include <limits>

namespace GridChargers::Huawei {

// Wait time/current before shuting down the PSU / charger
// This is set to allow the fan to run for some time
#define HUAWEI_AUTO_MODE_SHUTDOWN_DELAY 60000
#define HUAWEI_AUTO_MODE_SHUTDOWN_CURRENT 0.75
#define HUAWEI_AUTO_MODE_STABILIZATION_DELAY 5000
#define HUAWEI_AUTO_MODE_LOWER_LIMIT_HOLD_DELAY 10000
#define HUAWEI_AUTO_MODE_DEFAULT_EFFICIENCY 0.9f
#define HUAWEI_AUTO_MODE_BMS_CURRENT_MARGIN 0.5f

namespace {

constexpr uint32_t AutoPowerTargetZeroHoldMillis = 5 * 1000;
constexpr uint32_t AutoPowerStartupQualificationMillis = 5 * 1000;
constexpr float AutoPowerLowerLimitHoldAbortGridImportWatts = 100.0f;
constexpr float AutoPowerLowerLimitHoldAbortLowerLimitRatio = 0.5f;

bool millisAtOrAfter(uint32_t timestamp, uint32_t reference)
{
    auto constexpr halfOfAllMillis = std::numeric_limits<uint32_t>::max() / 2;
    return (timestamp - reference) < halfOfAllMillis;
}

float calcExponentialSmoothingAlpha(uint32_t elapsedMillis, uint16_t timeConstantSeconds)
{
    if (elapsedMillis == 0 || timeConstantSeconds == 0) { return 0.0f; }

    auto const elapsedSeconds = static_cast<float>(elapsedMillis) / 1000.0f;
    auto const timeConstant = static_cast<float>(timeConstantSeconds);
    return std::clamp(1.0f - std::exp(-elapsedSeconds / timeConstant), 0.0f, 1.0f);
}

bool batteryProviderUsesTwai(uint8_t provider)
{
    return provider == 0  // Pylontech
        || provider == 4  // Pytes
        || provider == 5; // SBS
}

uint16_t wattsToUint16(float watts)
{
    return static_cast<uint16_t>(std::clamp<float>(
            std::round(watts),
            0.0f,
            static_cast<float>(std::numeric_limits<uint16_t>::max())));
}

} // namespace

bool Provider::init()
{
    DTU_LOGI("Initialize Huawei AC charger interface...");

    _upHardwareInterface.reset(nullptr);

    auto const& config = Configuration.get();

    switch (config.GridCharger.Can.HardwareInterface) {
        case GridChargerHardwareInterface::MCP2515:
            _upHardwareInterface = std::make_unique<MCP2515>();
            break;
        case GridChargerHardwareInterface::TWAI:
            if (config.Battery.Enabled && batteryProviderUsesTwai(config.Battery.Provider)) {
                DTU_LOGE("TWAI is already reserved for the battery CAN provider; use MCP2515 for the Huawei charger");
                return false;
            }
            _upHardwareInterface = std::make_unique<TWAI>();
            break;
        default:
            DTU_LOGE("Unknown hardware interface setting %d", config.GridCharger.Can.HardwareInterface);
            return false;
            break;
    }

    if (!_upHardwareInterface->init()) {
        DTU_LOGE("Error initializing hardware interface");
        _upHardwareInterface.reset(nullptr);
        return false;
    };

    _autoPowerTargetPowerConsumption = calcAutoPowerTargetPowerConsumption();

    auto const& pin = PinMapping.get();
    if (pin.huawei_power > GPIO_NUM_NC) {
        _huaweiPower = pin.huawei_power;
        pinMode(_huaweiPower, OUTPUT);
        disableOutput();
    }

    _mode = HUAWEI_MODE_AUTO_EXT;
    if (config.GridCharger.AutoPowerEnabled) {
        _mode = HUAWEI_MODE_AUTO_INT;
    }

    // Set initial mode in datapoints
    _dataPoints.add<DataPointLabel::Mode>(_mode, true);

    subscribeTopics();

    DTU_LOGI("Hardware Interface initialized successfully");
    return true;
}

void Provider::deinit()
{
    std::lock_guard<std::mutex> lock(_mutex);

    _upHardwareInterface.reset(nullptr);
    unsubscribeTopics();
}

void Provider::enableOutput()
{
    if (_oOutputEnabled.value_or(false)) { return; }

    _setProduction(true);
    _oOutputEnabled = true;
    holdAutoPowerTargetPowerConsumptionAtZero();

    if (_huaweiPower <= GPIO_NUM_NC) { return; }
    digitalWrite(_huaweiPower, 0);
}

void Provider::disableOutput()
{
    if (!_oOutputEnabled.value_or(true)) { return; }

    _setProduction(false);
    _oOutputEnabled = false;

    if (_huaweiPower <= GPIO_NUM_NC) { return; }
    digitalWrite(_huaweiPower, 1);
}

void Provider::subscribeTopics()
{
    String const& prefix = MqttSettings.getPrefix();

    auto subscribe = [&prefix, this](char const* subTopic, Topic t) {
        String fullTopic(prefix + _cmdtopic.data() + subTopic);
        MqttSettings.subscribe(fullTopic.c_str(), 0,
                std::bind(&Provider::onMqttMessage, this, t,
                    std::placeholders::_1, std::placeholders::_2,
                    std::placeholders::_3, std::placeholders::_4));
    };

    for (auto const& s : _subscriptions) {
        subscribe(s.first.data(), s.second);
    }
}

void Provider::unsubscribeTopics()
{
    String const prefix = MqttSettings.getPrefix() + _cmdtopic.data();
    for (auto const& s : _subscriptions) {
        MqttSettings.unsubscribe(prefix + s.first.data());
    }
}

void Provider::loop()
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upHardwareInterface) { return; }

    auto const& config = Configuration.get();

    if (!config.GridCharger.Enabled) {
        _autoPowerLimitedByAvailablePower = false;
        return;
    }

    auto upNewData = _upHardwareInterface->getCurrentData();
    if (upNewData) {
        _dataPoints.updateFrom(*upNewData);
        _stats->updateFrom(*upNewData);
    }

    auto oOutputCurrent = _dataPoints.get<DataPointLabel::OutputCurrent>();
    auto oOutputVoltage = _dataPoints.get<DataPointLabel::OutputVoltage>();
    auto oOutputPower = _dataPoints.get<DataPointLabel::OutputPower>();
    auto oOnlineCurrent = _dataPoints.get<DataPointLabel::OnlineCurrent>();
    auto oEfficiency = _dataPoints.get<DataPointLabel::Efficiency>();
    float efficiency = HUAWEI_AUTO_MODE_DEFAULT_EFFICIENCY;
    if (oEfficiency && *oEfficiency > 50.0f) {
        efficiency = std::clamp(*oEfficiency / 100.0f, 0.5f, 1.0f);
    }

    // Internal PSU power pin (slot detect) control
    if (oOutputCurrent && *oOutputCurrent > HUAWEI_AUTO_MODE_SHUTDOWN_CURRENT) {
        _outputCurrentOnSinceMillis = millis();
    }

    if (_outputCurrentOnSinceMillis + HUAWEI_AUTO_MODE_SHUTDOWN_DELAY < millis() &&
            (_mode == HUAWEI_MODE_AUTO_EXT || _mode == HUAWEI_MODE_AUTO_INT)) {
        disableOutput();
    }

    using Setting = HardwareInterface::Setting;

    if (_mode == HUAWEI_MODE_AUTO_INT || _batteryEmergencyCharging) {

        // Set voltage limit in periodic intervals if we're in auto mode or if emergency battery charge is requested.
        if ( _nextAutoModePeriodicIntMillis < millis()) {
            DTU_LOGI("Periodically setting voltage limit: %f", config.GridCharger.AutoPowerVoltageLimit);
            _setParameter(config.GridCharger.AutoPowerVoltageLimit, Setting::OnlineVoltage);
            _nextAutoModePeriodicIntMillis = millis() + 60000;
        }
    }

    // ***********************
    // Emergency charge
    // ***********************
    auto stats = Battery.getStats();
    if (!_batteryEmergencyCharging && config.GridCharger.EmergencyChargeEnabled && stats->getImmediateChargingRequest()) {
        _autoPowerLimitedByAvailablePower = false;
        if (!oOutputVoltage) {
            // TODO(schlimmchen): if this situation actually occurs, this message
            // will be printed with high frequency for a prolonged time. how can
            // we deal with that?
            DTU_LOGW("Cannot perform emergency charging with unknown PSU output voltage value");
            return;
        }

        _batteryEmergencyCharging = true;

        // Set output current
        float outputCurrent = config.GridCharger.AutoPowerUpperPowerLimit / *oOutputVoltage;
        DTU_LOGI("Emergency Charge Output current %.02f", outputCurrent);
        _setParameter(outputCurrent, Setting::OnlineCurrent);
        return;
    }

    if (_batteryEmergencyCharging && !stats->getImmediateChargingRequest()) {
        _autoPowerLimitedByAvailablePower = false;
        // Battery request has changed. Set current to 0, wait for PSU to respond and then clear state
        // TODO(schlimmchen): this is repeated very often for up to (polling interval) seconds. maybe
        // trigger sending request for data immediately? otherwise implement a backoff instead.
        _setParameter(0, Setting::OnlineCurrent);
        if (oOutputCurrent && *oOutputCurrent < 1) {
            _batteryEmergencyCharging = false;
        }
        return;
    }

    // ***********************
    // Automatic power control
    // ***********************
    if (_mode == HUAWEI_MODE_AUTO_INT ) {
        if (!oOutputVoltage || !oOutputPower || !oOutputCurrent) {
            _autoPowerLimitedByAvailablePower = false;
            DTU_LOGW("Cannot perform auto power control while critical PSU values are still unknown");
            _autoModeBlockedTillMillis = millis() + 1000;
            return;
        }

        // Re-enable automatic power control if the output voltage has dropped below threshold
        if (oOutputVoltage && *oOutputVoltage < config.GridCharger.AutoPowerEnableVoltageLimit ) {
            _autoPowerEnabledCounter = 10;
        }

        bool const powerLimiterManaged = PowerLimiter.isGridChargerManaged();
        if (!powerLimiterManaged) {
            _oPowerLimiterTargetInputPowerWatts = std::nullopt;
        }

        if (!powerLimiterManaged && PowerLimiter.isGovernedBatteryPoweredInverterProducing()) {
            _autoPowerLimitedByAvailablePower = false;
            _setParameter(0.0, Setting::OnlineCurrent);
            _autoPowerReachedLowerPowerLimit = false;
            // Don't run auto mode for a second now. Otherwise we may send too much over the CAN bus
            _autoModeBlockedTillMillis = millis() + 1000;
            DTU_LOGI("Inverter is active, disable PSU");
            return;
        }

        if (config.Battery.Enabled && stats->updateAvailable(_lastBatteryUpdateReceivedMillis)) {
            _lastBatteryUpdateReceivedMillis = millis();

            if (stats->isChargeCurrentLimitValid()) {
                float chargeCurrentLimit = stats->getChargeCurrentLimit();
                float outputCurrentLimit = std::max(
                        chargeCurrentLimit - HUAWEI_AUTO_MODE_BMS_CURRENT_MARGIN, 0.0f);

                float otherChargeCurrent = 0.0f;
                if (stats->isCurrentValid()) {
                    otherChargeCurrent = stats->getChargeCurrent() - *oOutputCurrent;
                    otherChargeCurrent = std::max(otherChargeCurrent, 0.0f);
                }

                float outputCurrent = std::max(outputCurrentLimit - otherChargeCurrent, 0.0f);
                float acknowledgedOutputCurrent = oOnlineCurrent.value_or(*oOutputCurrent);
                float currentToLimit = std::max(*oOutputCurrent, acknowledgedOutputCurrent);

                if (currentToLimit > outputCurrent) {
                    _autoPowerLimitedByAvailablePower = false;
                    DTU_LOGD("Huawei current %.2fA exceeds BMS permissible %.2fA "
                            "(output %.2fA, ack %.2fA, limit %.2fA, other charge %.2fA), reducing immediately",
                            currentToLimit, outputCurrent, *oOutputCurrent,
                            acknowledgedOutputCurrent, chargeCurrentLimit, otherChargeCurrent);

                    _autoPowerEnabled = outputCurrent > HUAWEI_AUTO_MODE_SHUTDOWN_CURRENT;
                    _setParameter(outputCurrent, Setting::OnlineCurrent);
                    _oPowerLimiterTargetInputPowerWatts = wattsToUint16(
                            (outputCurrent * *oOutputVoltage) / efficiency);
                    _autoModeBlockedTillMillis = millis() + HUAWEI_AUTO_MODE_STABILIZATION_DELAY;
                    return;
                }
            }
        }

        // Check if we should run the normal automatic power calculation at all.
        // BMS and inverter limit corrections above intentionally bypass this.
        if (_autoModeBlockedTillMillis > millis()) {
            return;
        }

        if (powerLimiterManaged) {
            return;
        }

        if (PowerMeter.getLastUpdate() > _lastPowerMeterUpdateReceivedMillis &&
                _autoPowerEnabledCounter > 0) {
            // We have received a new PowerMeter value. Also we're _autoPowerEnabled
            // So we're good to calculate a new limit

            _lastPowerMeterUpdateReceivedMillis = PowerMeter.getLastUpdate();
            if (*oOutputPower < config.GridCharger.AutoPowerLowerPowerLimit) {
                holdAutoPowerTargetPowerConsumptionAtZero();
            }
            bool const updateDynamicTarget =
                !isAutoPowerTargetPowerConsumptionZeroHoldActive();
            updateDynamicAutoPowerTarget(updateDynamicTarget);
            auto const targetPowerConsumption = getEffectiveAutoPowerTargetPowerConsumption();

            // input power diff will be (close to) zero if the power meter value
            // reads what the user specified as the target power consumption.
            float inputPowerDiff = -1 * round(PowerMeter.getPowerTotal());
            inputPowerDiff += targetPowerConsumption;

            // Target output power is current output adjusted for the difference to
            // desired input power.
            float rawOutputPowerTarget = *oOutputPower + (inputPowerDiff * efficiency);
            float newOutputPowerTarget = rawOutputPowerTarget;

            DTU_LOGD("targeting %d W, input diff: %.0f, raw output target: %.0f, current output: %.01f",
                targetPowerConsumption, inputPowerDiff, rawOutputPowerTarget, *oOutputPower);

            bool stopRequestedByBatterySoC = false;

            // Check whether the battery SoC limit setting is enabled
            if (config.Battery.Enabled && config.GridCharger.AutoPowerBatterySoCLimitsEnabled) {
                auto const batterySoC = Battery.getStats()->getSoC();
                auto const stopBatterySoCThreshold = GridCharger.getAutoPowerStopBatterySoCThreshold();
                // Sets power limit to 0 if the BMS reported SoC reaches or exceeds the user configured value
                if (batterySoC >= stopBatterySoCThreshold) {
                    stopRequestedByBatterySoC = true;
                    _autoPowerLimitedByAvailablePower = false;
                    rawOutputPowerTarget = 0;
                    newOutputPowerTarget = 0;
                    DTU_LOGD("Current battery SoC %.1f reached stop threshold %i, so new output target is %f",
                            batterySoC, stopBatterySoCThreshold, newOutputPowerTarget);
                }
            }

            auto const plannedOutputPowerLimit = GridCharger.getAutoPowerSocPlanningOutputPowerLimit(*oOutputPower);
            if (plannedOutputPowerLimit) {
                rawOutputPowerTarget = std::min(rawOutputPowerTarget, *plannedOutputPowerLimit);
                newOutputPowerTarget = std::min(newOutputPowerTarget, *plannedOutputPowerLimit);
            }
            auto autoPowerLimit = static_cast<float>(config.GridCharger.AutoPowerUpperPowerLimit);
            if (plannedOutputPowerLimit) {
                autoPowerLimit = std::min(autoPowerLimit, *plannedOutputPowerLimit);
            }
            _autoPowerLimitedByAvailablePower = !stopRequestedByBatterySoC
                && rawOutputPowerTarget < autoPowerLimit;

            auto const lowerPowerLimit = static_cast<float>(config.GridCharger.AutoPowerLowerPowerLimit);
            if (*oOutputPower >= lowerPowerLimit) {
                _autoPowerReachedLowerPowerLimit = true;
            }

            auto const lowerLimitHoldAbortGridImport = std::max(
                    AutoPowerLowerLimitHoldAbortGridImportWatts,
                    lowerPowerLimit * AutoPowerLowerLimitHoldAbortLowerLimitRatio);
            bool const abortLowerLimitHold = inputPowerDiff < -lowerLimitHoldAbortGridImport;
            bool const lowerLimitHoldAllowed = !stopRequestedByBatterySoC
                && !abortLowerLimitHold
                && _autoPowerReachedLowerPowerLimit
                && (!plannedOutputPowerLimit || *plannedOutputPowerLimit >= lowerPowerLimit);
            if (rawOutputPowerTarget >= lowerPowerLimit) {
                _autoPowerLowerLimitHoldTillMillis = 0;
            } else if (abortLowerLimitHold) {
                if (isAutoPowerLowerLimitHoldActive()) {
                    DTU_LOGI("Aborting lower power limit hold because grid import exceeds target by %.0f W",
                            -inputPowerDiff);
                }
                _autoPowerLowerLimitHoldTillMillis = 0;
            } else if (lowerLimitHoldAllowed) {
                if (_autoPowerLowerLimitHoldTillMillis == 0) {
                    _autoPowerLowerLimitHoldTillMillis = millis() + HUAWEI_AUTO_MODE_LOWER_LIMIT_HOLD_DELAY;
                    DTU_LOGI("Holding charger at lower power limit %.0f W for %u s before shutdown",
                            lowerPowerLimit, HUAWEI_AUTO_MODE_LOWER_LIMIT_HOLD_DELAY / 1000);
                }

                if (isAutoPowerLowerLimitHoldActive()) {
                    rawOutputPowerTarget = lowerPowerLimit;
                    newOutputPowerTarget = lowerPowerLimit;
                }
            }

            if (rawOutputPowerTarget >= lowerPowerLimit) {
                // Check if the output power has dropped below the lower limit (i.e. the battery is full)
                // and if the PSU should be turned off. Also we use a simple counter mechanism here to be able
                // to ramp up from zero output power when starting up
                if (*oOutputPower < lowerPowerLimit) {
                    bool const startupNeedsQualification = !_autoPowerReachedLowerPowerLimit
                        && _lastRequestedOnlineCurrent <= HUAWEI_AUTO_MODE_SHUTDOWN_CURRENT;
                    if (startupNeedsQualification) {
                        auto const now = millis();
                        if (_autoPowerStartupQualificationSinceMillis == 0) {
                            _autoPowerStartupQualificationSinceMillis = now;
                            DTU_LOGI("Waiting %u s before starting charger at lower power limit %.0f W",
                                    AutoPowerStartupQualificationMillis / 1000,
                                    lowerPowerLimit);
                            return;
                        }

                        auto const qualificationTillMillis =
                            _autoPowerStartupQualificationSinceMillis + AutoPowerStartupQualificationMillis;
                        if (!millisAtOrAfter(now, qualificationTillMillis)) {
                            return;
                        }

                        _autoPowerStartupQualificationSinceMillis = 0;
                        DTU_LOGI("Starting charger after sustained lower power limit request");
                    } else {
                        _autoPowerStartupQualificationSinceMillis = 0;
                    }

                    DTU_LOGI("Charger output %.0f W is below lower power limit %.0f W while target is active",
                            *oOutputPower, lowerPowerLimit);
                    _autoPowerEnabledCounter--;
                    if (_autoPowerEnabledCounter == 0) {
                        _autoPowerEnabled = false;
                        _setParameter(0.0, Setting::OnlineCurrent);
                        return;
                    }
                } else {
                    _autoPowerStartupQualificationSinceMillis = 0;
                    _autoPowerEnabledCounter = 10;
                }

                newOutputPowerTarget = rawOutputPowerTarget;
                if (*oOutputPower < lowerPowerLimit) {
                    newOutputPowerTarget = std::max(newOutputPowerTarget, lowerPowerLimit);
                }

                newOutputPowerTarget = std::min(newOutputPowerTarget, config.GridCharger.AutoPowerUpperPowerLimit);
                if (plannedOutputPowerLimit) {
                    newOutputPowerTarget = std::min(newOutputPowerTarget, *plannedOutputPowerLimit);
                }

                float calculatedCurrent = newOutputPowerTarget / *oOutputVoltage;

                float outputCurrent = calculatedCurrent;

                // Limit the command directly to the current value requested by
                // the BMS. The measured battery current can lag by several
                // seconds, so it is only used to subtract other charging sources.
                if (config.Battery.Enabled && stats->isChargeCurrentLimitValid()) {
                    float chargeCurrentLimit = stats->getChargeCurrentLimit();
                    float outputCurrentLimit = std::max(
                            chargeCurrentLimit - HUAWEI_AUTO_MODE_BMS_CURRENT_MARGIN, 0.0f);

                    float otherChargeCurrent = 0.0f;
                    if (stats->isCurrentValid()) {
                        otherChargeCurrent = stats->getChargeCurrent() - *oOutputCurrent;
                        otherChargeCurrent = std::max(otherChargeCurrent, 0.0f);
                    }

                    float permissibleCurrent = std::max(outputCurrentLimit - otherChargeCurrent, 0.0f);
                    outputCurrent = std::min(outputCurrent, permissibleCurrent);
                    if (outputCurrent < calculatedCurrent - 0.1f) {
                        _autoPowerLimitedByAvailablePower = false;
                    }

                    DTU_LOGD("Setting output current to %.2fA. Calculated %.2fA, "
                            "BMS limit %.2fA, other charge %.2fA, permissible %.2fA",
                            outputCurrent, calculatedCurrent, chargeCurrentLimit,
                            otherChargeCurrent, permissibleCurrent);
                } else {
                    DTU_LOGD("Setting output current to %.2fA. Calculated %.2fA, "
                            "no valid BMS charge current limit",
                            outputCurrent, calculatedCurrent);
                }

                outputCurrent = outputCurrent > 0 ? outputCurrent : 0;

                _autoPowerEnabled = true;
                _setParameter(outputCurrent, Setting::OnlineCurrent);

                // Don't run auto mode some time to allow for output stabilization after issuing a new value
                _autoModeBlockedTillMillis = millis() + HUAWEI_AUTO_MODE_STABILIZATION_DELAY;
            } else {
                // requested PL is below minium. Set current to 0
                _autoPowerEnabled = false;
                _autoPowerLowerLimitHoldTillMillis = 0;
                _autoPowerStartupQualificationSinceMillis = 0;
                _autoPowerReachedLowerPowerLimit = false;
                _setParameter(0.0, Setting::OnlineCurrent);
            }
        }
    } else {
        _autoPowerLimitedByAvailablePower = false;
    }
}

void Provider::holdAutoPowerTargetPowerConsumptionAtZero()
{
    if (PowerLimiter.isGridChargerManaged()) {
        _autoPowerTargetPowerConsumptionZeroHoldTillMillis = 0;
        return;
    }

    bool const wasActive = isAutoPowerTargetPowerConsumptionZeroHoldActive();
    _autoPowerTargetPowerConsumptionZeroHoldTillMillis = millis() + AutoPowerTargetZeroHoldMillis;
    resetDynamicAutoPowerTargetState();
    _autoPowerTargetPowerConsumption = 0;

    if (!wasActive) {
        DTU_LOGI("Holding charger grid target at 0 W for %u s while startup settles",
                AutoPowerTargetZeroHoldMillis / 1000);
    }
}

bool Provider::isAutoPowerTargetPowerConsumptionZeroHoldActive() const
{
    auto const holdTillMillis = _autoPowerTargetPowerConsumptionZeroHoldTillMillis;
    return holdTillMillis != 0 && !millisAtOrAfter(millis(), holdTillMillis);
}

bool Provider::isAutoPowerLowerLimitHoldActive() const
{
    auto const holdTillMillis = _autoPowerLowerLimitHoldTillMillis;
    return holdTillMillis != 0 && !millisAtOrAfter(millis(), holdTillMillis);
}

int16_t Provider::getEffectiveAutoPowerTargetPowerConsumption() const
{
    if (isAutoPowerTargetPowerConsumptionZeroHoldActive()) { return 0; }
    return _autoPowerTargetPowerConsumption;
}

void Provider::resetDynamicAutoPowerTargetState()
{
    _lastDynamicAutoPowerTargetPowerMeterUpdate = 0;
    _dynamicAutoPowerTargetMean = 0.0f;
    _dynamicAutoPowerTargetVariance = 0.0f;
    _dynamicAutoPowerTargetInitialized = false;
}

void Provider::updateDynamicAutoPowerTarget(bool chargerControlActive)
{
    auto const& config = Configuration.get();

    if (!config.GridCharger.AutoPowerTargetPowerConsumptionDynamicEnabled
            || config.GridCharger.AutoPowerTargetPowerConsumptionDynamicWindow == 0
            || !PowerMeter.isDataValid()) {
        resetDynamicAutoPowerTargetState();
        _autoPowerTargetPowerConsumption = calcAutoPowerTargetPowerConsumption();
        return;
    }

    if (!chargerControlActive) {
        resetDynamicAutoPowerTargetState();
        return;
    }

    auto const powerMeterUpdate = PowerMeter.getLastUpdate();
    if (powerMeterUpdate == _lastDynamicAutoPowerTargetPowerMeterUpdate) { return; }

    auto const previousPowerMeterUpdate = _lastDynamicAutoPowerTargetPowerMeterUpdate;
    _lastDynamicAutoPowerTargetPowerMeterUpdate = powerMeterUpdate;

    auto const meterValue = PowerMeter.getPowerTotal();
    int32_t roundedMeterValue = static_cast<int32_t>(meterValue + (meterValue > 0 ? 0.5 : -0.5));
    auto const watts = static_cast<float>(std::clamp<int32_t>(
            roundedMeterValue,
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max()));

    if (!_dynamicAutoPowerTargetInitialized) {
        _dynamicAutoPowerTargetMean = watts;
        _dynamicAutoPowerTargetVariance = 0.0f;
        _dynamicAutoPowerTargetInitialized = true;
        return;
    }

    auto const alpha = calcExponentialSmoothingAlpha(
            powerMeterUpdate - previousPowerMeterUpdate,
            config.GridCharger.AutoPowerTargetPowerConsumptionDynamicWindow);
    if (alpha <= 0.0f) { return; }

    auto const previousMean = _dynamicAutoPowerTargetMean;
    auto const deviation = watts - previousMean;
    _dynamicAutoPowerTargetMean = previousMean + (alpha * deviation);
    _dynamicAutoPowerTargetVariance = (1.0f - alpha)
        * (_dynamicAutoPowerTargetVariance + (alpha * deviation * deviation));

    auto const calculatedTarget = calcAutoPowerTargetPowerConsumption();
    auto const smoothedTarget = static_cast<int32_t>(std::round(
            static_cast<float>(_autoPowerTargetPowerConsumption)
            + (alpha * (static_cast<float>(calculatedTarget) - _autoPowerTargetPowerConsumption))));
    _autoPowerTargetPowerConsumption = static_cast<int16_t>(std::clamp<int32_t>(
            smoothedTarget,
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max()));
}

int16_t Provider::calcAutoPowerTargetPowerConsumption() const
{
    auto const& config = Configuration.get();
    auto const staticTarget = static_cast<int32_t>(std::round(config.GridCharger.AutoPowerTargetPowerConsumption));
    auto const boundedStaticTarget = std::clamp<int32_t>(
            staticTarget,
            std::numeric_limits<int16_t>::min(),
            std::numeric_limits<int16_t>::max());

    if (!config.GridCharger.AutoPowerTargetPowerConsumptionDynamicEnabled
            || !_dynamicAutoPowerTargetInitialized) {
        return static_cast<int16_t>(boundedStaticTarget);
    }

    auto const maxDynamicTarget = static_cast<uint16_t>(std::numeric_limits<int16_t>::max());
    auto const configuredMaxTarget = std::min(
            config.GridCharger.AutoPowerTargetPowerConsumptionDynamicMax,
            maxDynamicTarget);
    auto const multiplier = std::max(0.0f, config.GridCharger.AutoPowerTargetPowerConsumptionDynamicMultiplier);
    auto calculatedTarget = std::round(std::sqrt(std::max(0.0f, _dynamicAutoPowerTargetVariance)) * multiplier);
    calculatedTarget = std::clamp(calculatedTarget, 0.0f, static_cast<float>(maxDynamicTarget));
    auto const dynamicTargetMagnitude = std::min<uint16_t>(
            configuredMaxTarget,
            static_cast<uint16_t>(calculatedTarget));

    if (dynamicTargetMagnitude == 0) {
        return static_cast<int16_t>(boundedStaticTarget);
    }

    // Dynamic mode produces an absolute grid target, not an offset added to the
    // static target. The more negative target wins because the charger should
    // keep the configured minimum export margin when dynamic demand is lower.
    auto const dynamicTarget = -static_cast<int16_t>(dynamicTargetMagnitude);
    return std::min<int16_t>(
            static_cast<int16_t>(boundedStaticTarget),
            dynamicTarget);
}

float Provider::getEfficiency() const
{
    auto oEfficiency = _dataPoints.get<DataPointLabel::Efficiency>();
    if (oEfficiency && *oEfficiency > 50.0f) {
        return std::clamp(*oEfficiency / 100.0f, 0.5f, 1.0f);
    }

    return HUAWEI_AUTO_MODE_DEFAULT_EFFICIENCY;
}

std::optional<float> Provider::getCurrentInputPowerWatts() const
{
    auto oInputPower = _dataPoints.get<DataPointLabel::InputPower>();
    if (oInputPower) { return std::max(0.0f, *oInputPower); }

    auto oOutputPower = _dataPoints.get<DataPointLabel::OutputPower>();
    if (!oOutputPower) { return std::nullopt; }

    return std::max(0.0f, *oOutputPower) / getEfficiency();
}

uint16_t Provider::getPowerLimiterCurrentInputPowerWatts() const
{
    return wattsToUint16(getCurrentInputPowerWatts().value_or(0.0f));
}

uint16_t Provider::getPowerLimiterExpectedInputPowerWatts() const
{
    return _oPowerLimiterTargetInputPowerWatts.value_or(getPowerLimiterCurrentInputPowerWatts());
}

std::optional<uint32_t> Provider::getPowerLimiterOutputReferenceMillis() const
{
    if (_powerLimiterCommandMillis != 0) {
        return _powerLimiterCommandMillis + HUAWEI_AUTO_MODE_STABILIZATION_DELAY;
    }

    auto const statsMillis = _stats->getLastUpdate();
    if (statsMillis == 0 && getPowerLimiterCurrentInputPowerWatts() > 0) {
        return std::nullopt;
    }

    return statsMillis;
}

float Provider::getPowerLimiterMaxInputPowerWattsFloat() const
{
    auto const& config = Configuration.get();
    if (!config.GridCharger.Enabled
            || !config.GridCharger.AutoPowerEnabled
            || _mode != HUAWEI_MODE_AUTO_INT) {
        return 0.0f;
    }

    auto oOutputVoltage = _dataPoints.get<DataPointLabel::OutputVoltage>();
    if (!oOutputVoltage || *oOutputVoltage <= 0.0f) { return 0.0f; }

    auto stats = Battery.getStats();
    if (config.Battery.Enabled && config.GridCharger.AutoPowerBatterySoCLimitsEnabled) {
        auto const batterySoC = stats->getSoC();
        auto const stopBatterySoCThreshold = GridCharger.getAutoPowerStopBatterySoCThreshold();
        if (batterySoC >= stopBatterySoCThreshold) { return 0.0f; }
    }

    float maxOutputPower = static_cast<float>(config.GridCharger.AutoPowerUpperPowerLimit);
    auto const plannedOutputPowerLimit = GridCharger.getAutoPowerSocPlanningOutputPowerLimit(
            _dataPoints.get<DataPointLabel::OutputPower>().value_or(0.0f));
    if (plannedOutputPowerLimit) {
        maxOutputPower = std::min(maxOutputPower, *plannedOutputPowerLimit);
    }

    if (config.Battery.Enabled && stats->isChargeCurrentLimitValid()) {
        float chargeCurrentLimit = stats->getChargeCurrentLimit();
        float outputCurrentLimit = std::max(
                chargeCurrentLimit - HUAWEI_AUTO_MODE_BMS_CURRENT_MARGIN, 0.0f);

        float otherChargeCurrent = 0.0f;
        auto oOutputCurrent = _dataPoints.get<DataPointLabel::OutputCurrent>();
        if (stats->isCurrentValid() && oOutputCurrent) {
            otherChargeCurrent = stats->getChargeCurrent() - *oOutputCurrent;
            otherChargeCurrent = std::max(otherChargeCurrent, 0.0f);
        }

        float permissibleCurrent = std::max(outputCurrentLimit - otherChargeCurrent, 0.0f);
        maxOutputPower = std::min(maxOutputPower, permissibleCurrent * *oOutputVoltage);
    }

    if (maxOutputPower <= 0.0f) { return 0.0f; }

    return maxOutputPower / getEfficiency();
}

uint16_t Provider::getPowerLimiterMaxInputPowerWatts() const
{
    return wattsToUint16(getPowerLimiterMaxInputPowerWattsFloat());
}

void Provider::setPowerLimiterInputPowerWatts(float inputPower)
{
    inputPower = std::clamp(inputPower, 0.0f, getPowerLimiterMaxInputPowerWattsFloat());

    auto const minInputPower = static_cast<float>(Configuration.get().GridCharger.AutoPowerLowerPowerLimit)
        / getEfficiency();
    if (inputPower > 0.0f && inputPower < minInputPower) {
        inputPower = 0.0f;
    }

    auto const targetInputPower = wattsToUint16(inputPower);
    if (_oPowerLimiterTargetInputPowerWatts
            && *_oPowerLimiterTargetInputPowerWatts == targetInputPower) {
        return;
    }

    auto oOutputVoltage = _dataPoints.get<DataPointLabel::OutputVoltage>();
    if (targetInputPower > 0 && (!oOutputVoltage || *oOutputVoltage <= 0.0f)) {
        return;
    }

    auto const outputCurrent = targetInputPower > 0
        ? (static_cast<float>(targetInputPower) * getEfficiency()) / *oOutputVoltage
        : 0.0f;

    _autoPowerEnabled = outputCurrent > HUAWEI_AUTO_MODE_SHUTDOWN_CURRENT;
    if (_autoPowerEnabled) { _autoPowerEnabledCounter = 10; }
    _setParameter(outputCurrent, HardwareInterface::Setting::OnlineCurrent);
    _powerLimiterCommandMillis = millis();
    _oPowerLimiterTargetInputPowerWatts = targetInputPower;
    _autoModeBlockedTillMillis = millis() + HUAWEI_AUTO_MODE_STABILIZATION_DELAY;
}

uint16_t Provider::applyPowerLimiterInputPowerIncrease(uint16_t increase)
{
    if (increase == 0) { return 0; }

    auto const expected = getPowerLimiterExpectedInputPowerWatts();
    auto const maximum = getPowerLimiterMaxInputPowerWatts();
    if (maximum <= expected) { return 0; }

    auto const minInputPower = wattsToUint16(
            static_cast<float>(Configuration.get().GridCharger.AutoPowerLowerPowerLimit)
            / getEfficiency());
    if (expected == 0 && increase < minInputPower) { return 0; }

    auto const target = static_cast<uint16_t>(std::min<uint32_t>(
            static_cast<uint32_t>(expected) + increase,
            maximum));
    setPowerLimiterInputPowerWatts(target);
    return target > expected ? target - expected : 0;
}

uint16_t Provider::applyPowerLimiterInputPowerReduction(uint16_t reduction)
{
    if (reduction == 0) { return 0; }

    auto const expected = getPowerLimiterExpectedInputPowerWatts();
    if (expected == 0) { return 0; }

    uint16_t target = expected > reduction ? expected - reduction : 0;
    auto const minInputPower = wattsToUint16(
            static_cast<float>(Configuration.get().GridCharger.AutoPowerLowerPowerLimit)
            / getEfficiency());
    if (target > 0 && target < minInputPower) {
        target = 0;
    }

    setPowerLimiterInputPowerWatts(target);
    return expected - target;
}

void Provider::setFan(bool online, bool fullSpeed)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upHardwareInterface) { return; }

    using Setting = HardwareInterface::Setting;
    auto setting = online ? Setting::FanOnlineFullSpeed : Setting::FanOfflineFullSpeed;
    _upHardwareInterface->setParameter(setting, fullSpeed ? 1 : 0);
}

void Provider::_setProduction(bool enable) const
{
    auto setting = HardwareInterface::Setting::ProductionDisable;
    _upHardwareInterface->setParameter(setting, enable ? 0 : 1);
}

void Provider::setProduction(bool enable)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upHardwareInterface) { return; }
    if (enable) { holdAutoPowerTargetPowerConsumptionAtZero(); }
    _setProduction(enable);
}

void Provider::setParameter(float val, HardwareInterface::Setting setting)
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (_mode == HUAWEI_MODE_AUTO_INT &&
        setting != HardwareInterface::Setting::OfflineVoltage &&
        setting != HardwareInterface::Setting::OfflineCurrent) { return; }

    // set pollFeedback to true only if we're setting a value because of a
    // request from the UI or MQTT. otherwise, we'll wait for the next
    // interval to get the new value.
    _setParameter(val, setting, true/*pollFeedback*/);
}

void Provider::_setParameter(float val, HardwareInterface::Setting setting, bool pollFeedback)
{
    // NOTE: the mutex is locked by any method calling this private method

    if (!_upHardwareInterface) { return; }

    if (val < 0) {
        DTU_LOGE("Tried to set voltage/current to negative value %.2f", val);
        return;
    }

    using Setting = HardwareInterface::Setting;

    // Start PSU if needed
    if (val > HUAWEI_AUTO_MODE_SHUTDOWN_CURRENT &&
            setting == Setting::OnlineCurrent &&
            (_mode == HUAWEI_MODE_AUTO_EXT || _mode == HUAWEI_MODE_AUTO_INT)) {
        if (_lastRequestedOnlineCurrent <= HUAWEI_AUTO_MODE_SHUTDOWN_CURRENT) {
            holdAutoPowerTargetPowerConsumptionAtZero();
        }
        enableOutput();
        _outputCurrentOnSinceMillis = millis();
    }

    _upHardwareInterface->setParameter(setting, val, pollFeedback);

    if (setting == Setting::OnlineCurrent) {
        _lastRequestedOnlineCurrent = val;
        if (val <= HUAWEI_AUTO_MODE_SHUTDOWN_CURRENT) {
            _autoPowerStartupQualificationSinceMillis = 0;
            _autoPowerReachedLowerPowerLimit = false;
        }
    }
}

void Provider::setMode(uint8_t mode) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_upHardwareInterface) { return; }

    if (mode == HUAWEI_MODE_OFF) {
        disableOutput();
        _mode = HUAWEI_MODE_OFF;
    }
    if (mode == HUAWEI_MODE_ON) {
        enableOutput();
        _mode = HUAWEI_MODE_ON;
    }

    // Update mode in datapoints
    _dataPoints.add<DataPointLabel::Mode>(_mode, true);

    auto const& config = Configuration.get();

    if (mode == HUAWEI_MODE_AUTO_INT && !config.GridCharger.AutoPowerEnabled ) {
        DTU_LOGW("Trying to set mode to internal automatic power control "
                "without being enabled in the UI. Ignoring command.");
        return;
    }

    if (_mode == HUAWEI_MODE_AUTO_INT && mode != HUAWEI_MODE_AUTO_INT) {
        _autoPowerEnabled = false;
        _setParameter(0, HardwareInterface::Setting::OnlineCurrent);
    }

    if (mode == HUAWEI_MODE_AUTO_EXT || mode == HUAWEI_MODE_AUTO_INT) {
        _mode = mode;
        // Update mode in datapoints for AUTO modes too
        _dataPoints.add<DataPointLabel::Mode>(_mode, true);
    }
}

void Provider::onMqttMessage(Topic enumTopic,
        const espMqttClientTypes::MessageProperties& properties,
        const char* topic, const uint8_t* payload, size_t len)
{
    std::string strValue(reinterpret_cast<const char*>(payload), len);
    float payload_val = -1;
    try {
        payload_val = std::stof(strValue);
    }
    catch (std::invalid_argument const& e) {
        DTU_LOGE("Huawei MQTT handler: cannot parse payload of topic '%s' as float: %s",
                topic, strValue.c_str());
        return;
    }

    using Setting = HardwareInterface::Setting;

    auto validateAndSetParameter = [this, payload_val](float min, float max,
            Setting setting, const char* paramName, const char* unit) -> bool {
        if (payload_val < min || payload_val > max) {
            DTU_LOGE("Invalid %s %.2f %s (valid range: %.2f-%.2f %s)",
                paramName, payload_val, unit, min, max, unit);
            return false;
        }
        DTU_LOGI("Limit %s: %.2f %s", paramName, payload_val, unit);
        setParameter(payload_val, setting);
        return true;
    };

    switch (enumTopic) {
        case Topic::LimitOnlineVoltage:
            validateAndSetParameter(MIN_ONLINE_VOLTAGE, MAX_ONLINE_VOLTAGE,
                Setting::OnlineVoltage, "online voltage", "V");
            break;

        case Topic::LimitOfflineVoltage:
            validateAndSetParameter(MIN_OFFLINE_VOLTAGE, MAX_OFFLINE_VOLTAGE,
                Setting::OfflineVoltage, "offline voltage", "V");
            break;

        case Topic::LimitOnlineCurrent:
            validateAndSetParameter(MIN_ONLINE_CURRENT, MAX_ONLINE_CURRENT,
                Setting::OnlineCurrent, "online current", "A");
            break;

        case Topic::LimitOfflineCurrent:
            validateAndSetParameter(MIN_OFFLINE_CURRENT, MAX_OFFLINE_CURRENT,
                Setting::OfflineCurrent, "offline current", "A");
            break;

        case Topic::Mode:
            switch (static_cast<int>(payload_val)) {
                case 3:
                    DTU_LOGI("Received MQTT msg. New mode: Full internal control");
                    setMode(HUAWEI_MODE_AUTO_INT);
                    break;

                case 2:
                    DTU_LOGI("Received MQTT msg. New mode: Internal on/off control, external power limit");
                    setMode(HUAWEI_MODE_AUTO_EXT);
                    break;

                case 1:
                    DTU_LOGI("Received MQTT msg. New mode: Turned ON");
                    setMode(HUAWEI_MODE_ON);
                    break;

                case 0:
                    DTU_LOGI("Received MQTT msg. New mode: Turned OFF");
                    setMode(HUAWEI_MODE_OFF);
                    break;

                default:
                    DTU_LOGE("Invalid mode %.0f", payload_val);
                    break;
            }
            break;

        case Topic::Production:
        {
            bool enable = payload_val > 0;
            DTU_LOGI("Production to be %sabled", (enable?"en":"dis"));
            setProduction(enable);
            break;
        }

        case Topic::LimitInputCurrent:
            validateAndSetParameter(MIN_INPUT_CURRENT_LIMIT, MAX_INPUT_CURRENT_LIMIT,
                Setting::InputCurrentLimit, "input current", "A");
            break;

        case Topic::FanOnlineFullSpeed:
        case Topic::FanOfflineFullSpeed:
        {
            bool online = (Topic::FanOnlineFullSpeed == enumTopic);
            bool fullSpeed = payload_val > 0;
            setFan(online, fullSpeed);
            break;
        }
    }
}

} // namespace GridChargers::Huawei
