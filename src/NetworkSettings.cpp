// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2022-2026 Thomas Basler and others
 */
#include "NetworkSettings.h"
#include "Configuration.h"
#include "SyslogLogger.h"
#include "PinMapping.h"
#include "Utils.h"
#include "__compiled_constants.h"
#include "defaults.h"
#include <ESPmDNS.h>
#include <ETH.h>
#include <cstdio>
#include <cstring>

#undef TAG
static const char* TAG = "network";

namespace {
constexpr int32_t PreferredApMinRssi = -80;
constexpr int32_t MinReselectGain = 8;
constexpr uint32_t ApSelectionCheckIntervalMillis = 60 * 1000;

struct WifiApCandidate {
    bool found = false;
    int32_t rssi = 0;
    int32_t channel = 0;
    uint8_t bssid[6] = {};
    String bssidString;
};

String formatBssid(const uint8_t* bssid)
{
    char result[18];
    snprintf(result, sizeof(result), "%02X:%02X:%02X:%02X:%02X:%02X",
        bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    return result;
}

bool parseBssid(const char* value, uint8_t* bssid)
{
    unsigned int bytes[6];
    if (!value || sscanf(value, "%2x:%2x:%2x:%2x:%2x:%2x",
            &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return false;
    }

    for (uint8_t i = 0; i < 6; i++) {
        if (bytes[i] > 0xff) {
            return false;
        }
        bssid[i] = static_cast<uint8_t>(bytes[i]);
    }

    return true;
}

bool scanStrongestAp(const char* ssid, WifiApCandidate& best)
{
    ESP_LOGI(TAG, "Scanning for strongest WiFi AP using SSID '%s'", ssid);
    const int16_t apCount = WiFi.scanNetworks(false, true, false, 250);
    const String currentBssid = WiFi.BSSIDstr();

    for (int16_t i = 0; i < apCount; i++) {
        const String candidateSsid = WiFi.SSID(static_cast<uint8_t>(i));
        if (candidateSsid != ssid) {
            continue;
        }

        const auto rssi = WiFi.RSSI(static_cast<uint8_t>(i));
        const auto channel = WiFi.channel(static_cast<uint8_t>(i));
        const auto* bssid = WiFi.BSSID(static_cast<uint8_t>(i));
        const String bssidString = formatBssid(bssid);

        ESP_LOGI(TAG, "WiFi candidate: BSSID %s, channel %" PRId32 ", RSSI %" PRId32 " dBm",
            bssidString.c_str(), channel, rssi);

        if (!best.found || rssi > best.rssi || (rssi == best.rssi && bssidString == currentBssid)) {
            best.found = true;
            best.rssi = rssi;
            best.channel = channel;
            memcpy(best.bssid, bssid, sizeof(best.bssid));
            best.bssidString = bssidString;
        }
    }

    if (!best.found) {
        ESP_LOGW(TAG, "No WiFi AP found for SSID '%s' during scan (%" PRId16 ")", ssid, apCount);
    }

    return best.found;
}
} // namespace

NetworkSettingsClass::NetworkSettingsClass()
    : _loopTask(TASK_IMMEDIATE, TASK_FOREVER, std::bind(&NetworkSettingsClass::loop, this))
    , _apIp(192, 168, 4, 1)
    , _apNetmask(255, 255, 255, 0)
    , _dnsServer(std::make_unique<DNSServer>())
{
}

void NetworkSettingsClass::init(Scheduler& scheduler)
{
    using std::placeholders::_1;
    using std::placeholders::_2;

    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

    WiFi.disconnect(true, true);

    WiFi.onEvent(std::bind(&NetworkSettingsClass::NetworkEvent, this, _1, _2));

    if (PinMapping.isValidW5500Config()) {
        const PinMapping_t& pin = PinMapping.get();
        _w5500 = W5500::setup(pin.w5500_mosi, pin.w5500_miso, pin.w5500_sclk, pin.w5500_cs, pin.w5500_int, pin.w5500_rst);
        if (_w5500)
            ESP_LOGI(TAG, "W5500: Connection successful");
        else
            ESP_LOGE(TAG, "W5500: Connection error!!");
    }
#if CONFIG_ETH_USE_ESP32_EMAC
    else if (PinMapping.isValidEthConfig()) {
        const PinMapping_t& pin = PinMapping.get();
#if ESP_ARDUINO_VERSION_MAJOR < 3
        ETH.begin(pin.eth_phy_addr, pin.eth_power, pin.eth_mdc, pin.eth_mdio, pin.eth_type, pin.eth_clk_mode);
#else
        ETH.begin(pin.eth_type, pin.eth_phy_addr, pin.eth_mdc, pin.eth_mdio, pin.eth_power, pin.eth_clk_mode);
#endif
    }
#endif

    setupMode();

    scheduler.addTask(_loopTask);
    _loopTask.enable();

    Syslog.init(scheduler);
}

void NetworkSettingsClass::NetworkEvent(const WiFiEvent_t event, WiFiEventInfo_t info)
{
    switch (event) {
    case ARDUINO_EVENT_ETH_START:
        ESP_LOGI(TAG, "ETH start");
        if (_networkMode == network_mode::Ethernet) {
            raiseEvent(network_event::NETWORK_START);
        }
        break;
    case ARDUINO_EVENT_ETH_STOP:
        ESP_LOGI(TAG, "ETH stop");
        if (_networkMode == network_mode::Ethernet) {
            raiseEvent(network_event::NETWORK_STOP);
        }
        break;
    case ARDUINO_EVENT_ETH_CONNECTED:
        ESP_LOGI(TAG, "ETH connected");
        _ethConnected = true;
        raiseEvent(network_event::NETWORK_CONNECTED);
        break;
    case ARDUINO_EVENT_ETH_GOT_IP:
        ESP_LOGI(TAG, "ETH got IP: %s", ETH.localIP().toString().c_str());
        if (_networkMode == network_mode::Ethernet) {
            raiseEvent(network_event::NETWORK_GOT_IP);
        }
        break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
        ESP_LOGI(TAG, "ETH disconnected");
        _ethConnected = false;
        if (_networkMode == network_mode::Ethernet) {
            raiseEvent(network_event::NETWORK_DISCONNECTED);
        }
        break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        ESP_LOGI(TAG, "WiFi connected");
        if (_networkMode == network_mode::WiFi) {
            raiseEvent(network_event::NETWORK_CONNECTED);
        }
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        // Reason codes can be found here: https://github.com/espressif/esp-idf/blob/5454d37d496a8c58542eb450467471404c606501/components/esp_wifi/include/esp_wifi_types_generic.h#L79-L141
        ESP_LOGW(TAG, "WiFi disconnected: %" PRIu8 "", info.wifi_sta_disconnected.reason);
        if (_networkMode == network_mode::WiFi) {
            ESP_LOGI(TAG, "Schedule WiFi reconnect");
            _lastReconnectAttempt = millis();
            _connectRequested = true;
            raiseEvent(network_event::NETWORK_DISCONNECTED);
        }
        break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        ESP_LOGI(TAG, "WiFi got ip: %s", WiFi.localIP().toString().c_str());
        if (_networkMode == network_mode::WiFi) {
            raiseEvent(network_event::NETWORK_GOT_IP);
        }
        break;
    default:
        break;
    }
}

bool NetworkSettingsClass::onEvent(DtuNetworkEventCb cbEvent, const network_event event)
{
    if (!cbEvent) {
        return pdFALSE;
    }
    DtuNetworkEventCbList_t newEventHandler;
    newEventHandler.cb = cbEvent;
    newEventHandler.event = event;
    _cbEventList.push_back(newEventHandler);
    return true;
}

void NetworkSettingsClass::raiseEvent(const network_event event)
{
    for (auto& entry : _cbEventList) {
        if (entry.cb) {
            if (entry.event == event || entry.event == network_event::NETWORK_EVENT_MAX) {
                entry.cb(event);
            }
        }
    }
}

void NetworkSettingsClass::handleMDNS()
{
    const bool mdnsEnabled = Configuration.get().Mdns.Enabled;

    // Return if no state change
    if (_lastMdnsEnabled == mdnsEnabled) {
        return;
    }

    _lastMdnsEnabled = mdnsEnabled;
    MDNS.end();

    if (!mdnsEnabled) {
        ESP_LOGI(TAG, "MDNS disabled");
        return;
    }

    ESP_LOGI(TAG, "Starting MDNS responder...");

    if (!MDNS.begin(getHostname())) {
        ESP_LOGE(TAG, "Error setting up MDNS responder!");
        return;
    }

    MDNS.addService("http", "tcp", 80);
    MDNS.addService("opendtu", "tcp", 80);
    MDNS.addServiceTxt("opendtu", "tcp", "git_hash", __COMPILED_GIT_HASH__);

    ESP_LOGI(TAG, "MDNS started");
}

void NetworkSettingsClass::setupMode()
{
    if (_adminEnabled) {
        WiFi.mode(WIFI_AP_STA);
        String ssidString = getApName();
        WiFi.softAPConfig(_apIp, _apIp, _apNetmask);
        WiFi.softAP(ssidString.c_str(), Configuration.get().Security.Password);
        _dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
        _dnsServer->start(DNS_PORT, "*", WiFi.softAPIP());
        _dnsServerStatus = true;
    } else {
        _dnsServerStatus = false;
        _dnsServer->stop();
        if (_networkMode == network_mode::WiFi) {
            WiFi.mode(WIFI_STA);
        } else {
            WiFi.mode(WIFI_MODE_NULL);
        }
    }
}

void NetworkSettingsClass::enableAdminMode()
{
    // This prevents a immediate "Disabling search for AP" when
    // the network connection persists for a long time and the
    // credentials gets changed.
    _connectTimeoutTimer = 0;
    _connectRedoTimer = 0;

    _adminTimeoutCounter = 0;
    _adminTimeoutCounterMax = Configuration.get().WiFi.ApTimeout * 60;
    _adminEnabled = true;
    setupMode();
}

void NetworkSettingsClass::disableAdminMode()
{
    _adminEnabled = false;
    ESP_LOGI(TAG, "Admin mode disabled");
    setupMode();
}

bool NetworkSettingsClass::wifiConfigured() const
{
    // Check if SSID is empty
    return strcmp(Configuration.get().WiFi.Ssid, "");
}

String NetworkSettingsClass::getApName() const
{
    return String(ACCESS_POINT_NAME + String(Utils::getChipId()));
}

void NetworkSettingsClass::loop()
{
    if (_ethConnected) {
        if (_networkMode != network_mode::Ethernet) {
            // Do stuff when switching to Ethernet mode
            ESP_LOGI(TAG, "Switch to Ethernet mode");
            _networkMode = network_mode::Ethernet;
            WiFi.mode(WIFI_MODE_NULL);
            setStaticIp();
            setHostname();
        }
    } else if (_networkMode != network_mode::WiFi) {
        // Do stuff when switching to Ethernet mode
        ESP_LOGI(TAG, "Switch to WiFi mode");
        _networkMode = network_mode::WiFi;
        enableAdminMode();
        applyConfig();
    }

    if (_connectRequested && isConnected()) {
        _connectRequested = false;
    }
    if (_connectRequested && _networkMode == network_mode::WiFi && _performConnection && wifiConfigured()) {
        ESP_LOGI(TAG, "Handling scheduled WiFi reconnect");
        _connectRequested = false;
        connectToConfiguredWifi();
    }

    if (millis() - _lastTimerCall > 1000) {
        if (_adminEnabled && _adminTimeoutCounterMax > 0) {
            _adminTimeoutCounter++;
            if (_adminTimeoutCounter % 10 == 0) {
                ESP_LOGI(TAG, "Admin AP remaining seconds: %" PRIu32 " / %" PRIu32 "", _adminTimeoutCounter, _adminTimeoutCounterMax);
            }
        }
        if (_performConnection && !isConnected() && wifiConfigured() && millis() - _lastReconnectAttempt > 60000) {
            ESP_LOGW(TAG, "Wifi reconnect watchdog triggered... Resetting Wifi hardware");
            WiFi.disconnect(true, false);
            WiFi.mode(WIFI_MODE_NULL);
            if (_adminEnabled) {
                // Call enableAdminMode to reset all the timeout values.
                // Otherwise the search for AP gets disabled immediatly after wifi reset.
                enableAdminMode();
            }
            applyConfig();
            _lastReconnectAttempt = millis(); // Just in case if the reconnect method gets not triggered
        }
        maintainWifiApSelection();
        _connectTimeoutTimer++;
        _connectRedoTimer++;
        _lastTimerCall = millis();
    }
    if (_adminEnabled) {
        // Don't disable the admin mode when network is not available
        if (!isConnected()) {
            _adminTimeoutCounter = 0;
        }
        // If WiFi is connected to AP for more than adminTimeoutCounterMax
        // seconds, disable the internal Access Point
        if (_adminTimeoutCounter > _adminTimeoutCounterMax) {
            disableAdminMode();
        }
        // It's nearly not possible to use the internal AP if the
        // WiFi is searching for an AP. So disable searching afer
        // WIFI_RECONNECT_TIMEOUT and repeat after WIFI_RECONNECT_REDO_TIMEOUT
        if (isConnected()) {
            _connectTimeoutTimer = 0;
            _connectRedoTimer = 0;
        } else {
            if (_connectTimeoutTimer > WIFI_RECONNECT_TIMEOUT && _performConnection) {
                ESP_LOGI(TAG, "Disabling search for AP...");
                WiFi.mode(WIFI_AP);
                _connectRedoTimer = 0;
                _performConnection = false;
            }
            if (_connectRedoTimer > WIFI_RECONNECT_REDO_TIMEOUT && !_performConnection) {
                ESP_LOGI(TAG, "Enable search for AP...");
                WiFi.mode(WIFI_AP_STA);
                applyConfig();
                _connectTimeoutTimer = 0;
                _performConnection = true;
            }
        }
    }
    if (_dnsServerStatus) {
        _dnsServer->processNextRequest();
    }

    handleMDNS();
}

void NetworkSettingsClass::applyConfig()
{
    setHostname();

    const auto& config = Configuration.get().WiFi;

    if (!wifiConfigured()) {
        return;
    }

    ESP_LOGI(TAG, "Start configuring WiFi STA using %s credentials",
        (strcmp(WiFi.SSID().c_str(), config.Ssid) || strcmp(WiFi.psk().c_str(), config.Password)) ? "new" : "existing");

    bool success = connectToConfiguredWifi();

    ESP_LOG_LEVEL_LOCAL((success ? ESP_LOG_INFO : ESP_LOG_ERROR), TAG, "Configuring WiFi %s", success ? "done" : "failed");

    setStaticIp();

    Syslog.updateSettings(getHostname());
}

bool NetworkSettingsClass::connectToConfiguredWifi()
{
    const auto& config = Configuration.get().WiFi;

    if (!wifiConfigured()) {
        return false;
    }

    if (connectToPreferredWifiAp()) {
        return true;
    }

    WifiApCandidate best;
    scanStrongestAp(config.Ssid, best);

    bool success = false;
    if (best.found) {
        ESP_LOGI(TAG, "Connecting to strongest WiFi AP %s on channel %" PRId32 " (%" PRId32 " dBm)",
            best.bssidString.c_str(), best.channel, best.rssi);
        success = WiFi.begin(config.Ssid, config.Password, best.channel, best.bssid) != WL_CONNECT_FAILED;
        if (success) {
            rememberPreferredWifiAp(best.bssid, best.bssidString, best.channel, best.rssi);
        }
    } else {
        ESP_LOGW(TAG, "Using default WiFi connect for SSID '%s'", config.Ssid);
        success = WiFi.begin(config.Ssid, config.Password) != WL_CONNECT_FAILED;
    }
    WiFi.scanDelete();

    return success;
}

bool NetworkSettingsClass::connectToPreferredWifiAp()
{
    const auto& config = Configuration.get().WiFi;
    if (config.PreferredApBssid[0] == '\0' || config.PreferredApChannel == 0) {
        return false;
    }

    uint8_t bssid[6];
    if (!parseBssid(config.PreferredApBssid, bssid)) {
        ESP_LOGW(TAG, "Configured preferred WiFi AP BSSID '%s' is invalid", config.PreferredApBssid);
        return false;
    }

    ESP_LOGI(TAG, "Connecting to preferred WiFi AP %s on channel %" PRIu8 " (last RSSI %" PRId8 " dBm)",
        config.PreferredApBssid, config.PreferredApChannel, config.PreferredApRssi);
    return WiFi.begin(config.Ssid, config.Password, config.PreferredApChannel, bssid) != WL_CONNECT_FAILED;
}

void NetworkSettingsClass::rememberPreferredWifiAp(const uint8_t* bssid, const String& bssidString, const int32_t channel, const int32_t rssi)
{
    if (!bssid || channel <= 0 || rssi <= PreferredApMinRssi) {
        return;
    }

    bool changed = false;
    {
        auto guard = Configuration.getWriteGuard();
        auto& wifi = guard.getConfig().WiFi;

        if (strcmp(wifi.PreferredApBssid, bssidString.c_str()) == 0 && wifi.PreferredApChannel == channel) {
            return;
        }

        strlcpy(wifi.PreferredApBssid, bssidString.c_str(), sizeof(wifi.PreferredApBssid));
        wifi.PreferredApChannel = static_cast<uint8_t>(channel);
        wifi.PreferredApRssi = static_cast<int8_t>(rssi);
        changed = true;
    }

    if (!changed) {
        return;
    }

    if (Configuration.write()) {
        ESP_LOGI(TAG, "Stored preferred WiFi AP %s on channel %" PRId32 " (%" PRId32 " dBm)",
            bssidString.c_str(), channel, rssi);
    } else {
        ESP_LOGE(TAG, "Failed to store preferred WiFi AP %s", bssidString.c_str());
    }
}

void NetworkSettingsClass::rememberCurrentWifiApIfUseful()
{
    const int32_t currentRssi = WiFi.RSSI();
    if (currentRssi <= PreferredApMinRssi) {
        return;
    }

    const auto* bssid = WiFi.BSSID();
    if (!bssid) {
        return;
    }

    rememberPreferredWifiAp(bssid, formatBssid(bssid), WiFi.channel(), currentRssi);
}

void NetworkSettingsClass::maintainWifiApSelection()
{

    if (_networkMode != network_mode::WiFi || !_performConnection || !isConnected() || !wifiConfigured()) {
        return;
    }

    if (millis() - _lastApSelectionCheck < ApSelectionCheckIntervalMillis) {
        return;
    }
    _lastApSelectionCheck = millis();

    const int32_t currentRssi = WiFi.RSSI();
    if (currentRssi > PreferredApMinRssi) {
        rememberCurrentWifiApIfUseful();
        return;
    }

    const auto& config = Configuration.get().WiFi;
    const String currentBssid = WiFi.BSSIDstr();
    WifiApCandidate best;
    if (!scanStrongestAp(config.Ssid, best)) {
        WiFi.scanDelete();
        return;
    }

    const bool betterAp = best.bssidString != currentBssid && best.rssi >= currentRssi + MinReselectGain;
    if (betterAp) {
        ESP_LOGW(TAG, "Switching WiFi AP from %s (%" PRId32 " dBm) to %s (%" PRId32 " dBm)",
            currentBssid.c_str(), currentRssi, best.bssidString.c_str(), best.rssi);
        rememberPreferredWifiAp(best.bssid, best.bssidString, best.channel, best.rssi);
        WiFi.begin(config.Ssid, config.Password, best.channel, best.bssid);
    }
    WiFi.scanDelete();
}

void NetworkSettingsClass::setHostname()
{
    if (_networkMode == network_mode::Undefined) {
        return;
    }

    const String hostname = getHostname();
    bool success = false;

    ESP_LOGI(TAG, "Start setting hostname...");
    if (_networkMode == network_mode::WiFi) {
        success = WiFi.hostname(hostname);

        // Evil bad hack to get the hostname set up correctly
        WiFi.mode(WIFI_MODE_APSTA);
        WiFi.mode(WIFI_MODE_STA);
        setupMode();
    } else if (_networkMode == network_mode::Ethernet) {
        success = ETH.setHostname(hostname.c_str());
    }

    ESP_LOG_LEVEL_LOCAL((success ? ESP_LOG_INFO : ESP_LOG_ERROR), TAG, "Setting hostname %s", success ? "done" : "failed");
}

void NetworkSettingsClass::setStaticIp()
{
    if (_networkMode == network_mode::Undefined) {
        return;
    }

    const auto& config = Configuration.get().WiFi;
    const char* mode = (_networkMode == network_mode::WiFi) ? "WiFi" : "Ethernet";
    const char* ipType = config.Dhcp ? "DHCP" : "static";

    ESP_LOGI(TAG, "Start configuring %s %s IP...", mode, ipType);

    bool success = false;
    if (_networkMode == network_mode::WiFi) {
        if (config.Dhcp) {
            success = WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
        } else {
            success = WiFi.config(
                IPAddress(config.Ip),
                IPAddress(config.Gateway),
                IPAddress(config.Netmask),
                IPAddress(config.Dns1),
                IPAddress(config.Dns2));
        }
    } else if (_networkMode == network_mode::Ethernet) {
        if (config.Dhcp) {
            success = ETH.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
        } else {
            success = ETH.config(
                IPAddress(config.Ip),
                IPAddress(config.Gateway),
                IPAddress(config.Netmask),
                IPAddress(config.Dns1),
                IPAddress(config.Dns2));
        }
    }

    ESP_LOG_LEVEL_LOCAL((success ? ESP_LOG_INFO : ESP_LOG_ERROR), TAG, "Configure IP %s", success ? "done" : "failed");
}

IPAddress NetworkSettingsClass::localIP() const
{
    switch (_networkMode) {
    case network_mode::Ethernet:
        return ETH.localIP();
        break;
    case network_mode::WiFi:
        return WiFi.localIP();
        break;
    default:
        return INADDR_NONE;
    }
}

IPAddress NetworkSettingsClass::subnetMask() const
{
    switch (_networkMode) {
    case network_mode::Ethernet:
        return ETH.subnetMask();
        break;
    case network_mode::WiFi:
        return WiFi.subnetMask();
        break;
    default:
        return IPAddress(255, 255, 255, 0);
    }
}

IPAddress NetworkSettingsClass::gatewayIP() const
{
    switch (_networkMode) {
    case network_mode::Ethernet:
        return ETH.gatewayIP();
        break;
    case network_mode::WiFi:
        return WiFi.gatewayIP();
        break;
    default:
        return INADDR_NONE;
    }
}

IPAddress NetworkSettingsClass::dnsIP(const uint8_t dns_no) const
{
    switch (_networkMode) {
    case network_mode::Ethernet:
        return ETH.dnsIP(dns_no);
        break;
    case network_mode::WiFi:
        return WiFi.dnsIP(dns_no);
        break;
    default:
        return INADDR_NONE;
    }
}

String NetworkSettingsClass::macAddress() const
{
    switch (_networkMode) {
    case network_mode::Ethernet:
        if (_w5500) {
            return _w5500->macAddress();
        }
        return ETH.macAddress();
        break;
    case network_mode::WiFi:
        return WiFi.macAddress();
        break;
    default:
        return "";
    }
}

String NetworkSettingsClass::getHostname()
{
    const CONFIG_T& config = Configuration.get();
    char preparedHostname[WIFI_MAX_HOSTNAME_STRLEN + 1];
    char resultHostname[WIFI_MAX_HOSTNAME_STRLEN + 1];
    uint8_t pos = 0;

    const uint32_t chipId = Utils::getChipId();
    snprintf(preparedHostname, WIFI_MAX_HOSTNAME_STRLEN + 1, config.WiFi.Hostname, chipId);

    const char* pC = preparedHostname;
    while (*pC && pos < WIFI_MAX_HOSTNAME_STRLEN) { // while !null and not over length
        if (isalnum(*pC)) { // if the current char is alpha-numeric append it to the hostname
            resultHostname[pos] = *pC;
            pos++;
        } else if (*pC == ' ' || *pC == '_' || *pC == '-' || *pC == '+' || *pC == '!' || *pC == '?' || *pC == '*') {
            resultHostname[pos] = '-';
            pos++;
        }
        // else do nothing - no leading hyphens and do not include hyphens for all other characters.
        pC++;
    }

    resultHostname[pos] = '\0'; // terminate string

    // last character must not be hyphen
    while (pos > 0 && resultHostname[pos - 1] == '-') {
        resultHostname[pos - 1] = '\0';
        pos--;
    }

    // Fallback if no other rule applied
    if (strlen(resultHostname) == 0) {
        snprintf(resultHostname, WIFI_MAX_HOSTNAME_STRLEN + 1, APP_HOSTNAME, chipId);
    }

    return resultHostname;
}

bool NetworkSettingsClass::isConnected() const
{
    return (WiFi.localIP()[0] != 0 && WiFi.isConnected() ) || ETH.localIP()[0] != 0;
}

network_mode NetworkSettingsClass::NetworkMode() const
{
    return _networkMode;
}

NetworkSettingsClass NetworkSettings;
