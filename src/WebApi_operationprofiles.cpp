// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Thomas Basler and others
 */
#include "WebApi_operationprofiles.h"
#include "OperationProfiles.h"
#include "WebApi.h"
#include "WebApi_errors.h"
#include <AsyncJson.h>

namespace {
void sendOperationResult(
    AsyncWebServerRequest* request,
    AsyncJsonResponse* response,
    bool success,
    String const& message)
{
    auto& retMsg = response->getRoot();
    retMsg["type"] = success ? "success" : "warning";
    retMsg["message"] = message;
    retMsg["code"] = success ? WebApiError::GenericSuccess : WebApiError::GenericValueMissing;
    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}
}  // namespace

void WebApiOperationProfilesClass::init(AsyncWebServer& server, Scheduler& scheduler)
{
    using std::placeholders::_1;

    server.on("/api/operationprofiles/status", HTTP_GET, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiOperationProfilesClass::onStatus, this, _1)));
    server.on("/api/operationprofiles/create", HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiOperationProfilesClass::onCreatePost, this, _1)));
    server.on("/api/operationprofiles/apply", HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiOperationProfilesClass::onApplyPost, this, _1)));
    server.on("/api/operationprofiles/rename", HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiOperationProfilesClass::onRenamePost, this, _1)));
    server.on("/api/operationprofiles/delete", HTTP_POST, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiOperationProfilesClass::onDeletePost, this, _1)));
}

void WebApiOperationProfilesClass::onStatus(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto root = response->getRoot().as<JsonObject>();

    String error;
    if (!OperationProfiles.serializeStatus(root, error)) {
        root["type"] = "warning";
        root["message"] = error;
        root["code"] = WebApiError::GenericInternalServerError;
        WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
        return;
    }

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}

void WebApiOperationProfilesClass::onCreatePost(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    JsonDocument root;
    if (!WebApi.parseRequestData(request, response, root)) {
        return;
    }

    String error;
    String createdId;
    bool const success = OperationProfiles.createFromCurrentConfig(root["name"] | "", createdId, error);
    auto& retMsg = response->getRoot();
    retMsg["profile_id"] = createdId;
    sendOperationResult(request, response, success, success ? "Profile created from current configuration" : error);
}

void WebApiOperationProfilesClass::onApplyPost(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    JsonDocument root;
    if (!WebApi.parseRequestData(request, response, root)) {
        return;
    }

    String error;
    bool const success = OperationProfiles.applyProfile(root["id"] | "", error);
    sendOperationResult(request, response, success, success ? "Profile applied" : error);
}

void WebApiOperationProfilesClass::onRenamePost(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    JsonDocument root;
    if (!WebApi.parseRequestData(request, response, root)) {
        return;
    }

    String error;
    bool const success = OperationProfiles.renameProfile(root["id"] | "", root["name"] | "", error);
    sendOperationResult(request, response, success, success ? "Profile renamed" : error);
}

void WebApiOperationProfilesClass::onDeletePost(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentials(request)) {
        return;
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    JsonDocument root;
    if (!WebApi.parseRequestData(request, response, root)) {
        return;
    }

    String error;
    bool const success = OperationProfiles.deleteProfile(root["id"] | "", error);
    sendOperationResult(request, response, success, success ? "Profile deleted" : error);
}
