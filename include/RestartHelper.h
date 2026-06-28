// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <TaskSchedulerDeclarations.h>

class RestartHelperClass {
public:
    RestartHelperClass();
    void init(Scheduler& scheduler);
    void triggerRestart(char const* reason = "unspecified");
    bool wasLastRestartRequested() const;
    char const* getLastRestartReason() const;

private:
    void loop();

    Task _rebootTask;
};

extern RestartHelperClass RestartHelper;
