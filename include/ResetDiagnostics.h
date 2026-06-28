// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

static constexpr size_t RESET_DIAGNOSTICS_TASK_NAME_LENGTH = 24;
static constexpr size_t RESET_DIAGNOSTICS_BACKTRACE_DEPTH = 16;

struct ResetDiagnosticsSnapshot {
    uint32_t version;
    uint32_t uptime_ms;
    uint32_t core_id;
    uint32_t heap_free;
    uint32_t heap_min_free;
    uint32_t stack_watermark;
    uint32_t backtrace_depth;
    char task_name[RESET_DIAGNOSTICS_TASK_NAME_LENGTH];
    uint32_t backtrace[RESET_DIAGNOSTICS_BACKTRACE_DEPTH];
};

class ResetDiagnosticsClass {
public:
    void init();
    bool wasLastShutdownCaptured() const;
    ResetDiagnosticsSnapshot const& getLastShutdownSnapshot() const;
};

extern ResetDiagnosticsClass ResetDiagnostics;
