// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "Configuration.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <TaskSchedulerDeclarations.h>
#include <functional>
#include <mutex>
#include <vector>

class StatisticsClass {
public:
    void init(Scheduler& scheduler);
    void getStatus(
            JsonVariant& root,
            String const& period,
            bool compactSamples = false,
            uint32_t requestedFrom = 0,
            String const& view = "flow") const;

    static constexpr uint32_t SampleIntervalSeconds = 5 * 60;
    static constexpr uint16_t DesiredSampleRetentionDays = 90;
    static constexpr uint16_t DailyRetentionDays = 366;

private:
    static constexpr uint8_t PanelCount = INV_MAX_COUNT * INV_MAX_CHAN_COUNT;
    static_assert(PanelCount <= 64, "Panel statistics flags use a 64-bit bitset");

    enum SampleFlags : uint16_t {
        HasBatteryDischargePower = 1 << 0,
        HasSolarPower = 1 << 1,
        HasBatteryPower = 1 << 2,
        HasBatterySoc = 1 << 3,
        HasBatteryVoltage = 1 << 4,
        HasBatteryTemperature = 1 << 5,
        HasGridPower = 1 << 6,
        HasGridChargerPower = 1 << 7,
        HasTargetPower = 1 << 8,
        HasIntegratedPower = 1 << 9,
        HasInverterTemperatures = 1 << 14,
        HasBatteryBoostSavings = 1 << 15,
    };

    static constexpr uint16_t FlexibleLoadPowerFlagBase = 1 << 10;

    struct Sample {
        uint32_t timestamp = 0;
        int16_t batteryDischargePowerWatts = 0;
        int16_t solarPowerWatts = 0;
        int16_t batteryPowerWatts = 0;
        int16_t gridPowerWatts = 0;
        int16_t gridChargerPowerWatts = 0;
        int16_t targetPowerWatts = 0;
        int16_t flexibleLoadPowerWatts[POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT] = {};
        // These two slots used to store unused yield-day values in v1. Reusing
        // them keeps the v2 import/export representation compact.
        uint16_t gridImportPowerWatts = 0;
        uint16_t gridExportPowerWatts = 0;
        uint16_t batterySocPermille = 0;
        uint16_t batteryVoltageDecivolt = 0;
        int16_t batteryTemperatureDecicelsius = 0;
        int16_t inverterTemperatureDecicelsius[INV_MAX_COUNT] = {};
        uint16_t inverterTemperatureFlags = 0;
        uint16_t batteryBoostSavingsNormalPowerWatts = 0;
        uint16_t batteryBoostSavingsRelaxedPowerWatts = 0;
        uint16_t batteryBoostRelaxLimitedPowerWatts = 0;
        uint16_t batteryBoostSeconds = 0;
        uint16_t batteryBoostBudgetUsedPermille = 0;
        uint16_t flags = 0;
    };

    struct EnergySummary {
        uint32_t from = 0;
        uint32_t to = 0;
        uint16_t sampleCount = 0;
        uint16_t intervalCount = 0;
        float solarEnergyWh = 0;
        float batteryChargeWh = 0;
        float batteryDischargeWh = 0;
        float gridImportWh = 0;
        float gridExportWh = 0;
        float gridChargerEnergyWh = 0;
        float flexibleLoadEnergyWh[POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT] = {};
        float targetDeviationWh = 0;
        float minBatterySoc = 101.0f;
        float maxBatterySoc = -1.0f;
        float minBatteryVoltage = 100000.0f;
        float maxBatteryVoltage = -1.0f;
        float minBatteryTemperature = 100000.0f;
        float maxBatteryTemperature = -100000.0f;
        int16_t maxGridImportWatts = 0;
        int16_t maxGridExportWatts = 0;
        int16_t maxBatteryChargeWatts = 0;
        int16_t maxBatteryDischargeWatts = 0;
        float batterySocPercentSeconds = 0;
        float batterySocSeconds = 0;
        float batteryBoostSavingsNormalWh = 0;
        float batteryBoostSavingsRelaxedWh = 0;
        float batteryBoostRelaxLimitedWh = 0;
        float batteryBoostSeconds = 0;
    };

    struct DailySummary : EnergySummary {
        uint32_t dayStart = 0;
    };

    struct Bucket {
        uint32_t timestamp = 0;
        int32_t batteryDischargePowerSum = 0;
        int32_t solarPowerSum = 0;
        int32_t batteryPowerSum = 0;
        int32_t gridPowerSum = 0;
        uint32_t gridImportPowerSum = 0;
        uint32_t gridExportPowerSum = 0;
        int32_t gridChargerPowerSum = 0;
        int32_t targetPowerSum = 0;
        int32_t flexibleLoadPowerSum[POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT] = {};
        uint32_t batteryBoostSavingsNormalPowerSum = 0;
        uint32_t batteryBoostSavingsRelaxedPowerSum = 0;
        uint32_t batteryBoostRelaxLimitedPowerSum = 0;
        uint32_t batteryBoostSecondsSum = 0;
        uint32_t batteryBoostBudgetUsedPermilleSum = 0;
        uint32_t batterySocPermilleSum = 0;
        int32_t inverterTemperatureDecicelsiusSum[INV_MAX_COUNT] = {};
        uint16_t sampleCount = 0;
        uint16_t batteryDischargePowerCount = 0;
        uint16_t solarPowerCount = 0;
        uint16_t batteryPowerCount = 0;
        uint16_t gridPowerCount = 0;
        uint16_t gridChargerPowerCount = 0;
        uint16_t targetPowerCount = 0;
        uint16_t flexibleLoadPowerCount[POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT] = {};
        uint16_t batteryBoostSavingsCount = 0;
        uint16_t batteryBoostBudgetUsedCount = 0;
        uint16_t batterySocCount = 0;
        uint16_t inverterTemperatureCount[INV_MAX_COUNT] = {};
    };

    struct ClassifiedInverterSample {
        float solarPowerWatts = 0;
        bool hasSolar = false;
        float batteryDischargePowerWatts = 0;
        bool hasBatteryDischarge = false;
    };

    struct Accumulator {
        uint32_t lastMillis = 0;
        float batteryDischargePowerWattSeconds = 0;
        float solarPowerWattSeconds = 0;
        float batteryPowerWattSeconds = 0;
        float gridPowerWattSeconds = 0;
        float gridImportPowerWattSeconds = 0;
        float gridExportPowerWattSeconds = 0;
        float gridChargerPowerWattSeconds = 0;
        float targetPowerWattSeconds = 0;
        float flexibleLoadPowerWattSeconds[POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT] = {};
        float batteryBoostSavingsNormalWattSeconds = 0;
        float batteryBoostSavingsRelaxedWattSeconds = 0;
        float batteryBoostRelaxLimitedWattSeconds = 0;
        float batteryBoostSeconds = 0;
        float batterySocPermilleSeconds = 0;
        float batteryVoltageDecivoltSeconds = 0;
        float batteryTemperatureDecicelsiusSeconds = 0;
        float inverterTemperatureDecicelsiusSeconds[INV_MAX_COUNT] = {};
        float windowSeconds = 0;
        float batteryDischargePowerSeconds = 0;
        float solarPowerSeconds = 0;
        float batteryPowerSeconds = 0;
        float gridPowerSeconds = 0;
        float gridChargerPowerSeconds = 0;
        float targetPowerSeconds = 0;
        float flexibleLoadPowerSeconds[POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT] = {};
        float batterySocSeconds = 0;
        float batteryVoltageSeconds = 0;
        float batteryTemperatureSeconds = 0;
        float inverterTemperatureSeconds[INV_MAX_COUNT] = {};
        bool hasBatteryDischargePower = false;
        bool hasSolarPower = false;
        bool hasBatteryPower = false;
        bool hasGridPower = false;
        bool hasGridChargerPower = false;
        bool hasTargetPower = false;
        bool hasFlexibleLoadPower[POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT] = {};
        bool hasBatteryBoostSavings = false;
        uint16_t batteryBoostBudgetUsedPermille = 0;
        bool hasBatterySoc = false;
        bool hasBatteryVoltage = false;
        bool hasBatteryTemperature = false;
        bool hasInverterTemperature[INV_MAX_COUNT] = {};
    };

    struct BatteryBoostSavingsState {
        float budgetUsedSeconds = 0;
    };

    struct SampleFileHeader {
        uint32_t magic = 0;
        uint16_t version = 0;
        uint16_t recordSize = 0;
        uint32_t capacity = 0;
        uint32_t count = 0;
        uint32_t nextIndex = 0;
    };

    struct DailyFileHeader {
        uint32_t magic = 0;
        uint16_t version = 0;
        uint16_t recordSize = 0;
        uint32_t capacity = 0;
    };

    struct PanelSample {
        uint32_t timestamp = 0;
        int16_t panelPowerWatts[PanelCount] = {};
        uint64_t panelPowerFlags = 0;
    };

    struct PanelEnergySummary {
        uint32_t from = 0;
        uint32_t to = 0;
        uint16_t sampleCount = 0;
        uint16_t intervalCount = 0;
        float panelEnergyWh[PanelCount] = {};
    };

    struct PanelDailySummary : PanelEnergySummary {
        uint32_t dayStart = 0;
    };

    struct PanelSampleFileHeader {
        uint32_t magic = 0;
        uint16_t version = 0;
        uint16_t recordSize = 0;
        uint32_t capacity = 0;
        uint32_t count = 0;
        uint32_t nextIndex = 0;
    };

    struct PanelDailyFileHeader {
        uint32_t magic = 0;
        uint16_t version = 0;
        uint16_t recordSize = 0;
        uint32_t capacity = 0;
    };

    struct PanelDescriptor {
        uint8_t inverterIndex = 0;
        uint8_t channel = 0;
        uint8_t storageIndex = 0;
        uint8_t order = 0;
        String serial;
        String inverterName;
        String name;
        bool enabled = false;
        uint16_t maxPower = 0;
    };

    void sampleTaskCb();
    Sample collectInstantaneousSample() const;
    PanelSample collectPanelSample(uint32_t timestamp) const;
    ClassifiedInverterSample collectClassifiedInverterSample() const;
    bool areEnabledInvertersReady() const;
    bool isSampleDue(uint32_t timestamp) const;
    void accumulateCurrent();
    void accumulateSample(Sample const& sample, uint32_t nowMillis);
    void accumulateBatteryBoostSavings(Sample const& sample, float seconds);
    void relaxBatteryBoostBudget(float seconds);
    Sample buildIntegratedSample(uint32_t timestamp) const;
    void resetAccumulator(uint32_t nowMillis = 0);
    void storeSample(Sample const& sample);
    void storePanelSample(PanelSample const& sample);
    void updateDailySummary(Sample const& sample);
    void updatePanelDailySummary(PanelSample const& sample);
    DailySummary getDailySummary(uint32_t dayStart) const;
    void writeDailySummary(DailySummary const& summary) const;
    PanelDailySummary getPanelDailySummary(uint32_t dayStart) const;
    void writePanelDailySummary(PanelDailySummary const& summary) const;

    void initFilesystem();
    void migrateLegacyFiles();
    bool copyFromLegacyIfMissing(char const* path) const;
    fs::LittleFSFS& filesystem() const { return *_fs; }
    uint32_t desiredSampleCapacity() const;
    size_t fileSize(char const* path) const;
    bool initSampleFile();
    bool archiveIncompatibleFile(char const* path) const;
    bool copyFile(char const* sourcePath, char const* targetPath) const;
    bool restoreFileFromArchive(char const* currentPath, char const* tempPath, String const& archivePath) const;
    void recoverSampleFileReplacement() const;
    void recoverArchivedStatisticsFiles();
    bool recoverArchivedSampleFileIfEmpty();
    bool recoverArchivedDailyFileIfEmpty();
    bool readSampleHeaderFrom(char const* path, SampleFileHeader& header) const;
    bool readDailyHeaderFrom(char const* path, DailyFileHeader& header) const;
    bool isRecoverableSampleHeader(SampleFileHeader const& header) const;
    bool isRecoverableDailyHeader(DailyFileHeader const& header) const;
    String findArchivedSampleFile() const;
    String findArchivedDailyFile() const;
    uint32_t countDailyRecords(char const* path, DailyFileHeader const& header) const;
    void migrateV2StatisticsFiles();
    bool migrateV2SampleFile(String const& archivePath);
    bool migrateV2DailyFile(String const& archivePath);
    bool migrateV3SampleFile();
    bool migrateV4SampleFile();
    bool migrateV3DailyFile();
    bool migrateV4DailyFile();
    void backfillDailySocAveragesFromSamples();
    void rebuildDailySummariesFromSamplesIfEmpty();
    String findV2Archive(char const* path, uint32_t magic, uint16_t recordSize) const;
    bool expandSampleFile(SampleFileHeader const& header, uint32_t capacity) const;
    bool initDailyFile();
    uint32_t desiredPanelSampleCapacity() const;
    bool initPanelSampleFile();
    bool initPanelDailyFile();
    bool readSampleHeader(SampleFileHeader& header) const;
    void writeSampleHeader(SampleFileHeader const& header) const;
    bool readPanelSampleHeader(PanelSampleFileHeader& header) const;
    void writePanelSampleHeader(PanelSampleFileHeader const& header) const;
    bool readSampleAt(uint32_t index, Sample& sample) const;
    bool readPanelSampleAt(uint32_t index, PanelSample& sample) const;
    bool writeSampleAt(uint32_t index, Sample const& sample) const;
    bool writePanelSampleAt(uint32_t index, PanelSample const& sample) const;
    bool appendArchivedSample(Sample const& sample) const;
    bool appendArchivedPanelSample(PanelSample const& sample) const;
    void forEachRecentSample(std::function<void(Sample const&)> const& callback) const;
    void forEachRecentSample(
            uint32_t from,
            uint32_t to,
            bool includePrevious,
            std::function<void(Sample const&)> const& callback) const;
    void forEachStoredSample(
            uint32_t from,
            uint32_t to,
            bool includePrevious,
            std::function<void(Sample const&)> const& callback) const;
    void forEachRecentPanelSample(
            uint32_t from,
            uint32_t to,
            bool includePrevious,
            std::function<void(PanelSample const&)> const& callback) const;
    void forEachStoredPanelSample(
            uint32_t from,
            uint32_t to,
            bool includePrevious,
            std::function<void(PanelSample const&)> const& callback) const;
    void ensureArchiveIndexes() const;
    void updateArchivedSampleBounds(uint32_t& oldest, uint32_t& newest) const;
    void invalidateArchivedSampleBoundsCache(uint32_t timestamp) const;
    void forEachDailySummary(uint32_t from, uint32_t to, std::function<void(DailySummary const&)> const& callback) const;
    void forEachPanelDailySummary(uint32_t from, uint32_t to, std::function<void(PanelDailySummary const&)> const& callback) const;
    void loadPreviousSample();
    void loadPreviousPanelSample();

    EnergySummary calculateRecentSummary(uint32_t from, uint32_t to) const;
    EnergySummary calculateDailySummary(uint32_t from, uint32_t to) const;
    PanelEnergySummary calculateRecentPanelSummary(uint32_t from, uint32_t to) const;
    PanelEnergySummary calculateDailyPanelSummary(uint32_t from, uint32_t to) const;
    std::vector<PanelDescriptor> collectPanelDescriptors(PanelEnergySummary const& summary) const;
    void addInterval(EnergySummary& summary, Sample const& previous, Sample const& current, uint32_t seconds) const;
    void addIntegratedInterval(EnergySummary& summary, Sample const& sample, uint32_t seconds) const;
    void addPanelInterval(PanelEnergySummary& summary, PanelSample const& previous, PanelSample const& current, uint32_t seconds) const;
    static void addExtrema(EnergySummary& summary, Sample const& sample);
    static void addBatterySocAverage(EnergySummary& summary, float socPercent, uint32_t seconds);
    static bool hasBatterySocAverage(EnergySummary const& summary);
    static float averageBatterySoc(EnergySummary const& summary);
    static uint32_t localDayStart(uint32_t timestamp);
    static int16_t clampInt16(float value);
    static uint16_t clampUInt16(float value);
    static float batteryBoostSavingWatts(float gridImportWatts, float batteryVoltage, float batteryDischargeCurrent, float currentLimit);
    static uint16_t flexibleLoadPowerFlag(uint8_t index);
    static uint8_t panelStorageIndex(uint8_t inverterIndex, uint8_t channel);
    static bool isValidPanelStorageIndex(uint8_t index);
    static void addSummaryJson(JsonObject& root, EnergySummary const& summary);
    void addPanelMetadataJson(JsonArray& root, std::vector<PanelDescriptor> const& panels, PanelEnergySummary const& summary) const;
    void addStorageDiagnosticsJson(JsonArray& root) const;
    void addRecentSeriesJson(
            JsonArray& root,
            uint32_t from,
            uint32_t to,
            uint32_t resolutionSeconds,
            bool compact,
            std::vector<PanelDescriptor> const& panels,
            bool includePanelPowers,
            bool includeTemperatures) const;
    void addDailySeriesJson(JsonArray& root, uint32_t from, uint32_t to, std::vector<PanelDescriptor> const& panels) const;

    mutable std::mutex _mutex;
    Task _sampleTask;
    Accumulator _accumulator;
    BatteryBoostSavingsState _batteryBoostSavingsState;
    fs::LittleFSFS _statisticsFs;
    fs::LittleFSFS* _fs = nullptr;
    bool _usingDedicatedFs = false;
    uint32_t _sampleCapacity = 0;
    bool _hasPreviousSample = false;
    Sample _previousSample;
    bool _hasPreviousPanelSample = false;
    PanelSample _previousPanelSample;
    mutable bool _archivedSampleBoundsValid = false;
    mutable uint32_t _archivedSampleBoundsCheckedMillis = 0;
    mutable uint32_t _archivedSampleBoundsOldest = 0;
    mutable uint32_t _archivedSampleBoundsNewest = 0;
};

extern StatisticsClass Statistics;
