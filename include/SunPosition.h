// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <TaskSchedulerDeclarations.h>
#include <atomic>
#include <ctime>
#include <sunset.h>

class SunPositionClass {
public:
    SunPositionClass();
    void init(Scheduler& scheduler);

    bool isDayPeriod() const;
    bool isSunsetAvailable() const;
    bool sunTimes(struct tm* sunrise, struct tm* sunset, time_t timestamp) const;
    bool sunsetTime(struct tm* info) const;
    bool sunriseTime(struct tm* info) const;
    void setDoRecalc(const bool doRecalc);

private:
    void loop();
    void updateSunData();
    bool checkRecalcDayChanged() const;
    bool calculateSunMinutes(struct tm const& timeinfo, uint32_t& sunriseMinutes, uint32_t& sunsetMinutes) const;
    bool getSunTime(struct tm* info, const uint32_t offset, time_t timestamp) const;

    Task _loopTask;

    bool _isSunsetAvailable = true;
    uint32_t _sunriseMinutes = 0;
    uint32_t _sunsetMinutes = 0;

    bool _isValidInfo = false;
    std::atomic_bool _doRecalc = true;
    uint32_t _lastSunPositionCalculatedYMD = 0;
};

extern SunPositionClass SunPosition;
