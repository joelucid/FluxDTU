// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Thomas Basler and others
 */
#include "ResetDiagnostics.h"
#include <esp_attr.h>
#include <esp_debug_helpers.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <inttypes.h>

#undef TAG
static const char* TAG = "ResetDiagnostics";

namespace {

static constexpr uint32_t sResetDiagnosticsMagic = 0x46524447; // "FRDG"
static constexpr uint32_t sResetDiagnosticsVersion = 1;

RTC_NOINIT_ATTR uint32_t sPendingSnapshotMagic;
RTC_NOINIT_ATTR ResetDiagnosticsSnapshot sPendingSnapshot;
RTC_NOINIT_ATTR uint32_t sLastSnapshotMagic;
RTC_NOINIT_ATTR ResetDiagnosticsSnapshot sLastSnapshot;

void copyTaskName(char* target, char const* name)
{
    if (name == nullptr || name[0] == '\0') {
        name = "unknown";
    }

    std::strncpy(target, name, RESET_DIAGNOSTICS_TASK_NAME_LENGTH - 1);
    target[RESET_DIAGNOSTICS_TASK_NAME_LENGTH - 1] = '\0';
}

void captureBacktrace(ResetDiagnosticsSnapshot& snapshot)
{
    uint32_t pc = 0;
    uint32_t sp = 0;
    uint32_t nextPc = 0;
    esp_backtrace_get_start(&pc, &sp, &nextPc);

    esp_backtrace_frame_t frame = {};
    frame.pc = pc;
    frame.sp = sp;
    frame.next_pc = nextPc;

    while (snapshot.backtrace_depth < RESET_DIAGNOSTICS_BACKTRACE_DEPTH && frame.pc != 0) {
        snapshot.backtrace[snapshot.backtrace_depth++] = frame.pc;
        if (!esp_backtrace_get_next_frame(&frame)) {
            break;
        }
    }
}

void captureShutdownSnapshot()
{
    sPendingSnapshotMagic = 0;
    std::memset(&sPendingSnapshot, 0, sizeof(sPendingSnapshot));

    sPendingSnapshot.version = sResetDiagnosticsVersion;
    sPendingSnapshot.uptime_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
    sPendingSnapshot.core_id = static_cast<uint32_t>(xPortGetCoreID());
    sPendingSnapshot.heap_free = esp_get_free_heap_size();
    sPendingSnapshot.heap_min_free = esp_get_minimum_free_heap_size();

    TaskHandle_t const currentTask = xTaskGetCurrentTaskHandle();
    if (currentTask != nullptr) {
        copyTaskName(sPendingSnapshot.task_name, pcTaskGetName(currentTask));
        sPendingSnapshot.stack_watermark = uxTaskGetStackHighWaterMark(currentTask);
    } else {
        copyTaskName(sPendingSnapshot.task_name, nullptr);
    }

    captureBacktrace(sPendingSnapshot);

    sPendingSnapshotMagic = sResetDiagnosticsMagic;
}

} // namespace

ResetDiagnosticsClass ResetDiagnostics;

void ResetDiagnosticsClass::init()
{
    if (sPendingSnapshotMagic == sResetDiagnosticsMagic
            && sPendingSnapshot.version == sResetDiagnosticsVersion) {
        std::memcpy(&sLastSnapshot, &sPendingSnapshot, sizeof(sLastSnapshot));
        sLastSnapshotMagic = sResetDiagnosticsMagic;
        ESP_LOGW(TAG, "previous software shutdown captured: task=%s core=%" PRIu32 " frames=%" PRIu32,
                sLastSnapshot.task_name,
                sLastSnapshot.core_id,
                sLastSnapshot.backtrace_depth);
    } else {
        sLastSnapshotMagic = 0;
        std::memset(&sLastSnapshot, 0, sizeof(sLastSnapshot));
    }

    sPendingSnapshotMagic = 0;
    std::memset(&sPendingSnapshot, 0, sizeof(sPendingSnapshot));

    esp_err_t const err = esp_register_shutdown_handler(captureShutdownSnapshot);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "failed to register shutdown handler: %s", esp_err_to_name(err));
    }
}

bool ResetDiagnosticsClass::wasLastShutdownCaptured() const
{
    return sLastSnapshotMagic == sResetDiagnosticsMagic
            && sLastSnapshot.version == sResetDiagnosticsVersion;
}

ResetDiagnosticsSnapshot const& ResetDiagnosticsClass::getLastShutdownSnapshot() const
{
    return sLastSnapshot;
}
