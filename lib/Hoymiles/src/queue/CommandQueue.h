// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../commands/CommandAbstract.h"
#include <ThreadSafeQueue.h>
#include <cstdint>
#include <memory>
#include <vector>

class InverterAbstract;

class CommandQueue : public ThreadSafeQueue<std::shared_ptr<CommandAbstract>> {
public:
    struct RemoveResult {
        std::vector<std::shared_ptr<CommandAbstract>> commands;
        bool removedFront = false;
    };

    RemoveResult removeAllEntriesForInverter(InverterAbstract* inv);
    std::vector<std::shared_ptr<CommandAbstract>> removeDuplicatedEntries(std::shared_ptr<CommandAbstract> cmd, bool preserveFront);
    std::vector<std::shared_ptr<CommandAbstract>> replaceEntries(std::shared_ptr<CommandAbstract> cmd, bool preserveFront);
    void pushPriority(std::shared_ptr<CommandAbstract> cmd, bool preserveFront);

    uint32_t countCommandsForTarget(uint64_t targetAddress) const;
    uint8_t countSimilarCommands(std::shared_ptr<CommandAbstract> cmd);
};
