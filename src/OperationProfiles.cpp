// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Thomas Basler and others
 */
#include "OperationProfiles.h"
#include "Configuration.h"
#include "FlexibleLoadStats.h"
#include "MqttHandlePowerLimiterHass.h"
#include "PowerLimiter.h"
#include <gridcharger/Controller.h>
#include <algorithm>
#include <LittleFS.h>

#undef TAG
static const char* TAG = "operation_profiles";

namespace {
static constexpr char const* TemporaryFilename = "/operation_profiles.tmp";
static constexpr char const* BadFilename = "/operation_profiles.bad.json";
static constexpr char const* DefaultProfileId = "standard";
static constexpr char const* DefaultProfileName = "Standard";
static constexpr size_t MaxIdLength = 31;

String serializeJsonObject(JsonObjectConst const& object)
{
    String serialized;
    serializeJson(object, serialized);
    return serialized;
}
}

void OperationProfilesClass::init()
{
    String error;
    if (!ensureStore(error)) {
        ESP_LOGW(TAG, "Failed to initialize operation profiles: %s", error.c_str());
    }
}

bool OperationProfilesClass::serializeStatus(JsonObject& target, String& error)
{
    JsonDocument doc;
    if (!loadStore(doc, error)) { return false; }

    JsonObject activeProfile = findActiveProfile(doc);

    target["version"] = doc["version"] | 1;
    target["active_profile_id"] = doc["active_profile_id"] | "";
    target["active_profile_name"] = activeProfile["name"] | "";
    target["modified"] = !activeProfile.isNull() && !isCurrentConfigEqualToProfile(activeProfile);

    JsonArray targetProfiles = target["profiles"].to<JsonArray>();
    String const activeId = doc["active_profile_id"] | "";
    JsonArray profiles = doc["profiles"].as<JsonArray>();
    for (JsonObject profile : profiles) {
        JsonObject targetProfile = targetProfiles.add<JsonObject>();
        String const id = profile["id"] | "";
        targetProfile["id"] = id;
        targetProfile["name"] = profile["name"] | "";
        targetProfile["active"] = id == activeId;
    }

    return true;
}

bool OperationProfilesClass::createFromCurrentConfig(String const& name, String& createdId, String& error)
{
    JsonDocument doc;
    if (!loadStore(doc, error)) { return false; }

    String const normalized = normalizedName(name);
    if (normalized.isEmpty()) {
        error = "Profile name is required";
        return false;
    }
    if (normalized.length() > MaxNameLength) {
        error = "Profile name is too long";
        return false;
    }

    createdId = createProfileId(doc, normalized);
    JsonArray profiles = doc["profiles"].as<JsonArray>();
    addProfileFromCurrentConfig(profiles, createdId, normalized);
    doc["active_profile_id"] = createdId;

    return writeStore(doc, error);
}

bool OperationProfilesClass::applyProfile(String const& id, String& error)
{
    JsonDocument doc;
    if (!loadStore(doc, error)) { return false; }

    JsonObject profile = findProfile(doc, id);
    if (profile.isNull()) {
        error = "Profile not found";
        return false;
    }

    if (!applyProfileSettings(profile, error)) { return false; }

    doc["active_profile_id"] = id;
    if (!writeStore(doc, error)) { return false; }

    notifySettingsChanged();
    return true;
}

bool OperationProfilesClass::syncActiveProfileFromCurrentConfig(String& error)
{
    JsonDocument doc;
    if (!loadStore(doc, error)) { return false; }

    String const profileId = doc["active_profile_id"] | "";

    JsonObject profile = findProfile(doc, profileId);
    if (profile.isNull()) {
        error = "Profile not found";
        return false;
    }

    profile.remove("powerlimiter");
    profile.remove("gridcharger");
    serializeCurrentSettings(profile);
    doc["active_profile_id"] = profileId;

    return writeStore(doc, error);
}

bool OperationProfilesClass::renameProfile(String const& id, String const& name, String& error)
{
    JsonDocument doc;
    if (!loadStore(doc, error)) { return false; }

    JsonObject profile = findProfile(doc, id);
    if (profile.isNull()) {
        error = "Profile not found";
        return false;
    }

    String const normalized = normalizedName(name);
    if (normalized.isEmpty()) {
        error = "Profile name is required";
        return false;
    }
    if (normalized.length() > MaxNameLength) {
        error = "Profile name is too long";
        return false;
    }

    profile["name"] = normalized;
    return writeStore(doc, error);
}

bool OperationProfilesClass::deleteProfile(String const& id, String& error)
{
    JsonDocument doc;
    if (!loadStore(doc, error)) { return false; }

    JsonArray profiles = doc["profiles"].as<JsonArray>();
    if (profiles.size() <= 1) {
        error = "The last profile cannot be deleted";
        return false;
    }

    bool removed = false;
    for (size_t i = 0; i < profiles.size(); ++i) {
        JsonObject profile = profiles[i];
        String const profileId = profile["id"] | "";
        if (profileId == id) {
            profiles.remove(i);
            removed = true;
            break;
        }
    }

    if (!removed) {
        error = "Profile not found";
        return false;
    }

    if (!profileExists(doc, doc["active_profile_id"] | "")) {
        JsonObject firstProfile = profiles[0];
        doc["active_profile_id"] = firstProfile["id"] | "";
    }

    return writeStore(doc, error);
}

bool OperationProfilesClass::ensureStore(String& error)
{
    JsonDocument doc;
    return loadStore(doc, error);
}

bool OperationProfilesClass::loadStore(JsonDocument& doc, String& error)
{
    if (!LittleFS.exists(OPERATION_PROFILES_FILENAME)) {
        if (!createDefaultStore(doc, error)) { return false; }
        return writeStore(doc, error);
    }

    File file = LittleFS.open(OPERATION_PROFILES_FILENAME, "r");
    if (!file) {
        error = "Failed to open operation profiles";
        return false;
    }

    DeserializationError const jsonError = deserializeJson(doc, file);
    file.close();
    if (jsonError || !validateStore(doc)) {
        ESP_LOGW(TAG, "Operation profile store is invalid, creating a new default profile from current config");
        backupBadStore();
        doc.clear();
        if (!createDefaultStore(doc, error)) { return false; }
        return writeStore(doc, error);
    }

    return ensureValidActiveProfile(doc, error);
}

bool OperationProfilesClass::writeStore(JsonDocument& doc, String& error)
{
    File file = LittleFS.open(TemporaryFilename, "w");
    if (!file) {
        error = "Failed to open temporary profile file";
        return false;
    }

    if (serializeJson(doc, file) == 0) {
        file.close();
        LittleFS.remove(TemporaryFilename);
        error = "Failed to serialize operation profiles";
        return false;
    }
    file.close();

    if (LittleFS.exists(OPERATION_PROFILES_FILENAME)) {
        LittleFS.remove(OPERATION_PROFILES_FILENAME);
    }

    if (!LittleFS.rename(TemporaryFilename, OPERATION_PROFILES_FILENAME)) {
        LittleFS.remove(TemporaryFilename);
        error = "Failed to save operation profiles";
        return false;
    }

    return true;
}

bool OperationProfilesClass::createDefaultStore(JsonDocument& doc, String& error)
{
    doc.clear();
    doc["version"] = 1;
    doc["active_profile_id"] = DefaultProfileId;

    JsonArray profiles = doc["profiles"].to<JsonArray>();
    addProfileFromCurrentConfig(profiles, DefaultProfileId, DefaultProfileName);

    if (profiles.size() == 0) {
        error = "Failed to create default profile";
        return false;
    }

    return true;
}

bool OperationProfilesClass::validateStore(JsonDocument& doc)
{
    if (!doc["profiles"].is<JsonArray>()) { return false; }
    JsonArray profiles = doc["profiles"].as<JsonArray>();
    if (profiles.size() == 0) { return false; }

    for (JsonObject profile : profiles) {
        if (!profile["id"].is<const char*>()) { return false; }
        if (!profile["name"].is<const char*>()) { return false; }
        if (!profile["powerlimiter"].is<JsonObject>()) { return false; }
        if (!profile["gridcharger"].is<JsonObject>()) { return false; }
    }

    return true;
}

bool OperationProfilesClass::backupBadStore()
{
    if (!LittleFS.exists(OPERATION_PROFILES_FILENAME)) { return true; }
    if (LittleFS.exists(BadFilename)) {
        LittleFS.remove(BadFilename);
    }
    return LittleFS.rename(OPERATION_PROFILES_FILENAME, BadFilename);
}

bool OperationProfilesClass::ensureValidActiveProfile(JsonDocument& doc, String& error)
{
    JsonArray profiles = doc["profiles"].as<JsonArray>();
    String activeId = doc["active_profile_id"] | "";

    if (profileExists(doc, activeId)) { return true; }

    JsonObject firstProfile = profiles[0];
    doc["active_profile_id"] = firstProfile["id"] | "";
    return writeStore(doc, error);
}

void OperationProfilesClass::addProfileFromCurrentConfig(JsonArray& profiles, String const& id, String const& name)
{
    JsonObject profile = profiles.add<JsonObject>();
    profile["id"] = id;
    profile["name"] = name;
    serializeCurrentSettings(profile);
}

void OperationProfilesClass::serializeCurrentSettings(JsonObject& target)
{
    auto const& config = Configuration.get();

    JsonObject powerlimiter = target["powerlimiter"].to<JsonObject>();
    ConfigurationClass::serializePowerLimiterConfig(config.PowerLimiter, powerlimiter);

    JsonObject gridcharger = target["gridcharger"].to<JsonObject>();
    serializeGridCharger(gridcharger);
}

void OperationProfilesClass::serializeGridCharger(JsonObject& target)
{
    auto const& config = Configuration.get();

    ConfigurationClass::serializeGridChargerConfig(config.GridCharger, target);

    JsonObject can = target["can"].to<JsonObject>();
    ConfigurationClass::serializeGridChargerCanConfig(config.GridCharger.Can, can);

    JsonObject huawei = target["huawei"].to<JsonObject>();
    ConfigurationClass::serializeGridChargerHuaweiConfig(config.GridCharger.Huawei, huawei);

    JsonObject trucki = target["trucki"].to<JsonObject>();
    ConfigurationClass::serializeGridChargerTruckiConfig(config.GridCharger.Trucki, trucki);
}

bool OperationProfilesClass::applyProfileSettings(JsonObject profile, String& error)
{
    JsonObject powerlimiter = profile["powerlimiter"].as<JsonObject>();
    JsonObject gridcharger = profile["gridcharger"].as<JsonObject>();
    if (powerlimiter.isNull() || gridcharger.isNull()) {
        error = "Profile is incomplete";
        return false;
    }

    {
        auto guard = Configuration.getWriteGuard();
        auto& config = guard.getConfig();

        ConfigurationClass::deserializePowerLimiterConfig(powerlimiter, config.PowerLimiter);
        ConfigurationClass::deserializeGridChargerConfig(gridcharger, config.GridCharger);
        ConfigurationClass::deserializeGridChargerCanConfig(gridcharger["can"].as<JsonObject>(), config.GridCharger.Can);
        ConfigurationClass::deserializeGridChargerHuaweiConfig(gridcharger["huawei"].as<JsonObject>(), config.GridCharger.Huawei);
        ConfigurationClass::deserializeGridChargerTruckiConfig(gridcharger["trucki"].as<JsonObject>(), config.GridCharger.Trucki);
    }

    if (!Configuration.write()) {
        error = "Failed to write configuration";
        return false;
    }

    return true;
}

void OperationProfilesClass::notifySettingsChanged()
{
    PowerLimiter.triggerReloadingConfig();
    FlexibleLoadStats.updateSettings();
    MqttHandlePowerLimiterHass.forceUpdate();
    GridCharger.updateSettings();
}

JsonObject OperationProfilesClass::findProfile(JsonDocument& doc, String const& id)
{
    JsonArray profiles = doc["profiles"].as<JsonArray>();
    for (JsonObject profile : profiles) {
        String const profileId = profile["id"] | "";
        if (profileId == id) { return profile; }
    }

    return JsonObject();
}

JsonObject OperationProfilesClass::findActiveProfile(JsonDocument& doc)
{
    return findProfile(doc, doc["active_profile_id"] | "");
}

bool OperationProfilesClass::profileExists(JsonDocument& doc, String const& id)
{
    return !findProfile(doc, id).isNull();
}

bool OperationProfilesClass::isCurrentConfigEqualToProfile(JsonObject profile)
{
    JsonDocument currentDoc;
    JsonObject current = currentDoc.to<JsonObject>();
    serializeCurrentSettings(current);

    JsonDocument profileDoc;
    JsonObject profileSettings = profileDoc.to<JsonObject>();
    profileSettings["powerlimiter"].set(profile["powerlimiter"]);
    profileSettings["gridcharger"].set(profile["gridcharger"]);

    return serializeJsonObject(current) == serializeJsonObject(profileSettings);
}

String OperationProfilesClass::normalizedName(String const& name) const
{
    String normalized = name;
    normalized.trim();
    return normalized;
}

String OperationProfilesClass::createProfileId(JsonDocument& doc, String const& name)
{
    String base;
    bool pendingSeparator = false;
    for (size_t i = 0; i < name.length() && base.length() < MaxIdLength; ++i) {
        char c = name.charAt(i);
        if (c >= 'A' && c <= 'Z') {
            c = c - 'A' + 'a';
        }

        bool const isLetter = c >= 'a' && c <= 'z';
        bool const isDigit = c >= '0' && c <= '9';
        if (isLetter || isDigit) {
            if (pendingSeparator && !base.isEmpty() && base.length() < MaxIdLength) {
                base += '-';
            }
            base += c;
            pendingSeparator = false;
        } else {
            pendingSeparator = !base.isEmpty();
        }
    }

    if (base.isEmpty()) {
        base = "profile";
    }

    String candidate = base;
    uint8_t suffix = 2;
    while (profileExists(doc, candidate)) {
        String const suffixString = "-" + String(suffix);
        size_t const baseLength = std::min(MaxIdLength - suffixString.length(), base.length());
        candidate = base.substring(0, baseLength) + suffixString;
        ++suffix;
    }

    return candidate;
}

OperationProfilesClass OperationProfiles;
