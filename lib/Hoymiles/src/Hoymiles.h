// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "HoymilesRadio_NRF.h"
#include "inverters/InverterAbstract.h"
#include "types.h"
#include <Print.h>
#include <SPI.h>
#include <memory>
#include <vector>

#ifndef HOYMILES_NRF_ONLY
#include "HoymilesRadio_CMT.h"
#endif

#define HOY_SYSTEM_CONFIG_PARA_POLL_INTERVAL (2 * 60 * 1000) // 2 minutes
#define HOY_SYSTEM_CONFIG_PARA_POLL_MIN_DURATION (4 * 60 * 1000) // at least 4 minutes between sending limit command and read request. Otherwise eventlog entry
#define HOY_DEGRADED_INVERTER_RECOVERY_MIN_INTERVAL (5 * 1000) // avoid excessive RF load while recovering lagging inverters

class HoymilesClass {
public:
    void init();
    void initNRF(SPIClass* initialisedSpiBus, const uint8_t pinCE, const uint8_t pinIRQ);
#ifndef HOYMILES_NRF_ONLY
    void initCMT(const int8_t pin_sdio, const int8_t pin_clk, const int8_t pin_cs, const int8_t pin_fcs, const int8_t pin_gpio2, const int8_t pin_gpio3);
#endif
    void loop();

    std::shared_ptr<InverterAbstract> addInverter(const char* name, const uint64_t serial);
    std::shared_ptr<InverterAbstract> getInverterByPos(const uint8_t pos);
    std::shared_ptr<InverterAbstract> getInverterBySerial(const uint64_t serial);
    std::shared_ptr<InverterAbstract> getInverterByFragment(const fragment_t& fragment);
    void removeInverterBySerial(const uint64_t serial);
    size_t getNumInverters() const;

    HoymilesRadio_NRF* getRadioNrf();
#ifndef HOYMILES_NRF_ONLY
    HoymilesRadio_CMT* getRadioCmt();
#endif

    uint32_t PollInterval() const;
    void setPollInterval(const uint32_t interval);
    void setDegradedInverterRecoveryEnabled(const bool enabled);

    bool isAllRadioIdle() const;

private:
    std::shared_ptr<InverterAbstract> getNextInverterForPolling(uint8_t& inverterPos, const bool requireRecoveryCandidate);
    bool isRecoveryCandidate(std::shared_ptr<InverterAbstract> iv) const;
    uint32_t getDegradedInverterRecoveryInterval() const;
    bool pollInverter(std::shared_ptr<InverterAbstract> iv);

    std::vector<std::shared_ptr<InverterAbstract>> _inverters;
    std::unique_ptr<HoymilesRadio_NRF> _radioNrf;
#ifndef HOYMILES_NRF_ONLY
    std::unique_ptr<HoymilesRadio_CMT> _radioCmt;
#endif

    std::mutex _mutex;

    uint32_t _pollInterval = 0;
    uint32_t _lastPoll = 0;
    uint32_t _lastDegradedInverterRecoveryPoll = 0;
    uint8_t _pollInverterPos = 0;
    uint8_t _recoveryInverterPos = 0;
    bool _degradedInverterRecoveryEnabled = false;
};

extern HoymilesClass Hoymiles;
