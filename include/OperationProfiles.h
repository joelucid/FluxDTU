// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <ArduinoJson.h>
#include <WString.h>

#define OPERATION_PROFILES_FILENAME "/operation_profiles.json"

class OperationProfilesClass {
public:
    void init();

    bool serializeStatus(JsonObject& target, String& error);
    bool createFromCurrentConfig(String const& name, String& createdId, String& error);
    bool applyProfile(String const& id, String& error);
    bool syncActiveProfileFromCurrentConfig(String& error);
    bool renameProfile(String const& id, String const& name, String& error);
    bool deleteProfile(String const& id, String& error);

    static constexpr size_t MaxNameLength = 31;

private:
    bool ensureStore(String& error);
    bool loadStore(JsonDocument& doc, String& error);
    bool writeStore(JsonDocument& doc, String& error);
    bool createDefaultStore(JsonDocument& doc, String& error);
    bool validateStore(JsonDocument& doc);
    bool backupBadStore();
    bool ensureValidActiveProfile(JsonDocument& doc, String& error);

    void addProfileFromCurrentConfig(JsonArray& profiles, String const& id, String const& name);
    void serializeCurrentSettings(JsonObject& target);
    void serializeGridCharger(JsonObject& target);
    bool applyProfileSettings(JsonObject profile, String& error);
    void notifySettingsChanged();

    JsonObject findProfile(JsonDocument& doc, String const& id);
    JsonObject findActiveProfile(JsonDocument& doc);
    bool profileExists(JsonDocument& doc, String const& id);
    bool isCurrentConfigEqualToProfile(JsonObject profile);

    String normalizedName(String const& name) const;
    String createProfileId(JsonDocument& doc, String const& name);
};

extern OperationProfilesClass OperationProfiles;
