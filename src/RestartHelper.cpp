// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024-2026 Thomas Basler and others
 */
#include "RestartHelper.h"
#include "Display_Graphic.h"
#include "Led_Single.h"
#include <Esp.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <cstring>

#undef TAG
static const char* TAG = "RestartHelper";

namespace {

static constexpr uint32_t sRestartBreadcrumbMagic = 0x46525452; // "F RTR"
static constexpr size_t sRestartReasonLength = 64;

RTC_NOINIT_ATTR uint32_t sPendingRestartMagic;
RTC_NOINIT_ATTR char sPendingRestartReason[sRestartReasonLength];
RTC_NOINIT_ATTR uint32_t sLastRestartMagic;
RTC_NOINIT_ATTR char sLastRestartReason[sRestartReasonLength];

void copyRestartReason(char* target, char const* reason)
{
    if (reason == nullptr || reason[0] == '\0') {
        reason = "unspecified";
    }

    std::strncpy(target, reason, sRestartReasonLength - 1);
    target[sRestartReasonLength - 1] = '\0';
}

} // namespace

RestartHelperClass RestartHelper;

RestartHelperClass::RestartHelperClass()
    : _rebootTask(1 * TASK_SECOND, TASK_FOREVER, std::bind(&RestartHelperClass::loop, this))
{
}

void RestartHelperClass::init(Scheduler& scheduler)
{
    if (sPendingRestartMagic == sRestartBreadcrumbMagic) {
        sLastRestartMagic = sRestartBreadcrumbMagic;
        copyRestartReason(sLastRestartReason, sPendingRestartReason);
        sPendingRestartMagic = 0;
        sPendingRestartReason[0] = '\0';
        ESP_LOGW(TAG, "previous controller restart was requested by %s",
                sLastRestartReason);
    } else {
        sLastRestartMagic = 0;
        sLastRestartReason[0] = '\0';
    }

    scheduler.addTask(_rebootTask);
}

void RestartHelperClass::triggerRestart(char const* reason)
{
    copyRestartReason(sPendingRestartReason, reason);
    sPendingRestartMagic = sRestartBreadcrumbMagic;
    ESP_LOGW(TAG, "scheduling controller restart: %s", sPendingRestartReason);
    _rebootTask.enable();
    _rebootTask.restart();
}

bool RestartHelperClass::wasLastRestartRequested() const
{
    return sLastRestartMagic == sRestartBreadcrumbMagic;
}

char const* RestartHelperClass::getLastRestartReason() const
{
    return wasLastRestartRequested() ? sLastRestartReason : "";
}

void RestartHelperClass::loop()
{
    if (_rebootTask.isFirstIteration()) {
        LedSingle.turnAllOff();
        Display.setStatus(false);
    } else {
        ESP.restart();
    }
}
