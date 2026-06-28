// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2024 Thomas Basler and others
 */
#include "WebApi_powerlimiter.h"
#include "ArduinoJson.h"
#include "AsyncJson.h"
#include "Configuration.h"
#include "FlexibleLoadStats.h"
#include "MqttHandlePowerLimiterHass.h"
#include "OperationProfiles.h"
#include "PowerLimiter.h"
#include "WebApi.h"
#include "helper.h"
#include "WebApi_errors.h"
#include "Configuration.h"
#include <algorithm>
#include <memory>

void WebApiPowerLimiterClass::init(AsyncWebServer& server, Scheduler& scheduler)
{
    using std::placeholders::_1;

    _server = &server;

    _server->on("/api/powerlimiter/status", HTTP_GET, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiPowerLimiterClass::onStatus, this, _1)));
    _server->on("/api/powerlimiter/state", HTTP_GET, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiPowerLimiterClass::onStatePersistenceStatus, this, _1)));
    _server->on("/api/powerlimiter/state/delete", HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiPowerLimiterClass::onStatePersistenceDelete, this, _1)));
    _server->on("/api/powerlimiter/config", HTTP_GET, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiPowerLimiterClass::onAdminGet, this, _1)));
    _server->on("/api/powerlimiter/config", HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiPowerLimiterClass::onAdminPost, this, _1)));
    _server->on("/api/powerlimiter/flexible_load/enabled", HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiPowerLimiterClass::onFlexibleLoadEnabledPost, this, _1)));
    _server->on("/api/powerlimiter/metadata", HTTP_GET, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiPowerLimiterClass::onMetaData, this, _1)));
    _server->on("/api/powerlimiter/trace", HTTP_GET, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiPowerLimiterClass::onTrace, this, _1)));
}

void WebApiPowerLimiterClass::onStatus(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentialsReadonly(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto root = response->getRoot().as<JsonObject>();
    auto const& config = Configuration.get();
    ConfigurationClass::serializePowerLimiterConfig(config.PowerLimiter, root);
    if (request->hasParam("debug") || request->hasParam("thermal_debug")) {
        auto thermalDebug = root["thermal_debug"].to<JsonObject>();
        PowerLimiter.addThermalDebugJson(thermalDebug);
        auto runtimeDebug = root["runtime_debug"].to<JsonObject>();
        PowerLimiter.addRuntimeDebugJson(runtimeDebug);
    }
    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiPowerLimiterClass::onStatePersistenceStatus(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentialsReadonly(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto root = response->getRoot().as<JsonObject>();
    PowerLimiter.addStatePersistenceJson(root);
    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiPowerLimiterClass::onStatePersistenceDelete(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto& root = response->getRoot();
    bool const success = PowerLimiter.clearPersistedRuntimeState();
    root["type"] = success ? "success" : "warning";
    root["message"] = success ? "Power limiter state deleted" : "Power limiter state not deleted";
    root["code"] = success ? WebApiError::GenericSuccess : WebApiError::GenericInternalServerError;
    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiPowerLimiterClass::onMetaData(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) { return; }

    auto const& config = Configuration.get();

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto& root = response->getRoot();

    root["power_meter_enabled"] = config.PowerMeter.Enabled;
    root["battery_enabled"] = config.Battery.Enabled;
    root["charge_controller_enabled"] = config.SolarCharger.Enabled;
    root["mqtt_enabled"] = config.Mqtt.Enabled;

    JsonArray inverters = root["inverters"].to<JsonArray>();
    for (uint8_t i = 0; i < INV_MAX_COUNT; i++) {
        auto inv = Hoymiles.getInverterBySerial(config.Inverter[i].Serial);
        if (!inv) { continue; }

        JsonObject obj = inverters.add<JsonObject>();
        obj["serial"] = inv->serialString();
        obj["pos"] = i;
        obj["order"] = config.Inverter[i].Order;
        obj["name"] = String(config.Inverter[i].Name);
        obj["poll_enable"] = config.Inverter[i].Poll_Enable;
        obj["poll_enable_night"] = config.Inverter[i].Poll_Enable_Night;
        obj["command_enable"] = config.Inverter[i].Command_Enable;
        obj["command_enable_night"] = config.Inverter[i].Command_Enable_Night;
        obj["max_power"] = inv->getMaxPower(); // okay if zero/unknown
        obj["detected_max_power"] = inv->getDetectedMaxPower();
        obj["max_power_override"] = inv->getMaxPowerOverride();
        obj["type"] = inv->typeName();
        auto channels = inv->Statistics()->getChannelsByType(TYPE_DC);
        obj["channels"] = channels.size();
        obj["pdl_supported"] = inv->supportsPowerDistributionLogic();
    }

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiPowerLimiterClass::onTrace(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentialsReadonly(request)) { return; }

    uint32_t offsetSeconds = 0;
    uint32_t requestedRangeSeconds = 0;
    if (request->hasParam("offset_s")) {
        auto const requestedOffsetSeconds = request->getParam("offset_s")->value().toInt();
        offsetSeconds = static_cast<uint32_t>(std::max<int32_t>(0, requestedOffsetSeconds));
    }
    if (request->hasParam("from_age_s")) {
        auto const requestedOffsetSeconds = request->getParam("from_age_s")->value().toInt();
        offsetSeconds = static_cast<uint32_t>(std::max<int32_t>(0, requestedOffsetSeconds));
    }
    if (request->hasParam("to_age_s")) {
        auto const requestedToSeconds = request->getParam("to_age_s")->value().toInt();
        auto const toSeconds = static_cast<uint32_t>(std::max<int32_t>(0, requestedToSeconds));
        requestedRangeSeconds = toSeconds > offsetSeconds ? toSeconds - offsetSeconds : 0;
    }
    if (request->hasParam("window_s") && requestedRangeSeconds == 0) {
        auto const requestedWindowSeconds = request->getParam("window_s")->value().toInt();
        requestedRangeSeconds = static_cast<uint32_t>(std::max<int32_t>(0, requestedWindowSeconds));
    }

    bool compactV2 = false;
    bool compact = request->hasParam("compact");
    if (request->hasParam("format")) {
        auto const format = request->getParam("format")->value();
        compactV2 = format == "compact-v2";
        compact = compact || compactV2;
    }
    if (request->hasParam("compact")) {
        auto const compactValue = request->getParam("compact")->value();
        compactV2 = compactV2 || compactValue == "2" || compactValue == "v2";
    }
    if (compactV2) {
        auto cursor = std::make_shared<PowerLimiterClass::TraceCompactV2ChunkCursor>();
        auto response = request->beginChunkedResponse(
                asyncsrv::T_application_json,
                [cursor, offsetSeconds, requestedRangeSeconds](
                        uint8_t* buffer,
                        size_t maxLen,
                        size_t index) -> size_t {
                    (void)index;
                    return PowerLimiter.writeTraceCompactV2JsonChunk(
                            *cursor,
                            buffer,
                            maxLen,
                            offsetSeconds,
                            requestedRangeSeconds);
                });
        request->send(response);
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto root = response->getRoot().as<JsonObject>();
    PowerLimiter.addTraceJson(root, offsetSeconds, compact, requestedRangeSeconds, compactV2);

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiPowerLimiterClass::onAdminGet(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    this->onStatus(request);
}

void WebApiPowerLimiterClass::onAdminPost(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    JsonDocument root;
    if (!WebApi.parseRequestData(request, response, root)) {
        return;
    }

    auto& retMsg = response->getRoot();

    {
        auto guard = Configuration.getWriteGuard();
        auto& config = guard.getConfig();
        ConfigurationClass::deserializePowerLimiterConfig(root.as<JsonObject>(), config.PowerLimiter);
    }

    WebApi.writeConfig(retMsg);
    if (retMsg["code"].as<int>() == WebApiError::GenericSuccess) {
        String profileError;
        OperationProfiles.syncActiveProfileFromCurrentConfig(profileError);
    }

    response->setLength();
    request->send(response);

    PowerLimiter.triggerReloadingConfig();
    FlexibleLoadStats.updateSettings();

    // potentially make thresholds auto-discoverable
    MqttHandlePowerLimiterHass.forceUpdate();
}

void WebApiPowerLimiterClass::onFlexibleLoadEnabledPost(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    JsonDocument root;
    if (!WebApi.parseRequestData(request, response, root)) {
        return;
    }

    auto& retMsg = response->getRoot();
    retMsg["type"] = "warning";

    if (!root["enabled"].is<bool>()) {
        retMsg["message"] = "Values are missing!";
        retMsg["code"] = WebApiError::GenericValueMissing;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    auto const index = root["index"] | 0;
    if (index < 0 || index >= POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT) {
        retMsg["message"] = "Values are missing!";
        retMsg["code"] = WebApiError::GenericValueMissing;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    bool changed = false;
    {
        auto guard = Configuration.getWriteGuard();
        auto& config = guard.getConfig();
        auto const enabled = root["enabled"].as<bool>();
        auto& flexibleLoad = config.PowerLimiter.FlexibleLoads[index];
        changed = flexibleLoad.Enabled != enabled;
        flexibleLoad.Enabled = enabled;
    }

    WebApi.writeConfig(retMsg);
    if (retMsg["code"].as<int>() == WebApiError::GenericSuccess) {
        String profileError;
        OperationProfiles.syncActiveProfileFromCurrentConfig(profileError);
    }
    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);

    if (changed) {
        PowerLimiter.triggerReloadingConfig();
        MqttHandlePowerLimiterHass.forceUpdate();
    }
}
