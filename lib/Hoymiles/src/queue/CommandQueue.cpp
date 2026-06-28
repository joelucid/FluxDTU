// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024-2026 Thomas Basler and others
 */
#include "CommandQueue.h"
#include "../inverters/InverterAbstract.h"
#include <algorithm>

CommandQueue::RemoveResult CommandQueue::removeAllEntriesForInverter(InverterAbstract* inv)
{
    std::lock_guard<std::mutex> lock(_mutex);

    RemoveResult result;
    if (!_queue.empty() && _queue.front()->getTargetAddress() == inv->serial()) {
        result.removedFront = true;
    }

    auto it = std::remove_if(_queue.begin(), _queue.end(),
        [&](const auto& v) {
            if (v->getTargetAddress() != inv->serial()) {
                return false;
            }
            result.commands.push_back(v);
            return true;
        });
    _queue.erase(it, _queue.end());
    return result;
}

std::vector<std::shared_ptr<CommandAbstract>> CommandQueue::removeDuplicatedEntries(std::shared_ptr<CommandAbstract> cmd, bool preserveFront)
{
    std::lock_guard<std::mutex> lock(_mutex);

    std::vector<std::shared_ptr<CommandAbstract>> removed;
    auto begin = (preserveFront && !_queue.empty()) ? _queue.begin() + 1 : _queue.begin();
    auto it = std::remove_if(begin, _queue.end(),
        [&](const auto& v) {
            if (!cmd->areSameParameter(v.get())
                    || cmd.get()->getQueueInsertType() != QueueInsertType::RemoveOldest) {
                return false;
            }
            removed.push_back(v);
            return true;
        });
    _queue.erase(it, _queue.end());
    return removed;
}

std::vector<std::shared_ptr<CommandAbstract>> CommandQueue::replaceEntries(std::shared_ptr<CommandAbstract> cmd, bool preserveFront)
{
    std::lock_guard<std::mutex> lock(_mutex);

    std::vector<std::shared_ptr<CommandAbstract>> replaced;
    auto begin = (preserveFront && !_queue.empty()) ? _queue.begin() + 1 : _queue.begin();
    for (auto iter = begin; iter != _queue.end(); ++iter) {
        if (cmd.get()->getQueueInsertType() != QueueInsertType::ReplaceExistent
                || !cmd->areSameParameter(iter->get())) {
            continue;
        }
        replaced.push_back(*iter);
        *iter = cmd;
    }
    return replaced;
}

void CommandQueue::pushPriority(std::shared_ptr<CommandAbstract> cmd, bool preserveFront)
{
    std::lock_guard<std::mutex> lock(_mutex);

    auto begin = (preserveFront && !_queue.empty())
        ? _queue.begin() + 1
        : _queue.begin();
    auto position = std::find_if(begin, _queue.end(),
        [](const auto& v) { return !v->isHighPriority(); });
    _queue.insert(position, cmd);
}

uint32_t CommandQueue::countCommandsForTarget(uint64_t targetAddress) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return std::count_if(_queue.begin(), _queue.end(),
        [&](const auto& v) {
            return v->getTargetAddress() == targetAddress;
        });
}

uint8_t CommandQueue::countSimilarCommands(std::shared_ptr<CommandAbstract> cmd)
{
    std::lock_guard<std::mutex> lock(_mutex);

    return std::count_if(_queue.begin(), _queue.end(),
        [&](const auto& v) {
            return cmd->areSameParameter(v.get());
        });
}
