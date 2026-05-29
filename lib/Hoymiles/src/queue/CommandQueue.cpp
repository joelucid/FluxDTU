// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024-2026 Thomas Basler and others
 */
#include "CommandQueue.h"
#include "../inverters/InverterAbstract.h"
#include <algorithm>

void CommandQueue::removeAllEntriesForInverter(InverterAbstract* inv)
{
    std::lock_guard<std::mutex> lock(_mutex);

    auto it = std::remove_if(_queue.begin(), _queue.end(),
        [&](const auto& v) { return v->getTargetAddress() == inv->serial(); });
    _queue.erase(it, _queue.end());
}

void CommandQueue::removeDuplicatedEntries(std::shared_ptr<CommandAbstract> cmd, bool preserveFront)
{
    std::lock_guard<std::mutex> lock(_mutex);

    auto begin = (preserveFront && !_queue.empty()) ? _queue.begin() + 1 : _queue.begin();
    auto it = std::remove_if(begin, _queue.end(),
        [&](const auto& v) {
            return cmd->areSameParameter(v.get())
                && cmd.get()->getQueueInsertType() == QueueInsertType::RemoveOldest;
        });
    _queue.erase(it, _queue.end());
}

void CommandQueue::replaceEntries(std::shared_ptr<CommandAbstract> cmd, bool preserveFront)
{
    std::lock_guard<std::mutex> lock(_mutex);

    auto begin = (preserveFront && !_queue.empty()) ? _queue.begin() + 1 : _queue.begin();
    std::replace_if(begin, _queue.end(),
        [&](const auto& v) {
            return cmd.get()->getQueueInsertType() == QueueInsertType::ReplaceExistent
                && cmd->areSameParameter(v.get());
            },
        cmd
    );
}

void CommandQueue::pushPriority(std::shared_ptr<CommandAbstract> cmd, bool preserveFront)
{
    std::lock_guard<std::mutex> lock(_mutex);

    auto position = (preserveFront && !_queue.empty())
        ? _queue.begin() + 1
        : _queue.begin();
    _queue.insert(position, cmd);
}

uint8_t CommandQueue::countSimilarCommands(std::shared_ptr<CommandAbstract> cmd)
{
    std::lock_guard<std::mutex> lock(_mutex);

    return std::count_if(_queue.begin(), _queue.end(),
        [&](const auto& v) {
            return cmd->areSameParameter(v.get());
        });
}
