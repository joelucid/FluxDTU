// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023-2026 Thomas Basler and others
 */
#include "HoymilesRadio.h"
#include "crc.h"
#include "Hoymiles.h"
#include <algorithm>
#include <esp_log.h>
#include <cstdio>
#include <cstring>

#undef TAG
static const char* TAG = "hoymiles";

serial_u HoymilesRadio::DtuSerial() const
{
    return _dtuSerial;
}

void HoymilesRadio::setDtuSerial(const uint64_t serial)
{
    _dtuSerial.u64 = serial;
}

serial_u HoymilesRadio::convertSerialToRadioId(const serial_u serial)
{
    serial_u radioId;
    radioId.u64 = 0;
    radioId.b[4] = serial.b[0];
    radioId.b[3] = serial.b[1];
    radioId.b[2] = serial.b[2];
    radioId.b[1] = serial.b[3];
    radioId.b[0] = 0x01;
    return radioId;
}

bool HoymilesRadio::checkFragmentCrc(const fragment_t& fragment) const
{
    const uint8_t crc = crc8(fragment.fragment, fragment.len - 1);
    return (crc == fragment.fragment[fragment.len - 1]);
}

HoymilesRadio::RequestHistoryRecord* HoymilesRadio::findRequestHistoryRecord(uint32_t seq)
{
    if (seq == 0) {
        return nullptr;
    }

    for (size_t i = 0; i < _requestHistoryCount; ++i) {
        auto const index =
            (_requestHistoryWrite + RequestHistoryCapacity - _requestHistoryCount + i)
            % RequestHistoryCapacity;
        if (_requestHistory[index].seq == seq) {
            return &_requestHistory[index];
        }
    }

    return nullptr;
}

void HoymilesRadio::recordQueuedRequestHistory(CommandAbstract& cmd, bool retransmit)
{
    if (cmd.suppressRequestHistory()) {
        return;
    }

    auto const commandName = cmd.getCommandName();
    RequestHistoryRecord record;
    record.queuedMillis = millis();
    record.updatedMillis = record.queuedMillis;
    record.serial = cmd.getTargetAddress();
    snprintf(record.command, sizeof(record.command), "%s", commandName.c_str());
    record.retransmit = retransmit;
    record.state = RequestHistoryState::Queued;

    std::lock_guard<std::mutex> lock(_requestHistoryMutex);
    record.seq = _requestHistorySeq++;
    if (_requestHistorySeq == 0) {
        _requestHistorySeq = 1;
    }
    cmd.setRequestHistorySeq(record.seq);
    _requestHistory[_requestHistoryWrite] = record;
    _requestHistoryWrite = (_requestHistoryWrite + 1) % RequestHistoryCapacity;
    _requestHistoryCount = std::min(_requestHistoryCount + 1, RequestHistoryCapacity);
}

void HoymilesRadio::markRequestHistoryInProcess(CommandAbstract& cmd)
{
    if (cmd.suppressRequestHistory()) {
        return;
    }
    if (cmd.getRequestHistorySeq() == 0) {
        recordQueuedRequestHistory(cmd);
    }

    auto const now = millis();
    std::lock_guard<std::mutex> lock(_requestHistoryMutex);
    auto* record = findRequestHistoryRecord(cmd.getRequestHistorySeq());
    if (record == nullptr) {
        return;
    }
    if (record->sentMillis == 0) {
        record->sentMillis = now;
    }
    record->updatedMillis = now;
    record->state = RequestHistoryState::InProcess;
}

void HoymilesRadio::markRequestHistoryFinished(CommandAbstract& cmd, bool success)
{
    if (cmd.suppressRequestHistory()) {
        return;
    }

    auto const now = millis();
    std::lock_guard<std::mutex> lock(_requestHistoryMutex);
    auto* record = findRequestHistoryRecord(cmd.getRequestHistorySeq());
    if (record == nullptr) {
        return;
    }
    if (record->sentMillis == 0) {
        record->sentMillis = now;
    }
    record->updatedMillis = now;
    record->state = success ? RequestHistoryState::Success : RequestHistoryState::Failed;
}

void HoymilesRadio::markRequestHistoryRemoved(std::shared_ptr<CommandAbstract> const& cmd)
{
    if (cmd == nullptr) {
        return;
    }

    markRequestHistoryFinished(*cmd, false);
}

void HoymilesRadio::markRequestHistoryRemoved(std::vector<std::shared_ptr<CommandAbstract>> const& commands)
{
    for (auto const& cmd : commands) {
        markRequestHistoryRemoved(cmd);
    }
}

void HoymilesRadio::sendRetransmitPacket(const uint8_t fragment_id)
{
    CommandAbstract* cmd = _commandQueue.front().get();

    CommandAbstract* requestCmd = cmd->getRequestFrameCommand(fragment_id);

    if (requestCmd != nullptr) {
        sendEsbPacket(*requestCmd);
    }
}

void HoymilesRadio::sendLastPacketAgain()
{
    CommandAbstract* cmd = _commandQueue.front().get();
    sendEsbPacket(*cmd);
}

void HoymilesRadio::handleReceivedPackage()
{
    if (_busyFlag && _rxTimeout.occured()) {
        ESP_LOGI(TAG, "RX Period End");
        std::shared_ptr<InverterAbstract> inv = Hoymiles.getInverterBySerial(_commandQueue.front().get()->getTargetAddress());

        if (nullptr != inv) {
            CommandAbstract* cmd = _commandQueue.front().get();
            uint8_t verifyResult = inv->verifyAllFragments(*cmd);
            if (verifyResult == FRAGMENT_ALL_MISSING_RESEND) {
                ESP_LOGW(TAG, "Nothing received, resend whole request");
                sendLastPacketAgain();

            } else if (verifyResult == FRAGMENT_ALL_MISSING_TIMEOUT) {
                ESP_LOGW(TAG, "Nothing received, resend count exeeded");
                // Statistics: Count RX Fail No Answer
                if (inv->RadioStats.TxRequestData > 0) {
                    inv->RadioStats.RxFailNoAnswer++;
                }

                markRequestHistoryFinished(*cmd, false);
                _commandQueue.pop();
                _busyFlag = false;

            } else if (verifyResult == FRAGMENT_RETRANSMIT_TIMEOUT) {
                ESP_LOGW(TAG, "Retransmit timeout");
                // Statistics: Count RX Fail Partial Answer
                if (inv->RadioStats.TxRequestData > 0) {
                    inv->RadioStats.RxFailPartialAnswer++;
                }

                markRequestHistoryFinished(*cmd, false);
                _commandQueue.pop();
                _busyFlag = false;

            } else if (verifyResult == FRAGMENT_HANDLE_ERROR) {
                ESP_LOGW(TAG, "Packet handling error");
                // Statistics: Count RX Fail Corrupt Data
                if (inv->RadioStats.TxRequestData > 0) {
                    inv->RadioStats.RxFailCorruptData++;
                }

                markRequestHistoryFinished(*cmd, false);
                _commandQueue.pop();
                _busyFlag = false;

            } else if (verifyResult > 0) {
                // Perform Retransmit
                ESP_LOGI(TAG, "Request retransmit: %" PRIu8 "", verifyResult);
                // Statistics: Count TX Re-Request Fragment
                inv->RadioStats.TxReRequestFragment++;

                sendRetransmitPacket(verifyResult);

            } else {
                // Successful received all packages
                ESP_LOGI(TAG, "Success");
                // Statistics: Count RX Success
                if (inv->RadioStats.TxRequestData > 0) {
                    inv->RadioStats.RxSuccess++;
                }

                markRequestHistoryFinished(*cmd, true);
                _commandQueue.pop();
                _busyFlag = false;
            }
        } else {
            // If inverter was not found, assume the command is invalid
            ESP_LOGW(TAG, "RX: Invalid inverter found");
            // Statistics: Count RX Fail Unknown Data
            CommandAbstract* cmd = _commandQueue.front().get();
            markRequestHistoryFinished(*cmd, false);
            _commandQueue.pop();
            _busyFlag = false;
        }
    } else if (!_busyFlag) {
        // Currently in idle mode --> send packet if one is in the queue
        if (!isQueueEmpty()) {
            CommandAbstract* cmd = _commandQueue.front().get();

            auto inv = Hoymiles.getInverterBySerial(cmd->getTargetAddress());
            if (nullptr != inv) {
                inv->clearRxFragmentBuffer();
                // Statistics: TX Requests
                inv->RadioStats.TxRequestData++;

                markRequestHistoryInProcess(*cmd);
                bool txOk = sendEsbPacket(*cmd);
                if (txSuccessCompletesCommand() && cmd->isCompleteOnTxSuccess()) {
                    bool const summarizeTxAutoAckFailures = cmd->getMaxResendCount() > 0;
                    while (!txOk && cmd->getSendCount() <= cmd->getMaxResendCount()) {
                        if (summarizeTxAutoAckFailures) {
                            ESP_LOGD(TAG, "TX failed, resend %s", cmd->getCommandName().c_str());
                        } else {
                            ESP_LOGW(TAG, "TX failed, resend %s", cmd->getCommandName().c_str());
                        }
                        txOk = sendEsbPacket(*cmd);
                    }
                    if (summarizeTxAutoAckFailures) {
                        uint8_t const attempts = cmd->getSendCount();
                        uint8_t const failedAttempts = txOk ? attempts - 1 : attempts;
                        if (failedAttempts > 0) {
                            if (txOk) {
                                ESP_LOGW(TAG, "TX %s auto ACK summary: succeeded after %" PRIu8
                                               " attempts; %" PRIu8 " attempts missed auto ACK",
                                    cmd->getCommandName().c_str(), attempts, failedAttempts);
                            } else {
                                ESP_LOGW(TAG, "TX %s auto ACK summary: %" PRIu8
                                               " attempts all missed auto ACK; command handler will decide final state",
                                    cmd->getCommandName().c_str(), attempts);
                            }
                        }
                    }
                    cmd->handleTxResult(txOk);
                    markRequestHistoryFinished(*cmd, txOk);
                    _commandQueue.pop();
                    _busyFlag = false;
                }
            } else {
                ESP_LOGE(TAG, "TX: Invalid inverter found");
                markRequestHistoryFinished(*cmd, false);
                _commandQueue.pop();
            }
        }
    }
}

bool HoymilesRadio::isInitialized() const
{
    return _isInitialized;
}

void HoymilesRadio::removeCommands(InverterAbstract* inv)
{
    auto const removed = _commandQueue.removeAllEntriesForInverter(inv);
    markRequestHistoryRemoved(removed.commands);
    if (removed.removedFront) {
        _busyFlag = false;
    }
}

uint8_t HoymilesRadio::countSimilarCommands(std::shared_ptr<CommandAbstract> cmd)
{
    return _commandQueue.countSimilarCommands(cmd);
}

bool HoymilesRadio::isIdle() const
{
    return !_busyFlag;
}

bool HoymilesRadio::isQueueEmpty() const
{
    return _commandQueue.size() == 0;
}

uint32_t HoymilesRadio::getQueueSize() const
{
    return _commandQueue.size();
}

uint32_t HoymilesRadio::getQueueSizeForTarget(uint64_t targetAddress) const
{
    return _commandQueue.countCommandsForTarget(targetAddress);
}

void HoymilesRadio::getRequestHistory(std::vector<RequestHistoryRecord>& records) const
{
    std::lock_guard<std::mutex> lock(_requestHistoryMutex);
    records.reserve(records.size() + _requestHistoryCount);
    for (size_t i = 0; i < _requestHistoryCount; ++i) {
        auto const index =
            (_requestHistoryWrite + RequestHistoryCapacity - _requestHistoryCount + i)
            % RequestHistoryCapacity;
        records.push_back(_requestHistory[index]);
    }
}
