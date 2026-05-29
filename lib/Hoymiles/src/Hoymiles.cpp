// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2026 Thomas Basler and others
 */
#include "Hoymiles.h"
#include "Utils.h"
#include "inverters/HERF_1CH.h"
#include "inverters/HERF_2CH.h"
#include "inverters/HERF_4CH.h"
#ifndef HOYMILES_NRF_ONLY
#include "inverters/HMS_1CH.h"
#include "inverters/HMS_1CHv2.h"
#include "inverters/HMS_2CH.h"
#include "inverters/HMS_4CH.h"
#include "inverters/HMT_4CH.h"
#include "inverters/HMT_6CH.h"
#endif
#include "inverters/HM_1CH.h"
#include "inverters/HM_2CH.h"
#include "inverters/HM_4CH.h"
#include <Arduino.h>
#include <algorithm>
#include <esp_log.h>

#undef TAG
static const char* TAG = "hoymiles";

HoymilesClass Hoymiles;

void HoymilesClass::init()
{
    _pollInterval = 0;
    _radioNrf.reset(new HoymilesRadio_NRF());
#ifndef HOYMILES_NRF_ONLY
    _radioCmt.reset(new HoymilesRadio_CMT());
#endif
}

void HoymilesClass::initNRF(SPIClass* initialisedSpiBus, const uint8_t pinCE, const uint8_t pinIRQ)
{
    _radioNrf->init(initialisedSpiBus, pinCE, pinIRQ);
}

#ifndef HOYMILES_NRF_ONLY
void HoymilesClass::initCMT(const int8_t pin_sdio, const int8_t pin_clk, const int8_t pin_cs, const int8_t pin_fcs, const int8_t pin_gpio2, const int8_t pin_gpio3)
{
    _radioCmt->init(pin_sdio, pin_clk, pin_cs, pin_fcs, pin_gpio2, pin_gpio3);
}
#endif

void HoymilesClass::loop()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _radioNrf->loop();
#ifndef HOYMILES_NRF_ONLY
    _radioCmt->loop();
#endif

    if (getNumInverters() == 0) {
        return;
    }

    const uint32_t now = millis();
    const bool regularPollDue = now - _lastPoll > _pollInterval;
    const bool recoveryPollDue = _degradedInverterRecoveryEnabled
        && now - _lastDegradedInverterRecoveryPoll > getDegradedInverterRecoveryInterval();

    if (!regularPollDue && !recoveryPollDue) {
        return;
    }

    bool recoveryPoll = false;
    std::shared_ptr<InverterAbstract> iv = nullptr;

    if (regularPollDue) {
        iv = getNextInverterForPolling(_pollInverterPos, false);
    }
    if (iv == nullptr && recoveryPollDue) {
        iv = getNextInverterForPolling(_recoveryInverterPos, true);
        recoveryPoll = iv != nullptr;
    }

    if (iv != nullptr && pollInverter(iv)) {
        if (recoveryPoll) {
            ESP_LOGI(TAG, "Recovery fetch inverter: %s", iv->serialString().c_str());
            _lastDegradedInverterRecoveryPoll = now;
        } else {
            _lastPoll = now;
            if (isRecoveryCandidate(iv)) {
                _lastDegradedInverterRecoveryPoll = now;
            }
        }
    }

    // Perform housekeeping of all inverters on day change
    const int8_t currentWeekDay = Utils::getWeekDay();
    static int8_t lastWeekDay = -1;
    if (lastWeekDay == -1) {
        lastWeekDay = currentWeekDay;
    } else {
        if (currentWeekDay != lastWeekDay) {

            for (auto& inv : _inverters) {
                inv->performDailyTask();
            }

            lastWeekDay = currentWeekDay;
        }
    }
}

std::shared_ptr<InverterAbstract> HoymilesClass::getNextInverterForPolling(uint8_t& inverterPos, const bool requireRecoveryCandidate)
{
    const size_t count = getNumInverters();
    if (count == 0) {
        return nullptr;
    }

    for (size_t i = 0; i < count; i++) {
        std::shared_ptr<InverterAbstract> iv = getInverterByPos(inverterPos);
        if (++inverterPos >= count) {
            inverterPos = 0;
        }

        if (iv == nullptr || !iv->getRadio()->isInitialized()) {
            continue;
        }

        if (!(iv->getEnablePolling() || iv->getEnableCommands())) {
            continue;
        }

        if (requireRecoveryCandidate && !isRecoveryCandidate(iv)) {
            continue;
        }

        return iv;
    }

    return nullptr;
}

bool HoymilesClass::isRecoveryCandidate(std::shared_ptr<InverterAbstract> iv) const
{
    return iv != nullptr && iv->getEnablePolling() && iv->Statistics()->getRxFailureCount() > 0;
}

uint32_t HoymilesClass::getDegradedInverterRecoveryInterval() const
{
    return std::max<uint32_t>(_pollInterval, HOY_DEGRADED_INVERTER_RECOVERY_MIN_INTERVAL);
}

bool HoymilesClass::pollInverter(std::shared_ptr<InverterAbstract> iv)
{
    if (iv == nullptr || !iv->getRadio()->isInitialized()) {
        return false;
    }

    if (iv->getZeroValuesIfUnreachable() && !iv->isReachable()) {
        iv->Statistics()->zeroRuntimeData();
    }

    if (!(iv->getEnablePolling() || iv->getEnableCommands())) {
        return false;
    }

    ESP_LOGI(TAG, "Fetch inverter: %s", iv->serialString().c_str());

    if (!iv->isReachable()) {
        iv->sendChangeChannelRequest();
    }

    if (Utils::getTimeAvailable()) {
        // Fetch statistics
        iv->sendStatsRequest();

        // Fetch event log
        const bool force = iv->EventLog()->getLastAlarmRequestSuccess() == CMD_NOK;
        iv->sendAlarmLogRequest(force);

        // Fetch limit
        if (((millis() - iv->SystemConfigPara()->getLastUpdateRequest() > HOY_SYSTEM_CONFIG_PARA_POLL_INTERVAL)
                && (millis() - iv->SystemConfigPara()->getLastUpdateCommand() > HOY_SYSTEM_CONFIG_PARA_POLL_MIN_DURATION))) {
            ESP_LOGI(TAG, "Request SystemConfigPara");
            iv->sendSystemConfigParaRequest();
        }

        // Fetch grid profile
        if (iv->Statistics()->getLastUpdate() > 0 && (iv->GridProfile()->getLastUpdate() == 0 || !iv->GridProfile()->containsValidData())) {
            iv->sendGridOnProFileParaRequest();
        }

        // Fetch dev info (but first fetch stats)
        if (iv->Statistics()->getLastUpdate() > 0) {
            const bool invalidDevInfo = !iv->DevInfo()->containsValidData()
                && iv->DevInfo()->getLastUpdateAll() > 0
                && iv->DevInfo()->getLastUpdateSimple() > 0;

            if (invalidDevInfo) {
                ESP_LOGW(TAG, "DevInfo: No Valid Data");
            }

            if ((iv->DevInfo()->getLastUpdateAll() == 0)
                || (iv->DevInfo()->getLastUpdateSimple() == 0)
                || invalidDevInfo) {
                ESP_LOGI(TAG, "Request device info");
                iv->sendDevInfoRequest();
            }
        }
    }

    // Set limit if required
    if (iv->SystemConfigPara()->getLastLimitCommandSuccess() == CMD_NOK) {
        ESP_LOGI(TAG, "Resend ActivePowerControl");
        iv->resendActivePowerControlRequest();
    }

    // Set power status if required
    if (iv->PowerCommand()->getLastPowerCommandSuccess() == CMD_NOK) {
        ESP_LOGI(TAG, "Resend PowerCommand");
        iv->resendPowerControlRequest();
    }

#ifndef HOYMILES_NRF_ONLY
    ESP_LOGI(TAG, "Queue size - NRF: %" PRIu32 " CMT: %" PRIu32 "", _radioNrf->getQueueSize(), _radioCmt->getQueueSize());
#else
    ESP_LOGI(TAG, "Queue size - NRF: %" PRIu32 "", _radioNrf->getQueueSize());
#endif

    return true;
}

std::shared_ptr<InverterAbstract> HoymilesClass::addInverter(const char* name, const uint64_t serial)
{
    std::shared_ptr<InverterAbstract> i = nullptr;
#ifndef HOYMILES_NRF_ONLY
    if (HMT_4CH::isValidSerial(serial)) {
        i = std::make_shared<HMT_4CH>(_radioCmt.get(), serial);
    } else if (HMT_6CH::isValidSerial(serial)) {
        i = std::make_shared<HMT_6CH>(_radioCmt.get(), serial);
    } else if (HMS_4CH::isValidSerial(serial)) {
        i = std::make_shared<HMS_4CH>(_radioCmt.get(), serial);
    } else if (HMS_2CH::isValidSerial(serial)) {
        i = std::make_shared<HMS_2CH>(_radioCmt.get(), serial);
    } else if (HMS_1CH::isValidSerial(serial)) {
        i = std::make_shared<HMS_1CH>(_radioCmt.get(), serial);
    } else if (HMS_1CHv2::isValidSerial(serial)) {
        i = std::make_shared<HMS_1CHv2>(_radioCmt.get(), serial);
    }

    if (!i) {
#endif
        if (HM_4CH::isValidSerial(serial)) {
            i = std::make_shared<HM_4CH>(_radioNrf.get(), serial);
        } else if (HM_2CH::isValidSerial(serial)) {
            i = std::make_shared<HM_2CH>(_radioNrf.get(), serial);
        } else if (HM_1CH::isValidSerial(serial)) {
            i = std::make_shared<HM_1CH>(_radioNrf.get(), serial);
        } else if (HERF_1CH::isValidSerial(serial)) {
            i = std::make_shared<HERF_1CH>(_radioNrf.get(), serial);
        } else if (HERF_2CH::isValidSerial(serial)) {
            i = std::make_shared<HERF_2CH>(_radioNrf.get(), serial);
        } else if (HERF_4CH::isValidSerial(serial)) {
            i = std::make_shared<HERF_4CH>(_radioNrf.get(), serial);
        }
#ifndef HOYMILES_NRF_ONLY
    }
#endif

    if (i) {
        i->setName(name);
        i->init();
        _inverters.push_back(std::move(i));
        return _inverters.back();
    }

    return nullptr;
}

std::shared_ptr<InverterAbstract> HoymilesClass::getInverterByPos(const uint8_t pos)
{
    if (pos >= _inverters.size()) {
        return nullptr;
    } else {
        return _inverters[pos];
    }
}

std::shared_ptr<InverterAbstract> HoymilesClass::getInverterBySerial(const uint64_t serial)
{
    for (auto& inv : _inverters) {
        if (inv->serial() == serial) {
            return inv;
        }
    }
    return nullptr;
}

std::shared_ptr<InverterAbstract> HoymilesClass::getInverterByFragment(const fragment_t& fragment)
{
    if (fragment.len <= 4) {
        return nullptr;
    }

    for (auto& inv : _inverters) {
        serial_u p;
        p.u64 = inv->serial();

        if ((p.b[3] == fragment.fragment[1])
            && (p.b[2] == fragment.fragment[2])
            && (p.b[1] == fragment.fragment[3])
            && (p.b[0] == fragment.fragment[4])) {

            return inv;
        }
    }
    return nullptr;
}

void HoymilesClass::removeInverterBySerial(const uint64_t serial)
{
    for (uint8_t i = 0; i < _inverters.size(); i++) {
        if (_inverters[i]->serial() == serial) {
            std::lock_guard<std::mutex> lock(_mutex);
            _inverters[i]->getRadio()->removeCommands(_inverters[i].get());
            _inverters.erase(_inverters.begin() + i);
            return;
        }
    }
}

size_t HoymilesClass::getNumInverters() const
{
    return _inverters.size();
}

HoymilesRadio_NRF* HoymilesClass::getRadioNrf()
{
    return _radioNrf.get();
}

#ifndef HOYMILES_NRF_ONLY
HoymilesRadio_CMT* HoymilesClass::getRadioCmt()
{
    return _radioCmt.get();
}
#endif

bool HoymilesClass::isAllRadioIdle() const
{
    bool allIdle = _radioNrf.get()->isIdle();
#ifndef HOYMILES_NRF_ONLY
    allIdle = allIdle && _radioCmt.get()->isIdle();
#endif
    return allIdle;
}

uint32_t HoymilesClass::PollInterval() const
{
    return _pollInterval;
}

void HoymilesClass::setPollInterval(const uint32_t interval)
{
    _pollInterval = interval;
}

void HoymilesClass::setDegradedInverterRecoveryEnabled(const bool enabled)
{
    _degradedInverterRecoveryEnabled = enabled;
}
