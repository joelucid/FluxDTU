// SPDX-License-Identifier: GPL-2.0-or-later
#include "WebApi_statistics.h"
#include "Statistics.h"
#include "WebApi.h"
#include <AsyncJson.h>
#include <cstdlib>

void WebApiStatisticsClass::init(AsyncWebServer& server, Scheduler& scheduler)
{
    using std::placeholders::_1;

    server.on("/api/statistics/status", HTTP_GET, static_cast<ArRequestHandlerFunction>(std::bind(&WebApiStatisticsClass::onStatus, this, _1)));
}

void WebApiStatisticsClass::onStatus(AsyncWebServerRequest* request)
{
    if (!WebApi.checkCredentialsReadonly(request)) { return; }

    String period = "today";
    if (request->hasParam("period")) {
        period = request->getParam("period")->value();
    }
    bool compactSamples = false;
    if (request->hasParam("format")) {
        compactSamples = request->getParam("format")->value() == "compact";
    }
    uint32_t requestedFrom = 0;
    if (request->hasParam("from")) {
        String value = request->getParam("from")->value();
        char* end = nullptr;
        const auto parsed = strtoul(value.c_str(), &end, 10);
        if (end != value.c_str()) {
            requestedFrom = static_cast<uint32_t>(parsed);
        }
    }
    String view = "flow";
    if (request->hasParam("view")) {
        view = request->getParam("view")->value();
    }

    AsyncJsonResponse* response = new AsyncJsonResponse();
    auto& root = response->getRoot();

    Statistics.getStatus(root, period, compactSamples, requestedFrom, view);

    WebApi.sendJsonResponse(request, response, __FUNCTION__, __LINE__);
}
