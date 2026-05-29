// SPDX-License-Identifier: GPL-2.0-or-later
#include <MqttSettings.h>
#include <battery/pylontech/Stats.h>

namespace Batteries::Pylontech {

void Stats::getLiveViewData(JsonVariant& root) const
{
    String manufacturer = "unknown";
    if (getManufacturer().has_value()) { manufacturer = *getManufacturer(); }

    root["manufacturer"] = manufacturer;
    if (!_serial.isEmpty()) {
        root["serial"] = _serial;
    }
    if (!_fwversion.isEmpty()) {
        root["fwversion"] = _fwversion;
    }
    if (!_hwversion.isEmpty()) {
        root["hwversion"] = _hwversion;
    }
    root["data_age"] = getAgeSeconds();
    root["showIssues"] = supportsAlarmsAndWarnings();

    if (isSoCValid()) {
        addLiveViewInSection(root, "status", "SoC", getSoC(), "%", getSoCPrecision());
    }

    if (isVoltageValid()) {
        addLiveViewInSection(root, "status", "voltage", getVoltage(), "V", 2);
    }

    if (isCurrentValid()) {
        addLiveViewInSection(root, "status", "current", getChargeCurrent(), "A", getChargeCurrentPrecision());
    }

    if (isDischargeCurrentLimitValid()) {
        addLiveViewInSection(root, "limits", "dischargeCurrentLimitation", getDischargeCurrentLimit(), "A", 1);
    }

    if (isChargeCurrentLimitValid()) {
        addLiveViewInSection(root, "limits", "chargeCurrentLimitation", getChargeCurrentLimit(), "A", 1);
    }

    addLiveViewInSection(root, "limits", "chargeVoltage", _chargeVoltage, "V", 1);
    addLiveViewInSection(root, "limits", "dischargeVoltageLimitation", _dischargeVoltageLimitation, "V", 1);

    addLiveViewInSection(root, "health", "stateOfHealth", _stateOfHealth, "%", 0);

    auto oTemperature = getTemperature();
    if (oTemperature) {
        addLiveViewInSection(root, "health", "temperature", *oTemperature, "°C", 1);
    }

    addLiveViewInSection(root, "health", "modules", _moduleCount, "", 0);

    addLiveViewTextInSection(root, "operation", "chargeEnabled", (_chargeEnabled?"yes":"no"));
    addLiveViewTextInSection(root, "operation", "dischargeEnabled", (_dischargeEnabled?"yes":"no"));
    addLiveViewTextInSection(root, "operation", "chargeImmediately", (_chargeImmediately?"yes":"no"));

    // alarms and warnings go into the "Issues" card of the web application
    addLiveViewWarning(root, "highCurrentDischarge", _warningHighCurrentDischarge);
    addLiveViewAlarm(root, "overCurrentDischarge", _alarmOverCurrentDischarge);

    addLiveViewWarning(root, "highCurrentCharge", _warningHighCurrentCharge);
    addLiveViewAlarm(root, "overCurrentCharge", _alarmOverCurrentCharge);

    addLiveViewWarning(root, "lowTemperature", _warningLowTemperature);
    addLiveViewAlarm(root, "underTemperature", _alarmUnderTemperature);

    addLiveViewWarning(root, "highTemperature", _warningHighTemperature);
    addLiveViewAlarm(root, "overTemperature", _alarmOverTemperature);

    addLiveViewWarning(root, "lowVoltage", _warningLowVoltage);
    addLiveViewAlarm(root, "underVoltage", _alarmUnderVoltage);

    addLiveViewWarning(root, "highVoltage", _warningHighVoltage);
    addLiveViewAlarm(root, "overVoltage", _alarmOverVoltage);

    addLiveViewWarning(root, "bmsInternal", _warningBmsInternal);
    addLiveViewAlarm(root, "bmsInternal", _alarmBmsInternal);
}

void Stats::mqttPublish() const
{
    ::Batteries::Stats::mqttPublish();

    MqttSettings.publish("battery/settings/chargeVoltage", String(_chargeVoltage));
    MqttSettings.publish("battery/settings/dischargeVoltageLimitation", String(_dischargeVoltageLimitation));
    MqttSettings.publish("battery/stateOfHealth", String(_stateOfHealth));

    auto oTemperature = getTemperature();
    if (oTemperature) {
        MqttSettings.publish("battery/temperature", String(*oTemperature));
    }

    MqttSettings.publish("battery/alarm/overCurrentDischarge", String(_alarmOverCurrentDischarge));
    MqttSettings.publish("battery/alarm/overCurrentCharge", String(_alarmOverCurrentCharge));
    MqttSettings.publish("battery/alarm/underTemperature", String(_alarmUnderTemperature));
    MqttSettings.publish("battery/alarm/overTemperature", String(_alarmOverTemperature));
    MqttSettings.publish("battery/alarm/underVoltage", String(_alarmUnderVoltage));
    MqttSettings.publish("battery/alarm/overVoltage", String(_alarmOverVoltage));
    MqttSettings.publish("battery/alarm/bmsInternal", String(_alarmBmsInternal));
    MqttSettings.publish("battery/warning/highCurrentDischarge", String(_warningHighCurrentDischarge));
    MqttSettings.publish("battery/warning/highCurrentCharge", String(_warningHighCurrentCharge));
    MqttSettings.publish("battery/warning/lowTemperature", String(_warningLowTemperature));
    MqttSettings.publish("battery/warning/highTemperature", String(_warningHighTemperature));
    MqttSettings.publish("battery/warning/lowVoltage", String(_warningLowVoltage));
    MqttSettings.publish("battery/warning/highVoltage", String(_warningHighVoltage));
    MqttSettings.publish("battery/warning/bmsInternal", String(_warningBmsInternal));
    MqttSettings.publish("battery/charging/chargeEnabled", String(_chargeEnabled));
    MqttSettings.publish("battery/charging/dischargeEnabled", String(_dischargeEnabled));
    MqttSettings.publish("battery/charging/chargeImmediately", String(_chargeImmediately));
    MqttSettings.publish("battery/modulesTotal", String(_moduleCount));
}

} // namespace Batteries::Pylontech
