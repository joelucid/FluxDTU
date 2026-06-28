// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Arduino.h"
#include "commands/CommandAbstract.h"
#include "queue/CommandQueue.h"
#include "types.h"
#include <TimeoutHelper.h>
#include <array>
#include <cstddef>
#include <mutex>
#include <vector>

#ifdef HOY_DEBUG_QUEUE
#include <esp_log.h>

#undef TAG
static const char* TAG = "hoymiles";

#define DEBUG_PRINT(fmt, args...) ESP_LOGD(TAG, fmt, ##args)
#else
#define DEBUG_PRINT(fmt, args...) /* Don't do anything in release builds */
#endif

class HoymilesRadio {
public:
    enum class RequestHistoryState : uint8_t {
        Queued,
        InProcess,
        Success,
        Failed,
    };

    struct RequestHistoryRecord {
        uint32_t seq = 0;
        uint32_t queuedMillis = 0;
        uint32_t sentMillis = 0;
        uint32_t updatedMillis = 0;
        uint64_t serial = 0;
        char command[32] = {};
        bool retransmit = false;
        RequestHistoryState state = RequestHistoryState::Queued;
    };

    serial_u DtuSerial() const;
    virtual void setDtuSerial(const uint64_t serial);

    bool isIdle() const;
    bool isQueueEmpty() const;
    uint32_t getQueueSize() const;
    uint32_t getQueueSizeForTarget(uint64_t targetAddress) const;
    bool isInitialized() const;

    void removeCommands(InverterAbstract* inv);
    uint8_t countSimilarCommands(std::shared_ptr<CommandAbstract> cmd);
    void getRequestHistory(std::vector<RequestHistoryRecord>& records) const;

    void enqueCommand(std::shared_ptr<CommandAbstract> cmd)
    {
        DEBUG_PRINT("Queue size before: %ld", _commandQueue.size());
        DEBUG_PRINT("Handling command %s with type %d", cmd.get()->getCommandName().c_str(), static_cast<uint8_t>(cmd.get()->getQueueInsertType()));
        switch (cmd.get()->getQueueInsertType()) {
        case QueueInsertType::RemoveOldest:
            markRequestHistoryRemoved(_commandQueue.removeDuplicatedEntries(cmd, _busyFlag));
            break;
        case QueueInsertType::ReplaceExistent:
            // Checks if the queue already contains a command like the new one
            // and replaces the existing one with the new one.
            // (The new one will not be pushed at the end of the queue)
            if (_commandQueue.countSimilarCommands(cmd) > 0) {
                DEBUG_PRINT("    ... existing entry will be replaced");
                markRequestHistoryRemoved(_commandQueue.replaceEntries(cmd, _busyFlag));
                recordQueuedRequestHistory(*cmd);
                return;
            }
            break;
        case QueueInsertType::RemoveNewest:
            // Checks if the queue already contains a command like the new one
            // and drops the new one. The new one will not be inserted.
            if (_commandQueue.countSimilarCommands(cmd) > 0) {
                DEBUG_PRINT("    ... new entry will be dropped");
                return;
            }
            break;
        case QueueInsertType::AllowMultiple:
            // Dont do anything, just fall through and insert the command.
            break;
        }

        // Push the command into the queue if we reach this position of the code
        if (cmd->isHighPriority()) {
            DEBUG_PRINT("    ... new entry will be prioritized");
            _commandQueue.pushPriority(cmd, _busyFlag);
        } else {
            DEBUG_PRINT("    ... new entry will be appended");
            _commandQueue.push(cmd);
        }
        recordQueuedRequestHistory(*cmd);

        DEBUG_PRINT("Queue size after: %ld", _commandQueue.size());
    }

    template <typename T>
    std::shared_ptr<T> prepareCommand(InverterAbstract* inv)
    {
        return std::make_shared<T>(inv);
    }

protected:
    static constexpr size_t RequestHistoryCapacity = 256;

    static serial_u convertSerialToRadioId(const serial_u serial);

    bool checkFragmentCrc(const fragment_t& fragment) const;
    virtual bool txSuccessCompletesCommand() const { return false; }
    virtual bool sendEsbPacket(CommandAbstract& cmd) = 0;
    void recordQueuedRequestHistory(CommandAbstract& cmd, bool retransmit = false);
    void markRequestHistoryInProcess(CommandAbstract& cmd);
    void markRequestHistoryFinished(CommandAbstract& cmd, bool success);
    void markRequestHistoryRemoved(std::shared_ptr<CommandAbstract> const& cmd);
    void markRequestHistoryRemoved(std::vector<std::shared_ptr<CommandAbstract>> const& commands);
    void sendRetransmitPacket(const uint8_t fragment_id);
    void sendLastPacketAgain();
    void handleReceivedPackage();

    RequestHistoryRecord* findRequestHistoryRecord(uint32_t seq);

    serial_u _dtuSerial;
    CommandQueue _commandQueue;
    bool _isInitialized = false;
    bool _busyFlag = false;

    TimeoutHelper _rxTimeout;

    mutable std::mutex _requestHistoryMutex;
    std::array<RequestHistoryRecord, RequestHistoryCapacity> _requestHistory;
    size_t _requestHistoryWrite = 0;
    size_t _requestHistoryCount = 0;
    uint32_t _requestHistorySeq = 1;
};
