// SPDX-License-Identifier: GPL-2.0-or-later

#include <gridcharger/trucki/Provider.h>
#include <gridcharger/trucki/DataPoints.h>
#include <gridcharger/Controller.h>
#include <battery/Controller.h>
#include <powermeter/Controller.h>
#include <PowerLimiter.h>
#include <Utils.h>
#include <WiFiUdp.h>
#include <LogHelper.h>
#include <algorithm>
#include <cmath>
#include <limits>

#undef TAG
static const char* TAG = "gridCharger";
static const char* SUBTAG = "Trucki";

static constexpr unsigned int udpPort = 4211;  // trucki T2xG port
static constexpr uint32_t AutoPowerTargetZeroHoldMillis = 10 * 1000;
static WiFiUDP TruckiUdp;

namespace GridChargers::Trucki {

namespace {

bool millisAtOrAfter(uint32_t timestamp, uint32_t reference)
{
    auto constexpr halfOfAllMillis = std::numeric_limits<uint32_t>::max() / 2;
    return (timestamp - reference) < halfOfAllMillis;
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
    DTU_LOGI("Initialize Trucki AC charger interface...");

    auto const& config = Configuration.get();
    auto const& ipAddress = IPAddress(config.GridCharger.Trucki.IpAddress);

    if (ipAddress.toString() == "0.0.0.0") {
        DTU_LOGE("Invalid IP address: %s", ipAddress.toString().c_str());
        return false;
    }

    if (!TruckiUdp.begin(udpPort)) {
        DTU_LOGE("Failed to initialize UDP");
        return false;
    }

    _httpRequestConfig = std::make_unique<HttpRequestConfig>();
    strlcpy(_httpRequestConfig->Url, ("http://" + ipAddress.toString() + "/jsonlive").c_str(), sizeof(_httpRequestConfig->Url));
    _httpRequestConfig->Timeout = HTTP_REQUEST_TIMEOUT_MS;
    strlcpy(_httpRequestConfig->HeaderKey, "", sizeof(_httpRequestConfig->HeaderKey));
    strlcpy(_httpRequestConfig->HeaderValue, "", sizeof(_httpRequestConfig->HeaderValue));
    strlcpy(_httpRequestConfig->Username, "admin", sizeof(_httpRequestConfig->Username)); // default username
    strlcpy(_httpRequestConfig->Password, config.GridCharger.Trucki.Password, sizeof(_httpRequestConfig->Password));

    if (strlen(_httpRequestConfig->Password) > 0) {
        _httpRequestConfig->AuthType = HttpRequestConfig::Auth::Basic;
    } else {
        _httpRequestConfig->AuthType = HttpRequestConfig::Auth::None;
    }

    DTU_LOGD("Request URL: %s", _httpRequestConfig->Url);

    _httpGetter = std::make_unique<HttpGetter>(*_httpRequestConfig);
    _httpGetter->addHeader("Accept", "*/*");

    if (!_httpGetter->init()) {
        DTU_LOGE("Initializing HTTP getter failed: %s", _httpGetter->getErrorText());
        _httpGetter = nullptr;
        return false;
    }

    return true;
}

int16_t Provider::getAutoPowerTargetPowerConsumption() const
{
    if (isAutoPowerTargetPowerConsumptionZeroHoldActive()) { return 0; }

    auto const target = std::round(Configuration.get().GridCharger.AutoPowerTargetPowerConsumption);
    return static_cast<int16_t>(std::clamp(
            target,
            static_cast<float>(std::numeric_limits<int16_t>::min()),
            static_cast<float>(std::numeric_limits<int16_t>::max())));
}

std::optional<uint32_t> Provider::getPowerLimiterOutputReferenceMillis() const
{
    if (_powerLimiterCommandMillis != 0) {
        return _powerLimiterCommandMillis + 4 * DATA_POLLING_INTERVAL_MS;
    }

    auto const statsMillis = _stats->getLastUpdate();
    if (statsMillis == 0 && getPowerLimiterCurrentInputPowerWatts() > 0) {
        return std::nullopt;
    }

    return statsMillis;
}

uint16_t Provider::getPowerLimiterCurrentInputPowerWatts() const
{
    return wattsToUint16(_dataCurrent.get<DataPointLabel::AcPower>().value_or(0.0f));
}

uint16_t Provider::getPowerLimiterExpectedInputPowerWatts() const
{
    return _oPowerLimiterTargetInputPowerWatts.value_or(getPowerLimiterCurrentInputPowerWatts());
}

float Provider::getPowerLimiterMaxInputPowerWattsFloat() const
{
    auto const& config = Configuration.get();
    if (!config.GridCharger.Enabled || !config.GridCharger.AutoPowerEnabled) {
        return 0.0f;
    }

    if (millis() - _dataCurrent.getLastUpdate() > 30 * 1000) {
        return 0.0f;
    }

    auto oMaxAcPower = _dataCurrent.get<DataPointLabel::MaxAcPower>();
    auto oOutputPower = _dataCurrent.get<DataPointLabel::DcPower>();
    auto oOutputVoltage = _dataCurrent.get<DataPointLabel::DcVoltage>();
    auto oOutputCurrent = _dataCurrent.get<DataPointLabel::DcCurrent>();
    if (!oMaxAcPower || !oOutputPower || !oOutputVoltage || !oOutputCurrent
            || *oOutputVoltage <= 0.0f) {
        return 0.0f;
    }

    auto stats = Battery.getStats();
    if (config.Battery.Enabled && config.GridCharger.AutoPowerBatterySoCLimitsEnabled) {
        auto const batterySoC = stats->getSoC();
        auto const stopBatterySoCThreshold = GridCharger.getAutoPowerStopBatterySoCThreshold();
        if (batterySoC >= stopBatterySoCThreshold) { return 0.0f; }
    }

    auto efficiency = _dataCurrent.get<DataPointLabel::Efficiency>().value_or(90.0f) / 100.0f;
    efficiency = efficiency > 0.5f ? efficiency : 0.9f;

    float maxInputPower = *oMaxAcPower;
    auto const plannedOutputPowerLimit = GridCharger.getAutoPowerSocPlanningOutputPowerLimit(*oOutputPower);
    if (plannedOutputPowerLimit) {
        maxInputPower = std::min(maxInputPower, *plannedOutputPowerLimit / efficiency);
    }

    if (config.Battery.Enabled && stats->isChargeCurrentLimitValid() && stats->isCurrentValid()) {
        float permissibleCurrent = stats->getChargeCurrentLimit()
            - (stats->getChargeCurrent() - *oOutputCurrent);
        permissibleCurrent = std::max(permissibleCurrent, 0.0f);
        maxInputPower = std::min(maxInputPower,
                (permissibleCurrent * *oOutputVoltage) / efficiency);
    }

    return std::max(0.0f, maxInputPower);
}

uint16_t Provider::getPowerLimiterMaxInputPowerWatts() const
{
    return wattsToUint16(getPowerLimiterMaxInputPowerWattsFloat());
}

uint16_t Provider::applyPowerLimiterInputPowerIncrease(uint16_t increase)
{
    if (increase == 0) { return 0; }

    auto const expected = getPowerLimiterExpectedInputPowerWatts();
    auto const maximum = getPowerLimiterMaxInputPowerWatts();
    if (maximum <= expected) { return 0; }

    auto const minInputPower = wattsToUint16(
            _dataCurrent.get<DataPointLabel::MinAcPower>().value_or(
                Configuration.get().GridCharger.AutoPowerLowerPowerLimit));
    if (expected == 0 && increase < minInputPower) { return 0; }

    auto const target = static_cast<uint16_t>(std::min<uint32_t>(
            static_cast<uint32_t>(expected) + increase,
            maximum));
    setRequestedPowerAc(target);
    _oPowerLimiterTargetInputPowerWatts = target;
    _powerLimiterCommandMillis = millis();
    _autoModeBlockedTillMillis = millis() + 4 * DATA_POLLING_INTERVAL_MS;
    _autoPowerEnabled = target > 0;
    return target > expected ? target - expected : 0;
}

uint16_t Provider::applyPowerLimiterInputPowerReduction(uint16_t reduction)
{
    if (reduction == 0) { return 0; }

    auto const expected = getPowerLimiterExpectedInputPowerWatts();
    if (expected == 0) { return 0; }

    uint16_t target = expected > reduction ? expected - reduction : 0;
    auto const minInputPower = wattsToUint16(
            _dataCurrent.get<DataPointLabel::MinAcPower>().value_or(
                Configuration.get().GridCharger.AutoPowerLowerPowerLimit));
    if (target > 0 && target < minInputPower) {
        target = 0;
    }

    setRequestedPowerAc(target);
    _oPowerLimiterTargetInputPowerWatts = target;
    _powerLimiterCommandMillis = millis();
    _autoModeBlockedTillMillis = millis() + 4 * DATA_POLLING_INTERVAL_MS;
    _autoPowerEnabled = target > 0;
    return expected - target;
}

void Provider::deinit()
{
    TruckiUdp.stop();

    _dataPollingTaskDone = false;

    std::unique_lock pollingLock(_dataPollingMutex);
    _stopPollingData = true;
    pollingLock.unlock();

    _dataPollingCv.notify_all();

    if (_dataPollingTaskHandle != nullptr) {
        while (!_dataPollingTaskDone) { delay(10); }
        _dataPollingTaskHandle = nullptr;
    }

    _httpGetter = nullptr;
    _httpRequestConfig = nullptr;
}

void Provider::loop()
{
    powerControlLoop();

    sendControlCommandRequest();
    parseControlCommandResponse();

    if (_dataPollingTaskHandle == nullptr) {
        std::unique_lock lock(_dataPollingMutex);
        _stopPollingData = false;
        lock.unlock();

        uint32_t constexpr stackSize = 6144;
        xTaskCreate(dataPollingLoopHelper, "TruckiPolling",
                stackSize, this, 1/*prio*/, &_dataPollingTaskHandle);
    }
}

void Provider::powerControlLoop()
{
    auto& config = Configuration.get();

    auto oMaxAcPower = _dataCurrent.get<DataPointLabel::MaxAcPower>();
    auto oOutputPower = _dataCurrent.get<DataPointLabel::DcPower>();

    // ***********************
    // Emergency charge
    // ***********************
    auto stats = Battery.getStats();
    if (!_batteryEmergencyCharging && config.GridCharger.EmergencyChargeEnabled && stats->getImmediateChargingRequest()) {
        _autoPowerLimitedByAvailablePower = false;
        if (!oMaxAcPower) {
            // TODO(andreasboehm): if this situation actually occurs, this message
            // will be printed with high frequency for a prolonged time. how can
            // we deal with that?
            DTU_LOGW("Cannot perform emergency charging with unknown PSU max ac power value");
            return;
        }

        _batteryEmergencyCharging = true;

        DTU_LOGI("Emergency Charge AC Power %.02f", *oMaxAcPower);
        setRequestedPowerAc(*oMaxAcPower);
        return;
    }

    if (_batteryEmergencyCharging && !stats->getImmediateChargingRequest()) {
        _autoPowerLimitedByAvailablePower = false;
        // Battery request has changed. Set current to 0, wait for PSU to respond and then clear state
        setRequestedPowerAc(0);
        if (oOutputPower && oOutputPower < 1) {
            _batteryEmergencyCharging = false;
        }
        return;
    }

    // ***********************
    // Automatic power control
    // ***********************
    if (config.GridCharger.AutoPowerEnabled) {
        // Check if we should run automatic power calculation at all.
        // We may have set a value recently and still wait for output stabilization
        if (_autoModeBlockedTillMillis > millis()) {
            return;
        }

        bool const powerLimiterManaged = PowerLimiter.isGridChargerManaged();
        if (!powerLimiterManaged) {
            _oPowerLimiterTargetInputPowerWatts = std::nullopt;
        }

        if (!powerLimiterManaged && PowerLimiter.isGovernedBatteryPoweredInverterProducing()) {
            _autoPowerLimitedByAvailablePower = false;
            setRequestedPowerAc(0);
            _autoPowerEnabled = false;
            DTU_LOGI("Inverter is active, disable PSU");
            _autoModeBlockedTillMillis = millis() + 1000;
            return;
        }

        if (millis() - _dataCurrent.getLastUpdate() > 30 * 1000) {
            _autoPowerLimitedByAvailablePower = false;
            DTU_LOGW("Cannot perform auto power control when critical PSU values are outdated");
            _autoModeBlockedTillMillis = millis() + 1000;
            return;
        }

        if (powerLimiterManaged) {
            return;
        }

        auto oOutputVoltage = _dataCurrent.get<DataPointLabel::DcVoltage>();
        auto oBatteryVoltageLimit = _dataCurrent.get<DataPointLabel::DcVoltageSetpoint>();
        auto oMinAcPower = _dataCurrent.get<DataPointLabel::MinAcPower>();
        auto oOutputCurrent = _dataCurrent.get<DataPointLabel::DcCurrent>();

        if (!oOutputPower || !oOutputVoltage || !oBatteryVoltageLimit || !oMinAcPower || !oMaxAcPower || !oOutputCurrent) {
            _autoPowerLimitedByAvailablePower = false;
            DTU_LOGW("Cannot perform auto power control while critical PSU values are still unknown");
            _autoModeBlockedTillMillis = millis() + 1000;
            return;
        }

        // Re-enable automatic power control if the output voltage has dropped below threshold
        if (oOutputVoltage && *oOutputVoltage < oBatteryVoltageLimit) {
            _autoPowerEnabled = true;
        }

        // We have received a new PowerMeter value. Also we're _autoPowerEnabled
        // So we're good to calculate a new limit
        if (PowerMeter.getLastUpdate() > _lastPowerMeterUpdateReceivedMillis && _autoPowerEnabled) {
            _lastPowerMeterUpdateReceivedMillis = PowerMeter.getLastUpdate();

            float powerTotal = round(PowerMeter.getPowerTotal());

            // Calculate new power limit
            float newPowerLimit = -1 * powerTotal;

            // Powerlimit is the current output power + permissable Grid consumption
            auto const targetPowerConsumption = getAutoPowerTargetPowerConsumption();
            newPowerLimit += *oOutputPower + targetPowerConsumption;

            DTU_LOGV("powerTotal: %.0f, outputPower: %.01f, targetPowerConsumption: %d, newPowerLimit: %.0f",
                    powerTotal, *oOutputPower, targetPowerConsumption, newPowerLimit);

            bool stopRequestedByBatterySoC = false;

            // Check whether the battery SoC limit setting is enabled
            if (config.Battery.Enabled && config.GridCharger.AutoPowerBatterySoCLimitsEnabled) {
                auto const batterySoC = Battery.getStats()->getSoC();
                auto const stopBatterySoCThreshold = GridCharger.getAutoPowerStopBatterySoCThreshold();
                // Sets power limit to 0 if the BMS reported SoC reaches or exceeds the user configured value
                if (batterySoC >= stopBatterySoCThreshold) {
                    stopRequestedByBatterySoC = true;
                    _autoPowerLimitedByAvailablePower = false;
                    newPowerLimit = 0;
                    DTU_LOGV("Current battery SoC %.1f reached stop threshold %i, set newPowerLimit to %f",
                            batterySoC, stopBatterySoCThreshold, newPowerLimit);
                }
            }

            auto efficiency = _dataCurrent.get<DataPointLabel::Efficiency>().value_or(90) / 100.0f;
            efficiency = efficiency > 0.5f ? efficiency : 0.9f;

            auto const plannedOutputPowerLimit = GridCharger.getAutoPowerSocPlanningOutputPowerLimit(*oOutputPower);
            if (plannedOutputPowerLimit) {
                newPowerLimit = std::min(newPowerLimit, *plannedOutputPowerLimit / efficiency);
            }
            auto autoPowerLimit = *oMaxAcPower;
            if (plannedOutputPowerLimit) {
                autoPowerLimit = std::min(autoPowerLimit, *plannedOutputPowerLimit / efficiency);
            }
            _autoPowerLimitedByAvailablePower = !stopRequestedByBatterySoC
                && newPowerLimit < autoPowerLimit;

            if (newPowerLimit >= *oMinAcPower) {
                // Limit power to maximum
                if (newPowerLimit > *oMaxAcPower) {
                    newPowerLimit = *oMaxAcPower;
                }

                // Calculate output current
                float calculatedCurrent = efficiency * (newPowerLimit / *oOutputVoltage);

                // Limit output current to value requested by BMS
                float permissibleCurrent = stats->getChargeCurrentLimit() - (stats->getChargeCurrent() - *oOutputCurrent); // BMS current limit - current from other sources, e.g. Victron MPPT charger
                float outputCurrent = std::min(calculatedCurrent, permissibleCurrent);
                outputCurrent = outputCurrent > 0 ? outputCurrent : 0;
                if (outputCurrent < calculatedCurrent - 0.1f) {
                    _autoPowerLimitedByAvailablePower = false;
                }

                // calculate new power limit based on output current
                newPowerLimit = (outputCurrent * *oOutputVoltage) / efficiency;

                _autoPowerEnabled = true;
                setRequestedPowerAc(newPowerLimit);

                // Don't run auto mode some time to allow for output stabilization after issuing a new value
                _autoModeBlockedTillMillis = millis() + 4 * DATA_POLLING_INTERVAL_MS;
            } else {
                // requested PL is below minium. Set power to 0
                _autoPowerEnabled = false;
                setRequestedPowerAc(0);
            }
        }
    } else {
        _autoPowerLimitedByAvailablePower = false;
    }
}

void Provider::setRequestedPowerAc(float power)
{
    if (_requestedPowerAc <= 0.0f && power > 0.0f) {
        holdAutoPowerTargetPowerConsumptionAtZero();
    }

    _requestedPowerAc = power;
}

void Provider::holdAutoPowerTargetPowerConsumptionAtZero()
{
    if (PowerLimiter.isGridChargerManaged()) {
        _autoPowerTargetPowerConsumptionZeroHoldTillMillis = 0;
        return;
    }

    bool const wasActive = isAutoPowerTargetPowerConsumptionZeroHoldActive();
    _autoPowerTargetPowerConsumptionZeroHoldTillMillis = millis() + AutoPowerTargetZeroHoldMillis;

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

void Provider::sendControlCommandRequest()
{
    auto& config = Configuration.get();

    if (!config.GridCharger.AutoPowerEnabled && !config.GridCharger.EmergencyChargeEnabled) {
        return;
    }

    if (millis() - _lastControlCommandRequestMillis < CONTROL_COMMAND_INTERVAL_MS) { return; }

    DTU_LOGV("Setting charging power to %.02fW AC", _requestedPowerAc);

    uint16_t acPowerSetpoint = _requestedPowerAc * 10; // ac power in W*10

    TruckiUdp.beginPacket(config.GridCharger.Trucki.IpAddress, udpPort);
    TruckiUdp.print(String(acPowerSetpoint));
    TruckiUdp.endPacket();

    _lastControlCommandRequestMillis = millis();
}

void Provider::parseControlCommandResponse()
{
    int packetSize = TruckiUdp.parsePacket();
    if (0 == packetSize) { return; }

    std::vector<char> buffer(packetSize + 1, '\0');
    int readBytes = TruckiUdp.read(buffer.data(), packetSize);
    if (readBytes <= 0) { return; }
    buffer[readBytes] = '\0'; // ensure null-terminated string

    DTU_LOGD("received %d bytes - %s", packetSize, buffer.data());

    // Parse packet format: "current_power;max_power;battery_state", e.g. "1000;1000;1"
    // - first number is the current AC power in W*10
    // - second number is the max AC power in W*10
    // - third number is the battery state and optional, not available in older versions
    float acPowerCurrent;
    float acPowerMax;
    int batteryState = -1;
    int parsedFields = sscanf(buffer.data(), "%f;%f;%d", &acPowerCurrent, &acPowerMax, &batteryState);
    if (parsedFields >= 2) {
        acPowerCurrent /= 10.0f; // Convert from W*10 to W
        acPowerMax /= 10.0f; // Convert from W*10 to W
    } else {
        DTU_LOGW("Invalid packet format: %s", buffer.data());
        return;
    }

    DTU_LOGV("acPowerCurrent: %f, acPowerMax: %f, batteryState: %d", acPowerCurrent, acPowerMax, batteryState);

    // Update data points
    {
        auto scopedLock = _dataCurrent.lock();
        _dataCurrent.add<DataPointLabel::AcPower>(acPowerCurrent);
        _dataCurrent.add<DataPointLabel::MaxAcPower>(acPowerMax);

        // only use batteryState when it could be parsed from the packet
        if (parsedFields == 3) {
            switch (batteryState) {
                case 5:
                    _dataCurrent.add<DataPointLabel::BatteryGridState>(std::string("trucki.VGRID_LOW"));
                    break;

                case 4:
                    _dataCurrent.add<DataPointLabel::BatteryGridState>(std::string("trucki.VGRID_LOW_DELAYED"));
                    break;

                case 3:
                    _dataCurrent.add<DataPointLabel::BatteryGridState>(std::string("trucki.VBAT_FULL_DELAYED"));
                    break;

                case 2:
                    _dataCurrent.add<DataPointLabel::BatteryGridState>(std::string("trucki.VBAT_FULL"));
                    break;

                case 1:
                    _dataCurrent.add<DataPointLabel::BatteryGridState>(std::string("trucki.VBAT_NORMAL"));
                    break;

                case 0:
                    _dataCurrent.add<DataPointLabel::BatteryGridState>(std::string("trucki.VBAT_LOW"));
                    break;

                default:
                    _dataCurrent.add<DataPointLabel::BatteryGridState>(std::string("trucki.UNKNOWN"));
                    break;
            }
        }
    }

    _stats->updateFrom(_dataCurrent);
}

void Provider::dataPollingLoopHelper(void* context)
{
    auto pInstance = static_cast<Provider*>(context);
    pInstance->dataPollingLoop();
    pInstance->_dataPollingTaskDone = true;
    vTaskDelete(nullptr);
}

void Provider::dataPollingLoop()
{
    std::unique_lock lock(_dataPollingMutex);

    while (!_stopPollingData) {
        auto elapsedMillis = millis() - _lastDataPoll;

        if (_lastDataPoll > 0 && elapsedMillis < DATA_POLLING_INTERVAL_MS) {
            auto sleepMs = DATA_POLLING_INTERVAL_MS - elapsedMillis;
            _dataPollingCv.wait_for(lock, std::chrono::milliseconds(sleepMs),
                    [this] { return _stopPollingData; }); // releases the mutex
            continue;
        }

        _lastDataPoll = millis();

        lock.unlock(); // polling can take quite some time
        pollData();
        lock.lock();
    }
}

void Provider::pollData()
{
    if (!_httpGetter) {
        DTU_LOGE("HTTP getter not initialized");
        return;
    }

    auto result = _httpGetter->performGetRequest();

    if (!result) {
        DTU_LOGE("Failed to get data from Trucki: %s", _httpGetter->getErrorText());
        return;
    }

    auto pStream = result.getStream();
    if (!pStream) {
        DTU_LOGE("Programmer error: HTTP request yields no stream");
        return;
    }

    JsonDocument data;
    const DeserializationError error = deserializeJson(data, *pStream);
    if (error) {
        DTU_LOGE("Unable to parse server response as JSON: %s", error.c_str());
        return;
    }

    {
        auto scopedLock = _dataCurrent.lock();

        addStringToDataPoints<DataPointLabel::ZEPC>(data, "ZEPCPOWER");
        addStringToDataPoints<DataPointLabel::State>(data, "MWPCSTATE");
        addFloatToDataPoints<DataPointLabel::Temperature>(data, "TEMP");
        addFloatToDataPoints<DataPointLabel::Efficiency>(data, "MWEFFICIENCY");
        addFloatToDataPoints<DataPointLabel::DayEnergy>(data, "DAYENERGY");
        addFloatToDataPoints<DataPointLabel::TotalEnergy>(data, "TOTALENERGY");

        addFloatToDataPoints<DataPointLabel::AcVoltage>(data, "VGRID");
        addFloatToDataPoints<DataPointLabel::AcPowerSetpoint>(data, "SETACPOWER");
        addFloatToDataPoints<DataPointLabel::AcPower>(data, "MQTT_ACDISPLAY_VALUE");

        addFloatToDataPoints<DataPointLabel::DcVoltage>(data, "VBAT");
        addFloatToDataPoints<DataPointLabel::DcVoltageSetpoint>(data, "VOUTSET");
        addFloatToDataPoints<DataPointLabel::DcPower>(data, "DCPOWER");
        addFloatToDataPoints<DataPointLabel::DcCurrent>(data, "IOUT");

        addFloatToDataPoints<DataPointLabel::DcVoltageOffline>(data, "VOUTOFFLINE");
        addFloatToDataPoints<DataPointLabel::DcCurrentOffline>(data, "IOUTOFFLINE");
        addFloatToDataPoints<DataPointLabel::MinAcPower>(data, "MQTT_MINPOWER_VALUE");
        addFloatToDataPoints<DataPointLabel::MaxAcPower>(data, "POWERLIMIT");
    }

    _stats->updateFrom(_dataCurrent);
}
} // namespace GridChargers::Trucki
