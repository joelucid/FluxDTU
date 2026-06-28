// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2026 Thomas Basler and others
 */
#include "HM_Abstract.h"
#include "HoymilesRadio.h"
#include "commands/ActivePowerControlCommand.h"
#include "commands/AlarmDataCommand.h"
#include "commands/DevInfoAllCommand.h"
#include "commands/DevInfoSimpleCommand.h"
#include "commands/GridOnProFilePara.h"
#include "commands/PowerControlCommand.h"
#include "commands/RealTimeRunDataCommand.h"
#include "commands/SystemConfigParaCommand.h"

HM_Abstract::HM_Abstract(HoymilesRadio* radio, const uint64_t serial)
    : InverterAbstract(radio, serial)
{
}

bool HM_Abstract::sendStatsRequest()
{
    if (!getEnablePolling()) {
        return false;
    }

    time_t now;
    time(&now);

    auto cmd = _radio->prepareCommand<RealTimeRunDataCommand>(this);
    cmd->setTime(now);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::sendAlarmLogRequest(const bool force)
{
    if (!getEnablePolling()) {
        return false;
    }

    if (!force) {
        if (Statistics()->hasChannelFieldValue(TYPE_INV, CH0, FLD_EVT_LOG)) {
            if (static_cast<uint8_t>(Statistics()->getChannelFieldValue(TYPE_INV, CH0, FLD_EVT_LOG) == _lastAlarmLogCnt)) {
                return false;
            }
        }
    }

    _lastAlarmLogCnt = static_cast<uint8_t>(Statistics()->getChannelFieldValue(TYPE_INV, CH0, FLD_EVT_LOG));

    time_t now;
    time(&now);

    auto cmd = _radio->prepareCommand<AlarmDataCommand>(this);
    cmd->setTime(now);
    EventLog()->setLastAlarmRequestSuccess(CMD_PENDING);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::sendDevInfoRequest()
{
    if (!getEnablePolling()) {
        return false;
    }

    time_t now;
    time(&now);

    auto cmdAll = _radio->prepareCommand<DevInfoAllCommand>(this);
    cmdAll->setTime(now);
    _radio->enqueCommand(cmdAll);

    auto cmdSimple = _radio->prepareCommand<DevInfoSimpleCommand>(this);
    cmdSimple->setTime(now);
    _radio->enqueCommand(cmdSimple);

    return true;
}

bool HM_Abstract::sendSystemConfigParaRequest()
{
    if (!getEnablePolling()) {
        return false;
    }

    time_t now;
    time(&now);

    auto cmd = _radio->prepareCommand<SystemConfigParaCommand>(this);
    cmd->setTime(now);
    SystemConfigPara()->setLastLimitRequestSuccess(CMD_PENDING);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::sendActivePowerControlRequest(float limit, const PowerLimitControlType type)
{
    bool const suppressRequestHistory = consumeSuppressNextRequestHistory();
    if (!getEnableCommands()) {
        return false;
    }

    if (CMD_PENDING == SystemConfigPara()->getLastLimitCommandSuccess()) {
        return false;
    }

    if (type == PowerLimitControlType::RelativNonPersistent || type == PowerLimitControlType::RelativPersistent) {
        limit = min<float>(100, limit);
    }

    _activePowerControlLimit = limit;
    _activePowerControlType = type;
    _activePowerControlSuppressRequestHistory = suppressRequestHistory;

    auto cmd = _radio->prepareCommand<ActivePowerControlCommand>(this);
    cmd->setActivePowerLimit(limit, type);
    cmd->setSuppressRequestHistory(_activePowerControlSuppressRequestHistory);
    SystemConfigPara()->setLastLimitCommandSuccess(CMD_PENDING);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::resendActivePowerControlRequest()
{
    if (_activePowerControlSuppressRequestHistory) {
        suppressNextRequestHistory();
    }
    return sendActivePowerControlRequest(_activePowerControlLimit, _activePowerControlType);
}

bool HM_Abstract::sendPowerControlRequest(const bool turnOn)
{
    return sendPowerControlRequest(turnOn, false);
}

bool HM_Abstract::sendPowerControlRequestCompleteOnTx(const bool turnOn)
{
    return sendPowerControlRequest(turnOn, true);
}

bool HM_Abstract::sendPowerControlRequest(const bool turnOn, bool completeOnTxSuccess)
{
    bool const suppressRequestHistory = consumeSuppressNextRequestHistory();
    if (!getEnableCommands()) {
        return false;
    }

    if (CMD_PENDING == PowerCommand()->getLastPowerCommandSuccess()) {
        return false;
    }

    if (turnOn) {
        _powerState = 1;
    } else {
        _powerState = 0;
    }
    _powerControlSuppressRequestHistory = suppressRequestHistory;

    auto cmd = _radio->prepareCommand<PowerControlCommand>(this);
    cmd->setPowerOn(turnOn);
    cmd->setCompleteOnTxSuccess(completeOnTxSuccess);
    cmd->setSuppressRequestHistory(_powerControlSuppressRequestHistory);
    PowerCommand()->setLastPowerCommandSuccess(CMD_PENDING);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::sendRestartControlRequest()
{
    bool const suppressRequestHistory = consumeSuppressNextRequestHistory();
    if (!getEnableCommands()) {
        return false;
    }

    _powerState = 2;
    _powerControlSuppressRequestHistory = suppressRequestHistory;

    auto cmd = _radio->prepareCommand<PowerControlCommand>(this);
    cmd->setRestart();
    cmd->setSuppressRequestHistory(_powerControlSuppressRequestHistory);
    PowerCommand()->setLastPowerCommandSuccess(CMD_PENDING);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::resendPowerControlRequest()
{
    switch (_powerState) {
    case 0:
        if (_powerControlSuppressRequestHistory) {
            suppressNextRequestHistory();
        }
        return sendPowerControlRequest(false);
        break;
    case 1:
        if (_powerControlSuppressRequestHistory) {
            suppressNextRequestHistory();
        }
        return sendPowerControlRequest(true);
        break;
    case 2:
        if (_powerControlSuppressRequestHistory) {
            suppressNextRequestHistory();
        }
        return sendRestartControlRequest();
        break;

    default:
        return false;
        break;
    }
}

bool HM_Abstract::sendGridOnProFileParaRequest()
{
    if (!getEnablePolling()) {
        return false;
    }

    time_t now;
    time(&now);

    auto cmd = _radio->prepareCommand<GridOnProFilePara>(this);
    cmd->setTime(now);
    _radio->enqueCommand(cmd);

    return true;
}

bool HM_Abstract::supportsPowerDistributionLogic()
{
    return false;
}
