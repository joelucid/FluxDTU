// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023-2026 Thomas Basler and others
 */
#include "SunPosition.h"
#include "Configuration.h"
#include "Utils.h"
#include <Arduino.h>
#include <cmath>

#define CALC_UNIQUE_ID(tm) (((tm.tm_year << 9) | (tm.tm_mon << 5) | tm.tm_mday) << 1 | tm.tm_isdst)

SunPositionClass SunPosition;

SunPositionClass::SunPositionClass()
    : _loopTask(5 * TASK_SECOND, TASK_FOREVER, std::bind(&SunPositionClass::loop, this))
{
}

void SunPositionClass::init(Scheduler& scheduler)
{
    scheduler.addTask(_loopTask);
    _loopTask.enable();
}

void SunPositionClass::loop()
{
    if (_doRecalc || checkRecalcDayChanged()) {
        updateSunData();
    }
}

bool SunPositionClass::isDayPeriod() const
{
    if (!_isValidInfo) {
        return true;
    }

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    const uint32_t minutesPastMidnight = tm.tm_hour * 60 + tm.tm_min;
    return (minutesPastMidnight >= _sunriseMinutes) && (minutesPastMidnight < _sunsetMinutes);
}

// Returns if sunset/sunrise exists (e.g. in norway sunset/sunrise don't happen in summer months)
bool SunPositionClass::isSunsetAvailable() const
{
    return _isSunsetAvailable;
}

void SunPositionClass::setDoRecalc(const bool doRecalc)
{
    _doRecalc = doRecalc;
}

bool SunPositionClass::checkRecalcDayChanged() const
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    return _lastSunPositionCalculatedYMD != CALC_UNIQUE_ID(tm);
}

void SunPositionClass::updateSunData()
{
    struct tm timeinfo;
    const bool gotLocalTime = getLocalTime(&timeinfo, 5);

    _lastSunPositionCalculatedYMD = CALC_UNIQUE_ID(timeinfo);
    setDoRecalc(false);

    if (!gotLocalTime) {
        _sunriseMinutes = 0;
        _sunsetMinutes = 0;
        _isSunsetAvailable = true;
        _isValidInfo = false;
        return;
    }

    uint32_t sunriseMinutes = 0;
    uint32_t sunsetMinutes = 0;
    if (!calculateSunMinutes(timeinfo, sunriseMinutes, sunsetMinutes)) {
        _sunriseMinutes = 0;
        _sunsetMinutes = 0;
        _isSunsetAvailable = false;
        _isValidInfo = false;
        return;
    }

    _sunriseMinutes = sunriseMinutes;
    _sunsetMinutes = sunsetMinutes;

    _isSunsetAvailable = true;
    _isValidInfo = true;
}

bool SunPositionClass::calculateSunMinutes(struct tm const& timeinfo, uint32_t& sunriseMinutes, uint32_t& sunsetMinutes) const
{
    CONFIG_T const& config = Configuration.get();

    double sunset_type;
    switch (config.Ntp.SunsetType) {
    case 0:
        sunset_type = SunSet::SUNSET_OFFICIAL;
        break;
    case 2:
        sunset_type = SunSet::SUNSET_CIVIL;
        break;
    case 3:
        sunset_type = SunSet::SUNSET_ASTONOMICAL;
        break;
    default:
        sunset_type = SunSet::SUNSET_NAUTICAL;
        break;
    }

    const int offset = Utils::getTimezoneOffset() / 3600;

    SunSet sun;
    sun.setPosition(config.Ntp.Latitude, config.Ntp.Longitude, offset);
    sun.setCurrentDate(1900 + timeinfo.tm_year, timeinfo.tm_mon + 1, timeinfo.tm_mday);

    const double sunriseRaw = sun.calcCustomSunrise(sunset_type);
    const double sunsetRaw = sun.calcCustomSunset(sunset_type);
    if (std::isnan(sunriseRaw) || std::isnan(sunsetRaw)) {
        return false;
    }

    sunriseMinutes = static_cast<uint32_t>(sunriseRaw);
    sunsetMinutes = static_cast<uint32_t>(sunsetRaw);
    return true;
}

bool SunPositionClass::getSunTime(struct tm* info, const uint32_t offset, time_t timestamp) const
{
    struct tm tm;
    localtime_r(&timestamp, &tm);

    tm.tm_sec = 0;
    tm.tm_min = offset;
    tm.tm_hour = 0;
    tm.tm_isdst = -1;

    const time_t midnight = mktime(&tm);
    localtime_r(&midnight, info);
    return true;
}

bool SunPositionClass::sunTimes(struct tm* sunrise, struct tm* sunset, time_t timestamp) const
{
    struct tm timeinfo;
    localtime_r(&timestamp, &timeinfo);

    uint32_t sunriseMinutes = 0;
    uint32_t sunsetMinutes = 0;
    if (_isValidInfo && _lastSunPositionCalculatedYMD == CALC_UNIQUE_ID(timeinfo)) {
        sunriseMinutes = _sunriseMinutes;
        sunsetMinutes = _sunsetMinutes;
    } else if (!calculateSunMinutes(timeinfo, sunriseMinutes, sunsetMinutes)) {
        return false;
    }

    if (sunrise) { getSunTime(sunrise, sunriseMinutes, timestamp); }
    if (sunset) { getSunTime(sunset, sunsetMinutes, timestamp); }
    return true;
}

bool SunPositionClass::sunsetTime(struct tm* info) const
{
    if (!_isValidInfo) { return false; }
    return getSunTime(info, _sunsetMinutes, time(NULL));
}

bool SunPositionClass::sunriseTime(struct tm* info) const
{
    if (!_isValidInfo) { return false; }
    return getSunTime(info, _sunriseMinutes, time(NULL));
}
