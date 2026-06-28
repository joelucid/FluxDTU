// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <ESPAsyncWebServer.h>
#include <TaskSchedulerDeclarations.h>


class WebApiPowerLimiterClass {
public:
    void init(AsyncWebServer& server, Scheduler& scheduler);

private:
    void onStatus(AsyncWebServerRequest* request);
    void onStatePersistenceStatus(AsyncWebServerRequest* request);
    void onStatePersistenceDelete(AsyncWebServerRequest* request);
    void onMetaData(AsyncWebServerRequest* request);
    void onTrace(AsyncWebServerRequest* request);
    void onAdminGet(AsyncWebServerRequest* request);
    void onAdminPost(AsyncWebServerRequest* request);
    void onFlexibleLoadEnabledPost(AsyncWebServerRequest* request);

    AsyncWebServer* _server;
};
