// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <ESPAsyncWebServer.h>
#include <TaskSchedulerDeclarations.h>

class WebApiOperationProfilesClass {
public:
    void init(AsyncWebServer& server, Scheduler& scheduler);

private:
    void onStatus(AsyncWebServerRequest* request);
    void onCreatePost(AsyncWebServerRequest* request);
    void onApplyPost(AsyncWebServerRequest* request);
    void onRenamePost(AsyncWebServerRequest* request);
    void onDeletePost(AsyncWebServerRequest* request);
};
