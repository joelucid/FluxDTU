// SPDX-License-Identifier: GPL-2.0-or-later
#include "Statistics.h"
#include "Configuration.h"
#include "FlexibleLoadStats.h"
#include "PowerLimiter.h"
#include "SunPosition.h"
#include <battery/Controller.h>
#include <battery/Stats.h>
#include <esp_partition.h>
#include <gridcharger/Controller.h>
#include <gridcharger/Stats.h>
#include <Hoymiles.h>
#include <powermeter/Controller.h>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <ctime>
#include <list>
#include <limits>
#include <optional>
#include <vector>

namespace {
constexpr char SampleFile[] = "/statistics_samples.bin";
constexpr char SampleTempFile[] = "/statistics_samples.tmp";
constexpr char SampleBackupFile[] = "/statistics_samples.bak";
constexpr char DailyFile[] = "/statistics_days.bin";
constexpr char DailyTempFile[] = "/statistics_days.tmp";
constexpr char DailyBackupFile[] = "/statistics_days.bak";
constexpr char PanelSampleFile[] = "/statistics_panel_samples.bin";
constexpr char PanelDailyFile[] = "/statistics_panel_days.bin";
constexpr char SampleArchivePrefix[] = "statistics_samples_archive";
constexpr char PanelSampleArchivePrefix[] = "statistics_panel_samples_archive";
constexpr uint32_t ArchiveIndexMagic = 0x53544958; // STIX
constexpr uint16_t ArchiveIndexVersion = 1;
constexpr uint32_t InvalidArchiveIndex = std::numeric_limits<uint32_t>::max();
constexpr uint32_t SampleFileMagic = 0x53544154; // STAT
constexpr uint32_t DailyFileMagic = 0x53544459; // STDY
constexpr uint32_t PanelSampleFileMagic = 0x53545053; // STPS
constexpr uint32_t PanelDailyFileMagic = 0x53545044; // STPD
constexpr uint16_t SampleFileVersion = 5;
constexpr uint16_t DailyFileVersion = 5;
constexpr uint16_t PanelSampleFileVersion = 1;
constexpr uint16_t PanelDailyFileVersion = 1;
constexpr uint16_t LegacyV2FileVersion = 2;
constexpr uint16_t LegacyV3SampleFileVersion = 3;
constexpr uint16_t LegacyV4SampleFileVersion = 4;
constexpr uint16_t LegacyV3DailyFileVersion = 3;
constexpr uint16_t LegacyV4DailyFileVersion = 4;
constexpr uint16_t LegacyBoostV4HasBatteryBoostSavings = 1 << 14;
constexpr float BatteryBoostNormalCurrentAmps = 25.0f;
constexpr float BatteryBoostMaxCurrentAmps = 50.0f;
constexpr float BatteryBoostBudgetSeconds = 60.0f;
constexpr float BatteryBoostCooldownSeconds = 30.0f * 60.0f;
constexpr float BatteryBoostInverterEfficiency = 0.92f;
constexpr float BatteryBoostAcLimitWatts = 1500.0f;
static_assert(POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT <= 4,
        "Statistics sample flags reserve four flexible load measurement slots");

struct LegacyV2Sample {
    uint32_t timestamp = 0;
    int16_t batteryDischargePowerWatts = 0;
    int16_t solarPowerWatts = 0;
    int16_t batteryPowerWatts = 0;
    int16_t gridPowerWatts = 0;
    int16_t gridChargerPowerWatts = 0;
    int16_t targetPowerWatts = 0;
    uint16_t gridImportPowerWatts = 0;
    uint16_t gridExportPowerWatts = 0;
    uint16_t batterySocPermille = 0;
    uint16_t batteryVoltageDecivolt = 0;
    int16_t batteryTemperatureDecicelsius = 0;
    uint16_t flags = 0;
};

struct ArchiveIndexHeader {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t recordSize = 0;
    uint32_t archiveSize = 0;
    uint32_t recordCount = 0;
    uint32_t dayCount = 0;
    uint32_t firstTimestamp = 0;
    uint32_t lastTimestamp = 0;
    uint32_t lastIndex = InvalidArchiveIndex;
};

struct ArchiveDayIndexEntry {
    uint32_t dayStart = 0;
    uint32_t firstIndex = 0;
    uint32_t endIndex = 0;
    uint32_t previousIndex = InvalidArchiveIndex;
};

struct ArchiveReadRange {
    bool hasRange = false;
    uint32_t startIndex = 0;
    uint32_t endIndex = 0;
    uint32_t previousIndex = InvalidArchiveIndex;
};

struct LegacyV3Sample {
    uint32_t timestamp = 0;
    int16_t batteryDischargePowerWatts = 0;
    int16_t solarPowerWatts = 0;
    int16_t batteryPowerWatts = 0;
    int16_t gridPowerWatts = 0;
    int16_t gridChargerPowerWatts = 0;
    int16_t targetPowerWatts = 0;
    int16_t flexibleLoadPowerWatts[POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT] = {};
    uint16_t gridImportPowerWatts = 0;
    uint16_t gridExportPowerWatts = 0;
    uint16_t batterySocPermille = 0;
    uint16_t batteryVoltageDecivolt = 0;
    int16_t batteryTemperatureDecicelsius = 0;
    uint16_t flags = 0;
};

struct LegacyV4Sample {
    uint32_t timestamp = 0;
    int16_t batteryDischargePowerWatts = 0;
    int16_t solarPowerWatts = 0;
    int16_t batteryPowerWatts = 0;
    int16_t gridPowerWatts = 0;
    int16_t gridChargerPowerWatts = 0;
    int16_t targetPowerWatts = 0;
    int16_t flexibleLoadPowerWatts[POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT] = {};
    uint16_t gridImportPowerWatts = 0;
    uint16_t gridExportPowerWatts = 0;
    uint16_t batterySocPermille = 0;
    uint16_t batteryVoltageDecivolt = 0;
    int16_t batteryTemperatureDecicelsius = 0;
    int16_t inverterTemperatureDecicelsius[INV_MAX_COUNT] = {};
    uint16_t inverterTemperatureFlags = 0;
    uint16_t flags = 0;
};

struct LegacyV4BoostSample {
    uint32_t timestamp = 0;
    int16_t batteryDischargePowerWatts = 0;
    int16_t solarPowerWatts = 0;
    int16_t batteryPowerWatts = 0;
    int16_t gridPowerWatts = 0;
    int16_t gridChargerPowerWatts = 0;
    int16_t targetPowerWatts = 0;
    int16_t flexibleLoadPowerWatts[POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT] = {};
    uint16_t gridImportPowerWatts = 0;
    uint16_t gridExportPowerWatts = 0;
    uint16_t batterySocPermille = 0;
    uint16_t batteryVoltageDecivolt = 0;
    int16_t batteryTemperatureDecicelsius = 0;
    uint16_t batteryBoostSavingsNormalPowerWatts = 0;
    uint16_t batteryBoostSavingsRelaxedPowerWatts = 0;
    uint16_t batteryBoostRelaxLimitedPowerWatts = 0;
    uint16_t batteryBoostSeconds = 0;
    uint16_t batteryBoostBudgetUsedPermille = 0;
    uint16_t flags = 0;
};

struct LegacyV2EnergySummary {
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
};

struct LegacyV2DailySummary : LegacyV2EnergySummary {
    uint32_t dayStart = 0;
};

struct LegacyV3EnergySummary {
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
};

struct LegacyV3DailySummary : LegacyV3EnergySummary {
    uint32_t dayStart = 0;
};

struct LegacyV4EnergySummary {
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
};

struct LegacyV4DailySummary : LegacyV4EnergySummary {
    uint32_t dayStart = 0;
};

uint32_t nextLocalDayStart(uint32_t dayStart)
{
    time_t t = dayStart + (36 * 60 * 60);
    struct tm timeinfo;
    localtime_r(&t, &timeinfo);
    timeinfo.tm_hour = 0;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    return static_cast<uint32_t>(mktime(&timeinfo));
}  // namespace

uint32_t addLocalDays(uint32_t dayStart, uint16_t days)
{
    auto result = dayStart;
    for (uint16_t i = 0; i < days; ++i) {
        const auto next = nextLocalDayStart(result);
        if (next <= result) { break; }
        result = next;
    }
    return result;
}

uint32_t saturatingAddSeconds(uint32_t start, uint32_t seconds)
{
    if (std::numeric_limits<uint32_t>::max() - start < seconds) {
        return std::numeric_limits<uint32_t>::max();
    }

    return start + seconds;
}

uint32_t localMonthStart(uint32_t timestamp)
{
    time_t t = timestamp;
    struct tm timeinfo;
    localtime_r(&t, &timeinfo);
    timeinfo.tm_mday = 1;
    timeinfo.tm_hour = 0;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    return static_cast<uint32_t>(mktime(&timeinfo));
}

uint32_t offsetLocalMonthStart(uint32_t timestamp, int monthOffset)
{
    time_t t = timestamp;
    struct tm timeinfo;
    localtime_r(&t, &timeinfo);
    timeinfo.tm_mday = 1;
    timeinfo.tm_hour = 0;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    timeinfo.tm_mon += monthOffset;
    return static_cast<uint32_t>(mktime(&timeinfo));
}

uint32_t nextLocalMonthStart(uint32_t monthStart)
{
    return offsetLocalMonthStart(monthStart, 1);
}

uint32_t previousLocalMonthStart(uint32_t monthStart)
{
    return offsetLocalMonthStart(monthStart, -1);
}

String archivePath(char const* prefix, uint32_t timestamp)
{
    time_t t = timestamp;
    struct tm timeinfo;
    localtime_r(&t, &timeinfo);

    char path[48];
    snprintf(path, sizeof(path), "/%s_%04d%02d.bin",
            prefix,
            timeinfo.tm_year + 1900,
            timeinfo.tm_mon + 1);
    return String(path);
}

String archiveIndexPath(String const& archivePath)
{
    String path = archivePath;
    if (path.endsWith(".bin")) {
        path.remove(path.length() - 4);
    }
    path += ".idx";
    return path;
}

bool archiveMonthBounds(String const& path, char const* prefix, uint32_t& from, uint32_t& to)
{
    const String expectedPrefix = String("/") + prefix + "_";
    if (!path.startsWith(expectedPrefix) || !path.endsWith(".bin")) { return false; }
    if (path.length() != expectedPrefix.length() + 10) { return false; }

    const auto year = path.substring(expectedPrefix.length(), expectedPrefix.length() + 4).toInt();
    const auto month = path.substring(expectedPrefix.length() + 4, expectedPrefix.length() + 6).toInt();
    if (year < 2020 || month < 1 || month > 12) { return false; }

    struct tm timeinfo = {};
    timeinfo.tm_year = year - 1900;
    timeinfo.tm_mon = month - 1;
    timeinfo.tm_mday = 1;
    timeinfo.tm_hour = 0;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    from = static_cast<uint32_t>(mktime(&timeinfo));

    timeinfo.tm_mon += 1;
    to = static_cast<uint32_t>(mktime(&timeinfo));
    return to > from;
}

uint32_t archiveLocalDayStart(uint32_t timestamp)
{
    time_t t = timestamp;
    struct tm timeinfo;
    localtime_r(&t, &timeinfo);
    timeinfo.tm_hour = 0;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    return static_cast<uint32_t>(mktime(&timeinfo));
}

bool readArchiveIndex(
        fs::FS& fs,
        String const& archivePath,
        uint16_t recordSize,
        uint32_t archiveSize,
        ArchiveIndexHeader& header,
        std::vector<ArchiveDayIndexEntry>& entries)
{
    auto const indexPath = archiveIndexPath(archivePath);
    auto file = fs.open(indexPath.c_str(), "r");
    if (!file) { return false; }

    const auto read = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
    if (read != sizeof(header)
            || header.magic != ArchiveIndexMagic
            || header.version != ArchiveIndexVersion
            || header.recordSize != recordSize
            || header.archiveSize != archiveSize
            || header.recordCount != archiveSize / recordSize
            || header.dayCount > 62
            || file.size() != sizeof(ArchiveIndexHeader) + (header.dayCount * sizeof(ArchiveDayIndexEntry))) {
        file.close();
        return false;
    }

    entries.clear();
    entries.resize(header.dayCount);
    if (header.dayCount > 0) {
        const auto bytes = header.dayCount * sizeof(ArchiveDayIndexEntry);
        if (file.read(reinterpret_cast<uint8_t*>(entries.data()), bytes) != bytes) {
            file.close();
            entries.clear();
            return false;
        }
    }
    file.close();
    return true;
}

bool writeArchiveIndex(
        fs::FS& fs,
        String const& archivePath,
        ArchiveIndexHeader& header,
        std::vector<ArchiveDayIndexEntry> const& entries)
{
    header.dayCount = entries.size();
    auto const indexPath = archiveIndexPath(archivePath);
    auto const tempPath = indexPath + ".tmp";
    fs.remove(tempPath.c_str());
    auto indexFile = fs.open(tempPath.c_str(), "w");
    if (!indexFile) { return false; }

    bool success = indexFile.write(reinterpret_cast<uint8_t const*>(&header), sizeof(header)) == sizeof(header);
    if (success && !entries.empty()) {
        const auto bytes = entries.size() * sizeof(ArchiveDayIndexEntry);
        success = indexFile.write(reinterpret_cast<uint8_t const*>(entries.data()), bytes) == bytes;
    }
    indexFile.close();
    if (!success) {
        fs.remove(tempPath.c_str());
        return false;
    }

    fs.remove(indexPath.c_str());
    if (!fs.rename(tempPath.c_str(), indexPath.c_str())) {
        fs.remove(tempPath.c_str());
        return false;
    }
    return true;
}

bool buildArchiveIndex(
        fs::FS& fs,
        String const& archivePath,
        uint16_t recordSize,
        std::function<uint32_t(uint8_t const*)> const& timestampReader,
        std::function<bool(uint8_t const*)> const& validReader)
{
    auto archive = fs.open(archivePath.c_str(), "r");
    if (!archive) { return false; }

    ArchiveIndexHeader header;
    header.magic = ArchiveIndexMagic;
    header.version = ArchiveIndexVersion;
    header.recordSize = recordSize;
    header.archiveSize = archive.size();
    header.recordCount = header.archiveSize / recordSize;

    std::vector<ArchiveDayIndexEntry> entries;
    std::vector<uint8_t> buffer(recordSize);
    uint32_t lastValidIndex = InvalidArchiveIndex;
    for (uint32_t index = 0; index < header.recordCount; ++index) {
        if (archive.read(buffer.data(), recordSize) != recordSize) { break; }
        if (!validReader(buffer.data())) { continue; }

        const auto timestamp = timestampReader(buffer.data());
        if (timestamp == 0) { continue; }

        if (header.firstTimestamp == 0) {
            header.firstTimestamp = timestamp;
        }
        header.lastTimestamp = timestamp;
        header.lastIndex = index;

        const auto dayStart = archiveLocalDayStart(timestamp);
        if (entries.empty() || entries.back().dayStart != dayStart) {
            ArchiveDayIndexEntry entry;
            entry.dayStart = dayStart;
            entry.firstIndex = index;
            entry.endIndex = index + 1;
            entry.previousIndex = lastValidIndex;
            entries.push_back(entry);
        } else {
            entries.back().endIndex = index + 1;
        }

        lastValidIndex = index;
    }
    archive.close();

    return writeArchiveIndex(fs, archivePath, header, entries);
}

void appendArchiveIndexRecord(
        fs::FS& fs,
        String const& archivePath,
        uint16_t recordSize,
        uint32_t oldArchiveSize,
        uint32_t timestamp)
{
    if (recordSize == 0 || timestamp == 0 || oldArchiveSize % recordSize != 0) { return; }

    ArchiveIndexHeader header;
    std::vector<ArchiveDayIndexEntry> entries;
    if (!readArchiveIndex(fs, archivePath, recordSize, oldArchiveSize, header, entries)) {
        return;
    }

    const auto recordIndex = oldArchiveSize / recordSize;
    const auto previousLastIndex = header.lastIndex;
    header.archiveSize = oldArchiveSize + recordSize;
    header.recordCount++;
    if (header.firstTimestamp == 0) {
        header.firstTimestamp = timestamp;
    }
    header.lastTimestamp = timestamp;
    header.lastIndex = recordIndex;

    const auto dayStart = archiveLocalDayStart(timestamp);
    if (entries.empty() || entries.back().dayStart != dayStart) {
        ArchiveDayIndexEntry entry;
        entry.dayStart = dayStart;
        entry.firstIndex = recordIndex;
        entry.endIndex = recordIndex + 1;
        entry.previousIndex = previousLastIndex;
        entries.push_back(entry);
    } else {
        entries.back().endIndex = recordIndex + 1;
    }

    writeArchiveIndex(fs, archivePath, header, entries);
}

bool ensureArchiveIndex(
        fs::FS& fs,
        String const& archivePath,
        uint16_t recordSize,
        uint32_t archiveSize,
        std::function<uint32_t(uint8_t const*)> const& timestampReader,
        std::function<bool(uint8_t const*)> const& validReader,
        ArchiveIndexHeader& header,
        std::vector<ArchiveDayIndexEntry>& entries)
{
    if (recordSize == 0 || archiveSize < recordSize) { return false; }
    if (readArchiveIndex(fs, archivePath, recordSize, archiveSize, header, entries)) { return true; }
    if (!buildArchiveIndex(fs, archivePath, recordSize, timestampReader, validReader)) { return false; }
    return readArchiveIndex(fs, archivePath, recordSize, archiveSize, header, entries);
}

ArchiveReadRange archiveReadRange(
        ArchiveIndexHeader const& header,
        std::vector<ArchiveDayIndexEntry> const& entries,
        uint32_t from,
        uint32_t to,
        bool includePrevious)
{
    ArchiveReadRange range;
    if (entries.empty()) {
        if (includePrevious && header.lastTimestamp > 0 && header.lastTimestamp < from) {
            range.previousIndex = header.lastIndex;
        }
        return range;
    }

    const auto fromDay = archiveLocalDayStart(from);
    const auto toDay = archiveLocalDayStart(to);
    for (auto const& entry : entries) {
        if (entry.dayStart > toDay) { break; }
        if (entry.endIndex <= entry.firstIndex) { continue; }
        if (entry.dayStart < fromDay) { continue; }

        if (!range.hasRange) {
            range.hasRange = true;
            range.startIndex = entry.firstIndex;
            range.previousIndex = entry.previousIndex;
        }
        range.endIndex = entry.endIndex;
    }

    if (!range.hasRange && includePrevious && header.lastTimestamp > 0 && header.lastTimestamp < from) {
        range.previousIndex = header.lastIndex;
    }
    return range;
}

void forEachArchivePathInRange(
        char const* prefix,
        uint32_t from,
        uint32_t to,
        bool includePrevious,
        std::function<void(String const&)> const& callback)
{
    if (to < from) { return; }

    auto monthStart = localMonthStart(from);
    if (includePrevious) {
        const auto previousMonth = previousLocalMonthStart(monthStart);
        if (previousMonth < monthStart) {
            monthStart = previousMonth;
        }
    }

    const auto endMonth = localMonthStart(to);
    for (uint16_t guard = 0; guard < 1800; ++guard) {
        callback(archivePath(prefix, monthStart));
        if (monthStart >= endMonth) { break; }

        const auto nextMonth = nextLocalMonthStart(monthStart);
        if (nextMonth <= monthStart) { break; }
        monthStart = nextMonth;
    }
}

std::optional<PowerLimiterInverterConfig::InverterPowerSource> getGovernedPowerSource(CONFIG_T const& config, uint64_t serial)
{
    for (auto const& invConfig : config.PowerLimiter.Inverters) {
        if (invConfig.Serial == 0ULL) { break; }
        if (invConfig.Serial != serial || !invConfig.IsGoverned) { continue; }
        return invConfig.PowerSource;
    }

    return std::nullopt;
}

bool isBatteryBackedPowerSource(std::optional<PowerLimiterInverterConfig::InverterPowerSource> const& powerSource)
{
    return powerSource
        && (*powerSource == PowerLimiterInverterConfig::InverterPowerSource::Battery
            || *powerSource == PowerLimiterInverterConfig::InverterPowerSource::SmartBuffer);
}

String serialString(uint64_t serial)
{
    char serialBuffer[17];
    snprintf(serialBuffer, sizeof(serialBuffer), "%08" PRIx32 "%08" PRIx32,
            static_cast<uint32_t>((serial >> 32) & 0xFFFFFFFF),
            static_cast<uint32_t>(serial & 0xFFFFFFFF));
    return String(serialBuffer);
}

}  // namespace

StatisticsClass Statistics;

void StatisticsClass::init(Scheduler& scheduler)
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        initFilesystem();
        recoverSampleFileReplacement();
        migrateLegacyFiles();
        migrateV2StatisticsFiles();
        recoverArchivedStatisticsFiles();
        initSampleFile();
        initDailyFile();
        initPanelSampleFile();
        initPanelDailyFile();
        rebuildDailySummariesFromSamplesIfEmpty();
        backfillDailySocAveragesFromSamples();
        loadPreviousSample();
        loadPreviousPanelSample();
        ensureArchiveIndexes();
    }

    scheduler.addTask(_sampleTask);
    _sampleTask.setCallback(std::bind(&StatisticsClass::sampleTaskCb, this));
    _sampleTask.setIterations(TASK_FOREVER);
    _sampleTask.setInterval(1 * TASK_SECOND);
    _sampleTask.enable();
}

void StatisticsClass::sampleTaskCb()
{
    const time_t now = time(nullptr);
    if (now < 1600000000) {
        std::lock_guard<std::mutex> lock(_mutex);
        resetAccumulator();
        return;
    }

    // After boot, avoid integrating partial inverter sets. The first stable
    // period is scaled to the full statistics interval when stored.
    if (!areEnabledInvertersReady()) {
        std::lock_guard<std::mutex> lock(_mutex);
        resetAccumulator(millis());
        return;
    }

    accumulateCurrent();

    std::lock_guard<std::mutex> lock(_mutex);
    if (!isSampleDue(static_cast<uint32_t>(now))) { return; }

    const auto sample = buildIntegratedSample(static_cast<uint32_t>(now));
    resetAccumulator(millis());
    if (sample.timestamp == 0) { return; }

    storeSample(sample);
    storePanelSample(collectPanelSample(sample.timestamp));
}

StatisticsClass::Sample StatisticsClass::collectInstantaneousSample() const
{
    Sample sample;

    const time_t now = time(nullptr);
    if (now < 1600000000) { return sample; }

    auto const& config = Configuration.get();
    sample.timestamp = static_cast<uint32_t>(now);

    const auto classifiedInverters = collectClassifiedInverterSample();
    if (classifiedInverters.hasSolar) {
        sample.solarPowerWatts = clampInt16(classifiedInverters.solarPowerWatts);
        sample.flags |= HasSolarPower;
    }

    if (classifiedInverters.hasBatteryDischarge) {
        sample.batteryDischargePowerWatts = clampInt16(classifiedInverters.batteryDischargePowerWatts);
        sample.flags |= HasBatteryDischargePower;
    }

    auto const nowMillis = millis();
    auto constexpr maxInverterTemperatureAgeMillis = SampleIntervalSeconds * 2 * 1000;
    for (uint8_t i = 0; i < INV_MAX_COUNT; ++i) {
        auto const& invConfig = config.Inverter[i];
        if (invConfig.Serial == 0 || !invConfig.Poll_Enable) { continue; }

        auto inv = Hoymiles.getInverterBySerial(invConfig.Serial);
        if (inv == nullptr) { continue; }

        auto stats = inv->Statistics();
        auto const lastUpdate = stats->getLastUpdate();
        if (lastUpdate == 0 || (nowMillis - lastUpdate) > maxInverterTemperatureAgeMillis) {
            continue;
        }

        if (!stats->hasChannelFieldValue(TYPE_INV, CH0, FLD_T)) { continue; }

        auto const temperature = stats->getChannelFieldValue(TYPE_INV, CH0, FLD_T);
        if (!std::isfinite(temperature) || temperature < -40.0f || temperature > 140.0f) {
            continue;
        }

        sample.inverterTemperatureDecicelsius[i] = clampInt16(temperature * 10.0f);
        sample.inverterTemperatureFlags |= 1 << i;
        sample.flags |= HasInverterTemperatures;
    }

    if (config.Battery.Enabled) {
        auto stats = Battery.getStats();

        if (stats->isSoCValid()) {
            sample.batterySocPermille = clampUInt16(stats->getSoC() * 10.0f);
            sample.flags |= HasBatterySoc;
        }

        if (stats->isVoltageValid()) {
            sample.batteryVoltageDecivolt = clampUInt16(stats->getVoltage() * 10.0f);
            sample.flags |= HasBatteryVoltage;
        }

        if (stats->isVoltageValid() && stats->isCurrentValid()) {
            sample.batteryPowerWatts = clampInt16(stats->getVoltage() * stats->getChargeCurrent());
            sample.flags |= HasBatteryPower;
        }

        auto temperature = stats->getTemperature();
        if (temperature) {
            sample.batteryTemperatureDecicelsius = clampInt16(*temperature * 10.0f);
            sample.flags |= HasBatteryTemperature;
        }
    }

    if (config.PowerMeter.Enabled && PowerMeter.isDataValid()) {
        sample.gridPowerWatts = clampInt16(PowerMeter.getPowerTotal());
        sample.flags |= HasGridPower;
    }

    if (config.GridCharger.Enabled) {
        auto inputPower = GridCharger.getStats()->getInputPower();
        if (inputPower) {
            sample.gridChargerPowerWatts = clampInt16(*inputPower);
            sample.flags |= HasGridChargerPower;
        }
    }

    for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
        auto const power = FlexibleLoadStats.getPowerWatts(i);
        if (!power) { continue; }

        sample.flexibleLoadPowerWatts[i] = clampInt16(*power);
        sample.flags |= flexibleLoadPowerFlag(i);
    }

    if (config.PowerLimiter.Enabled && PowerLimiter.usesBatteryPoweredInverter()) {
        sample.targetPowerWatts = clampInt16(PowerLimiter.getBatteryTargetPowerConsumption());
        sample.flags |= HasTargetPower;
    }

    return sample;
}

StatisticsClass::PanelSample StatisticsClass::collectPanelSample(uint32_t timestamp) const
{
    PanelSample sample;
    if (timestamp == 0) { return sample; }

    auto const& config = Configuration.get();
    sample.timestamp = timestamp;

    auto const nowMillis = millis();
    auto constexpr maxInverterDataAgeMillis = SampleIntervalSeconds * 2 * 1000;
    for (uint8_t i = 0; i < INV_MAX_COUNT; ++i) {
        auto const& invConfig = config.Inverter[i];
        if (invConfig.Serial == 0 || !invConfig.Poll_Enable) { continue; }
        if (isBatteryBackedPowerSource(getGovernedPowerSource(config, invConfig.Serial))) { continue; }

        auto inv = Hoymiles.getInverterBySerial(invConfig.Serial);
        if (inv == nullptr) { continue; }

        auto stats = inv->Statistics();
        auto const lastUpdate = stats->getLastUpdate();
        if (lastUpdate == 0 || (nowMillis - lastUpdate) > maxInverterDataAgeMillis) {
            continue;
        }

        for (auto const& c : stats->getChannelsByType(TYPE_DC)) {
            const auto channel = static_cast<uint8_t>(c);
            if (channel >= INV_MAX_CHAN_COUNT) { continue; }
            if (!stats->hasChannelFieldValue(TYPE_DC, c, FLD_PDC)) { continue; }

            auto const power = stats->getChannelFieldValue(TYPE_DC, c, FLD_PDC);
            if (!std::isfinite(power)) { continue; }

            const auto index = panelStorageIndex(i, channel);
            if (!isValidPanelStorageIndex(index)) { continue; }

            sample.panelPowerWatts[index] = clampInt16(power);
            sample.panelPowerFlags |= 1ULL << index;
        }
    }

    return sample;
}

StatisticsClass::ClassifiedInverterSample StatisticsClass::collectClassifiedInverterSample() const
{
    ClassifiedInverterSample result;
    auto const& config = Configuration.get();

    for (uint8_t i = 0; i < Hoymiles.getNumInverters(); i++) {
        auto inv = Hoymiles.getInverterByPos(i);
        if (inv == nullptr || !inv->getEnablePolling()) { continue; }

        const auto powerSource = getGovernedPowerSource(config, inv->serial());
        const auto isBatteryDischargeInverter = isBatteryBackedPowerSource(powerSource);

        float acPowerWatts = 0;
        for (auto const& c : inv->Statistics()->getChannelsByType(TYPE_AC)) {
            acPowerWatts += inv->Statistics()->getChannelFieldValue(TYPE_AC, c, FLD_PAC);
        }

        if (isBatteryDischargeInverter) {
            result.batteryDischargePowerWatts += acPowerWatts;
            result.hasBatteryDischarge = true;
        } else {
            result.solarPowerWatts += acPowerWatts;
            result.hasSolar = true;
        }
    }

    return result;
}

bool StatisticsClass::areEnabledInvertersReady() const
{
    auto const& config = Configuration.get();
    const bool isDayPeriod = SunPosition.isDayPeriod();

    for (uint8_t i = 0; i < Hoymiles.getNumInverters(); i++) {
        auto inv = Hoymiles.getInverterByPos(i);
        if (inv == nullptr || !inv->getEnablePolling()) { continue; }
        if (!isDayPeriod && !isBatteryBackedPowerSource(getGovernedPowerSource(config, inv->serial()))) {
            continue;
        }
        if (inv->Statistics()->getLastUpdate() == 0) { return false; }
    }

    return true;
}

bool StatisticsClass::isSampleDue(uint32_t timestamp) const
{
    if (_hasPreviousSample && timestamp > _previousSample.timestamp) {
        return (timestamp - _previousSample.timestamp) >= SampleIntervalSeconds;
    }

    return _accumulator.windowSeconds >= SampleIntervalSeconds;
}

void StatisticsClass::accumulateCurrent()
{
    const time_t now = time(nullptr);
    const auto nowMillis = millis();

    if (now < 1600000000) {
        std::lock_guard<std::mutex> lock(_mutex);
        resetAccumulator();
        return;
    }

    const auto sample = collectInstantaneousSample();

    std::lock_guard<std::mutex> lock(_mutex);
    accumulateSample(sample, nowMillis);
}

void StatisticsClass::accumulateSample(Sample const& sample, uint32_t nowMillis)
{
    if (_accumulator.lastMillis == 0) {
        _accumulator.lastMillis = nowMillis == 0 ? 1 : nowMillis;
        return;
    }

    auto const elapsedMillis = nowMillis - _accumulator.lastMillis;
    _accumulator.lastMillis = nowMillis == 0 ? 1 : nowMillis;
    if (elapsedMillis == 0) { return; }

    auto const seconds = elapsedMillis / 1000.0f;
    _accumulator.windowSeconds += seconds;

    if (sample.flags & HasBatteryDischargePower) {
        _accumulator.batteryDischargePowerWattSeconds += std::max<int32_t>(0, sample.batteryDischargePowerWatts) * seconds;
        _accumulator.batteryDischargePowerSeconds += seconds;
        _accumulator.hasBatteryDischargePower = true;
    }

    if (sample.flags & HasSolarPower) {
        _accumulator.solarPowerWattSeconds += std::max<int32_t>(0, sample.solarPowerWatts) * seconds;
        _accumulator.solarPowerSeconds += seconds;
        _accumulator.hasSolarPower = true;
    }

    if (sample.flags & HasBatteryPower) {
        _accumulator.batteryPowerWattSeconds += sample.batteryPowerWatts * seconds;
        _accumulator.batteryPowerSeconds += seconds;
        _accumulator.hasBatteryPower = true;
    }

    if (sample.flags & HasGridPower) {
        _accumulator.gridPowerWattSeconds += sample.gridPowerWatts * seconds;
        _accumulator.gridImportPowerWattSeconds += std::max<int32_t>(0, sample.gridPowerWatts) * seconds;
        _accumulator.gridExportPowerWattSeconds += std::max<int32_t>(0, -static_cast<int32_t>(sample.gridPowerWatts)) * seconds;
        _accumulator.gridPowerSeconds += seconds;
        _accumulator.hasGridPower = true;
    }

    accumulateBatteryBoostSavings(sample, seconds);

    if (sample.flags & HasGridChargerPower) {
        _accumulator.gridChargerPowerWattSeconds += std::max<int32_t>(0, sample.gridChargerPowerWatts) * seconds;
        _accumulator.gridChargerPowerSeconds += seconds;
        _accumulator.hasGridChargerPower = true;
    }

    for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
        if (!(sample.flags & flexibleLoadPowerFlag(i))) { continue; }

        _accumulator.flexibleLoadPowerWattSeconds[i] += std::max<int32_t>(0, sample.flexibleLoadPowerWatts[i]) * seconds;
        _accumulator.flexibleLoadPowerSeconds[i] += seconds;
        _accumulator.hasFlexibleLoadPower[i] = true;
    }

    if (sample.flags & HasTargetPower) {
        _accumulator.targetPowerWattSeconds += sample.targetPowerWatts * seconds;
        _accumulator.targetPowerSeconds += seconds;
        _accumulator.hasTargetPower = true;
    }

    if (sample.flags & HasBatterySoc) {
        _accumulator.batterySocPermilleSeconds += sample.batterySocPermille * seconds;
        _accumulator.batterySocSeconds += seconds;
        _accumulator.hasBatterySoc = true;
    }

    if (sample.flags & HasBatteryVoltage) {
        _accumulator.batteryVoltageDecivoltSeconds += sample.batteryVoltageDecivolt * seconds;
        _accumulator.batteryVoltageSeconds += seconds;
        _accumulator.hasBatteryVoltage = true;
    }

    if (sample.flags & HasBatteryTemperature) {
        _accumulator.batteryTemperatureDecicelsiusSeconds += sample.batteryTemperatureDecicelsius * seconds;
        _accumulator.batteryTemperatureSeconds += seconds;
        _accumulator.hasBatteryTemperature = true;
    }

    if (sample.flags & HasInverterTemperatures) {
        for (uint8_t i = 0; i < INV_MAX_COUNT; ++i) {
            if (!(sample.inverterTemperatureFlags & (1 << i))) { continue; }

            _accumulator.inverterTemperatureDecicelsiusSeconds[i] += sample.inverterTemperatureDecicelsius[i] * seconds;
            _accumulator.inverterTemperatureSeconds[i] += seconds;
            _accumulator.hasInverterTemperature[i] = true;
        }
    }
}

void StatisticsClass::relaxBatteryBoostBudget(float seconds)
{
    if (seconds <= 0.0f || _batteryBoostSavingsState.budgetUsedSeconds <= 0.0f) { return; }

    _batteryBoostSavingsState.budgetUsedSeconds = std::max<float>(
            0.0f,
            _batteryBoostSavingsState.budgetUsedSeconds
                - (BatteryBoostBudgetSeconds * seconds / BatteryBoostCooldownSeconds));
}

float StatisticsClass::batteryBoostSavingWatts(
        float gridImportWatts,
        float batteryVoltage,
        float batteryDischargeCurrent,
        float currentLimit)
{
    if (gridImportWatts <= 0.0f || batteryVoltage <= 0.0f || currentLimit <= batteryDischargeCurrent) {
        return 0.0f;
    }

    auto const batteryPowerHeadroom = (currentLimit - batteryDischargeCurrent)
        * batteryVoltage
        * BatteryBoostInverterEfficiency;
    auto const acPowerHeadroom = std::min<float>(BatteryBoostAcLimitWatts, batteryPowerHeadroom);
    return std::min<float>(gridImportWatts, std::max<float>(0.0f, acPowerHeadroom));
}

void StatisticsClass::accumulateBatteryBoostSavings(Sample const& sample, float seconds)
{
    if (seconds <= 0.0f) { return; }

    if (!(sample.flags & HasGridPower)
            || !(sample.flags & HasBatteryPower)
            || !(sample.flags & HasBatteryVoltage)
            || sample.batteryVoltageDecivolt == 0) {
        relaxBatteryBoostBudget(seconds);
        return;
    }

    auto const gridImportWatts = std::max<float>(0.0f, sample.gridPowerWatts);
    auto const batteryVoltage = sample.batteryVoltageDecivolt / 10.0f;
    if (batteryVoltage <= 0.0f) {
        relaxBatteryBoostBudget(seconds);
        return;
    }

    auto const batteryCurrent = sample.batteryPowerWatts / batteryVoltage;
    auto const batteryDischargeCurrent = std::max<float>(0.0f, -batteryCurrent);

    auto remainingSeconds = seconds;
    while (remainingSeconds > 0.0f) {
        auto const stepSeconds = std::min<float>(1.0f, remainingSeconds);
        remainingSeconds -= stepSeconds;

        auto const normalSavingsWatts = batteryBoostSavingWatts(
                gridImportWatts,
                batteryVoltage,
                batteryDischargeCurrent,
                BatteryBoostNormalCurrentAmps);

        auto const budgetRatio = BatteryBoostBudgetSeconds > 0.0f
            ? std::min<float>(1.0f, _batteryBoostSavingsState.budgetUsedSeconds / BatteryBoostBudgetSeconds)
            : 1.0f;
        auto const relaxedCurrentLimit = BatteryBoostNormalCurrentAmps
            + ((BatteryBoostMaxCurrentAmps - BatteryBoostNormalCurrentAmps) * (1.0f - budgetRatio));
        auto const relaxedSavingsWatts = batteryBoostSavingWatts(
                gridImportWatts,
                batteryVoltage,
                batteryDischargeCurrent,
                relaxedCurrentLimit);
        auto const unrestrictedSavingsWatts = batteryBoostSavingWatts(
                gridImportWatts,
                batteryVoltage,
                batteryDischargeCurrent,
                BatteryBoostMaxCurrentAmps);

        _accumulator.batteryBoostSavingsNormalWattSeconds += normalSavingsWatts * stepSeconds;
        _accumulator.batteryBoostSavingsRelaxedWattSeconds += relaxedSavingsWatts * stepSeconds;
        _accumulator.batteryBoostRelaxLimitedWattSeconds += std::max<float>(
                0.0f,
                unrestrictedSavingsWatts - relaxedSavingsWatts) * stepSeconds;
        _accumulator.hasBatteryBoostSavings = true;

        if (relaxedSavingsWatts > normalSavingsWatts + 0.5f) {
            auto const simulatedDischargeCurrent = batteryDischargeCurrent
                + (relaxedSavingsWatts / (batteryVoltage * BatteryBoostInverterEfficiency));
            auto const stressRatio = std::max<float>(
                    0.0f,
                    (simulatedDischargeCurrent - BatteryBoostNormalCurrentAmps)
                        / (BatteryBoostMaxCurrentAmps - BatteryBoostNormalCurrentAmps));
            _batteryBoostSavingsState.budgetUsedSeconds = std::min<float>(
                    BatteryBoostBudgetSeconds,
                    _batteryBoostSavingsState.budgetUsedSeconds
                        + (stressRatio * stressRatio * stepSeconds));
            _accumulator.batteryBoostSeconds += stepSeconds;
        } else {
            relaxBatteryBoostBudget(stepSeconds);
        }
    }

    _accumulator.batteryBoostBudgetUsedPermille = clampUInt16(
            BatteryBoostBudgetSeconds > 0.0f
                ? (_batteryBoostSavingsState.budgetUsedSeconds / BatteryBoostBudgetSeconds) * 1000.0f
                : 0.0f);
}

StatisticsClass::Sample StatisticsClass::buildIntegratedSample(uint32_t timestamp) const
{
    Sample sample;
    sample.timestamp = timestamp;
    sample.flags = HasIntegratedPower;

    auto average = [](float wattSeconds, float seconds) {
        return seconds > 0.0f ? wattSeconds / seconds : 0.0f;
    };

    if (_accumulator.hasBatteryDischargePower) {
        sample.batteryDischargePowerWatts = clampInt16(average(
                _accumulator.batteryDischargePowerWattSeconds,
                _accumulator.batteryDischargePowerSeconds));
        sample.flags |= HasBatteryDischargePower;
    }

    if (_accumulator.hasSolarPower) {
        sample.solarPowerWatts = clampInt16(average(
                _accumulator.solarPowerWattSeconds,
                _accumulator.solarPowerSeconds));
        sample.flags |= HasSolarPower;
    }

    if (_accumulator.hasBatteryPower) {
        sample.batteryPowerWatts = clampInt16(average(
                _accumulator.batteryPowerWattSeconds,
                _accumulator.batteryPowerSeconds));
        sample.flags |= HasBatteryPower;
    }

    if (_accumulator.hasGridPower) {
        sample.gridPowerWatts = clampInt16(average(
                _accumulator.gridPowerWattSeconds,
                _accumulator.gridPowerSeconds));
        sample.gridImportPowerWatts = clampUInt16(average(
                _accumulator.gridImportPowerWattSeconds,
                _accumulator.gridPowerSeconds));
        sample.gridExportPowerWatts = clampUInt16(average(
                _accumulator.gridExportPowerWattSeconds,
                _accumulator.gridPowerSeconds));
        sample.flags |= HasGridPower;
    }

    if (_accumulator.hasGridChargerPower) {
        sample.gridChargerPowerWatts = clampInt16(average(
                _accumulator.gridChargerPowerWattSeconds,
                _accumulator.gridChargerPowerSeconds));
        sample.flags |= HasGridChargerPower;
    }

    for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
        if (!_accumulator.hasFlexibleLoadPower[i]) { continue; }

        sample.flexibleLoadPowerWatts[i] = clampInt16(average(
                _accumulator.flexibleLoadPowerWattSeconds[i],
                _accumulator.flexibleLoadPowerSeconds[i]));
        sample.flags |= flexibleLoadPowerFlag(i);
    }

    if (_accumulator.hasBatteryBoostSavings) {
        sample.batteryBoostSavingsNormalPowerWatts = clampUInt16(average(
                _accumulator.batteryBoostSavingsNormalWattSeconds,
                _accumulator.windowSeconds));
        sample.batteryBoostSavingsRelaxedPowerWatts = clampUInt16(average(
                _accumulator.batteryBoostSavingsRelaxedWattSeconds,
                _accumulator.windowSeconds));
        sample.batteryBoostRelaxLimitedPowerWatts = clampUInt16(average(
                _accumulator.batteryBoostRelaxLimitedWattSeconds,
                _accumulator.windowSeconds));
        sample.batteryBoostSeconds = clampUInt16(_accumulator.batteryBoostSeconds);
        sample.batteryBoostBudgetUsedPermille = _accumulator.batteryBoostBudgetUsedPermille;
        sample.flags |= HasBatteryBoostSavings;
    }

    if (_accumulator.hasTargetPower) {
        sample.targetPowerWatts = clampInt16(average(
                _accumulator.targetPowerWattSeconds,
                _accumulator.targetPowerSeconds));
        sample.flags |= HasTargetPower;
    }

    if (_accumulator.hasBatterySoc && _accumulator.batterySocSeconds > 0) {
        sample.batterySocPermille = clampUInt16(_accumulator.batterySocPermilleSeconds / _accumulator.batterySocSeconds);
        sample.flags |= HasBatterySoc;
    }

    if (_accumulator.hasBatteryVoltage && _accumulator.batteryVoltageSeconds > 0) {
        sample.batteryVoltageDecivolt = clampUInt16(_accumulator.batteryVoltageDecivoltSeconds / _accumulator.batteryVoltageSeconds);
        sample.flags |= HasBatteryVoltage;
    }

    if (_accumulator.hasBatteryTemperature && _accumulator.batteryTemperatureSeconds > 0) {
        sample.batteryTemperatureDecicelsius = clampInt16(_accumulator.batteryTemperatureDecicelsiusSeconds / _accumulator.batteryTemperatureSeconds);
        sample.flags |= HasBatteryTemperature;
    }

    for (uint8_t i = 0; i < INV_MAX_COUNT; ++i) {
        if (!_accumulator.hasInverterTemperature[i] || _accumulator.inverterTemperatureSeconds[i] <= 0) {
            continue;
        }

        sample.inverterTemperatureDecicelsius[i] = clampInt16(
                _accumulator.inverterTemperatureDecicelsiusSeconds[i] / _accumulator.inverterTemperatureSeconds[i]);
        sample.inverterTemperatureFlags |= 1 << i;
        sample.flags |= HasInverterTemperatures;
    }

    return sample;
}

void StatisticsClass::resetAccumulator(uint32_t nowMillis)
{
    _accumulator = Accumulator();
    _accumulator.lastMillis = nowMillis;
}

uint32_t StatisticsClass::desiredSampleCapacity() const
{
    const auto total = filesystem().totalBytes();
    if (total == 0) { return 0; }

    const auto reserve = std::min<size_t>(128 * 1024, total / 4);
    const auto dailyBytes = sizeof(DailyFileHeader) + (DailyRetentionDays * sizeof(DailySummary));
    const auto statisticsBytes = fileSize(SampleFile) + fileSize(DailyFile);
    const auto nonStatisticsUsed = filesystem().usedBytes() > statisticsBytes
        ? filesystem().usedBytes() - statisticsBytes
        : 0;
    if (total <= nonStatisticsUsed + reserve + dailyBytes) { return 0; }

    const auto availableForSamples = total - nonStatisticsUsed - reserve - dailyBytes;
    const auto maxSamples = DesiredSampleRetentionDays * 24 * 60 * 60 / SampleIntervalSeconds;
    const auto capacity = std::min<size_t>(maxSamples, availableForSamples / sizeof(Sample));
    return static_cast<uint32_t>(capacity);
}

void StatisticsClass::initFilesystem()
{
    _fs = &LittleFS;
    _usingDedicatedFs = false;

    auto partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
        "stats");

    if (!partition) { return; }

    if (_statisticsFs.begin(true, "/statistics", 4, "stats")) {
        _fs = &_statisticsFs;
        _usingDedicatedFs = true;
    }
}

void StatisticsClass::migrateLegacyFiles()
{
    if (!_usingDedicatedFs) { return; }

    copyFromLegacyIfMissing(SampleFile);
    copyFromLegacyIfMissing(DailyFile);
}

bool StatisticsClass::copyFromLegacyIfMissing(char const* path) const
{
    if (filesystem().exists(path) || !LittleFS.exists(path)) { return false; }

    auto source = LittleFS.open(path, "r");
    if (!source) { return false; }

    auto target = filesystem().open(path, "w");
    if (!target) {
        source.close();
        return false;
    }

    uint8_t buffer[256];
    while (source.available()) {
        const auto read = source.read(buffer, sizeof(buffer));
        if (read == 0) { break; }
        if (target.write(buffer, read) != read) {
            source.close();
            target.close();
            filesystem().remove(path);
            return false;
        }
    }

    source.close();
    target.close();
    return true;
}

size_t StatisticsClass::fileSize(char const* path) const
{
    auto file = filesystem().open(path, "r");
    if (!file) { return 0; }
    const auto size = file.size();
    file.close();
    return size;
}

bool StatisticsClass::initSampleFile()
{
    recoverSampleFileReplacement();

    _sampleCapacity = desiredSampleCapacity();
    if (_sampleCapacity == 0) { return false; }

    SampleFileHeader header;
    const bool hasCompatibleFile = readSampleHeader(header)
        && header.magic == SampleFileMagic
        && header.version == SampleFileVersion
        && header.recordSize == sizeof(Sample)
        && header.capacity > 0;

    if (hasCompatibleFile) {
        _sampleCapacity = header.capacity;
        return true;
    }

    const bool hasLegacyV4File = header.magic == SampleFileMagic
            && header.version == LegacyV4SampleFileVersion
            && (header.recordSize == sizeof(LegacyV4Sample)
                    || header.recordSize == sizeof(LegacyV4BoostSample))
            && header.capacity > 0;

    if (hasLegacyV4File) {
        if (migrateV4SampleFile()) { return true; }

        ESP_LOGE(TAG, "Refusing to replace v4 statistics samples after migration failure");
        return false;
    }

    const bool hasLegacyV3File = header.magic == SampleFileMagic
            && header.version == LegacyV3SampleFileVersion
            && header.recordSize == sizeof(LegacyV3Sample)
            && header.capacity > 0;

    if (hasLegacyV3File) {
        if (migrateV3SampleFile()) { return true; }

        ESP_LOGE(TAG, "Refusing to replace v3 statistics samples after migration failure");
        return false;
    }

    if (!archiveIncompatibleFile(SampleFile)) { return false; }
    header.magic = SampleFileMagic;
    header.version = SampleFileVersion;
    header.recordSize = sizeof(Sample);
    header.capacity = _sampleCapacity;
    header.count = 0;
    header.nextIndex = 0;
    writeSampleHeader(header);
    return true;
}

bool StatisticsClass::archiveIncompatibleFile(char const* path) const
{
    if (!filesystem().exists(path)) { return true; }

    for (uint8_t i = 1; i < 100; ++i) {
        const String target = String(path) + ".v1." + String(static_cast<uint32_t>(i));
        if (filesystem().exists(target.c_str())) { continue; }
        if (filesystem().rename(path, target.c_str())) { return true; }
    }

    return false;
}

bool StatisticsClass::copyFile(char const* sourcePath, char const* targetPath) const
{
    auto source = filesystem().open(sourcePath, "r");
    if (!source) { return false; }

    auto target = filesystem().open(targetPath, "w");
    if (!target) {
        source.close();
        return false;
    }

    bool success = true;
    uint8_t buffer[256];
    while (source.available()) {
        const auto read = source.read(buffer, sizeof(buffer));
        if (read == 0) { break; }
        if (target.write(buffer, read) != read) {
            success = false;
            break;
        }
    }

    source.close();
    target.close();

    if (!success) {
        filesystem().remove(targetPath);
    }
    return success;
}

bool StatisticsClass::restoreFileFromArchive(char const* currentPath, char const* tempPath, String const& archivePath) const
{
    if (archivePath.length() == 0) { return false; }

    filesystem().remove(tempPath);
    if (!copyFile(archivePath.c_str(), tempPath)) {
        ESP_LOGW(TAG, "Failed to copy statistics archive %s", archivePath.c_str());
        return false;
    }

    String emptyPath;
    for (uint8_t i = 1; i < 100; ++i) {
        const String candidate = String(currentPath) + ".empty." + String(static_cast<uint32_t>(i));
        if (filesystem().exists(candidate.c_str())) { continue; }
        emptyPath = candidate;
        break;
    }
    if (emptyPath.length() == 0) {
        filesystem().remove(tempPath);
        return false;
    }

    const bool hadCurrent = filesystem().exists(currentPath);
    if (hadCurrent && !filesystem().rename(currentPath, emptyPath.c_str())) {
        filesystem().remove(tempPath);
        return false;
    }

    if (!filesystem().rename(tempPath, currentPath)) {
        if (hadCurrent) {
            filesystem().rename(emptyPath.c_str(), currentPath);
        }
        filesystem().remove(tempPath);
        return false;
    }

    ESP_LOGW(TAG, "Restored empty statistics file %s from archive %s", currentPath, archivePath.c_str());
    return true;
}

void StatisticsClass::recoverSampleFileReplacement() const
{
    if (!filesystem().exists(SampleFile) && filesystem().exists(SampleBackupFile)) {
        filesystem().rename(SampleBackupFile, SampleFile);
    }
}

bool StatisticsClass::readSampleHeaderFrom(char const* path, SampleFileHeader& header) const
{
    auto file = filesystem().open(path, "r");
    if (!file) { return false; }
    const auto read = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
    file.close();
    return read == sizeof(header);
}

bool StatisticsClass::readDailyHeaderFrom(char const* path, DailyFileHeader& header) const
{
    auto file = filesystem().open(path, "r");
    if (!file) { return false; }
    const auto read = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
    file.close();
    return read == sizeof(header);
}

bool StatisticsClass::isRecoverableSampleHeader(SampleFileHeader const& header) const
{
    if (header.magic != SampleFileMagic
            || header.capacity == 0
            || header.count == 0
            || header.count > header.capacity
            || header.nextIndex >= header.capacity) {
        return false;
    }

    if (header.version == SampleFileVersion && header.recordSize == sizeof(Sample)) { return true; }
    if (header.version == LegacyV4SampleFileVersion
            && (header.recordSize == sizeof(LegacyV4Sample)
                    || header.recordSize == sizeof(LegacyV4BoostSample))) {
        return true;
    }
    return header.version == LegacyV3SampleFileVersion && header.recordSize == sizeof(LegacyV3Sample);
}

bool StatisticsClass::isRecoverableDailyHeader(DailyFileHeader const& header) const
{
    if (header.magic != DailyFileMagic || header.capacity == 0) { return false; }
    if (header.version == DailyFileVersion && header.recordSize == sizeof(DailySummary)) { return true; }
    if (header.version == LegacyV4DailyFileVersion && header.recordSize == sizeof(LegacyV4DailySummary)) { return true; }
    return header.version == LegacyV3DailyFileVersion && header.recordSize == sizeof(LegacyV3DailySummary);
}

String StatisticsClass::findArchivedSampleFile() const
{
    String bestPath;
    uint32_t bestCount = 0;

    auto consider = [&](String const& path) {
        SampleFileHeader header;
        if (!readSampleHeaderFrom(path.c_str(), header) || !isRecoverableSampleHeader(header)) { return; }
        if (header.count <= bestCount) { return; }

        bestPath = path;
        bestCount = header.count;
    };

    consider(SampleBackupFile);
    consider(SampleTempFile);
    for (uint8_t i = 1; i < 100; ++i) {
        const String archivePath = String(SampleFile) + ".v1." + String(static_cast<uint32_t>(i));
        consider(archivePath);
        consider(archivePath + ".migrated");
        consider(String(SampleFile) + ".empty." + String(static_cast<uint32_t>(i)));
    }

    return bestPath;
}

uint32_t StatisticsClass::countDailyRecords(char const* path, DailyFileHeader const& header) const
{
    if (!isRecoverableDailyHeader(header)) { return 0; }

    auto file = filesystem().open(path, "r");
    if (!file) { return 0; }

    auto countRecords = [&](auto& sample) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < header.capacity; ++i) {
            file.seek(sizeof(DailyFileHeader) + (i * header.recordSize));
            const auto read = file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample));
            if (read != sizeof(sample)) { break; }
            if (sample.dayStart != 0 && sample.sampleCount > 0) { count++; }
        }
        return count;
    };

    uint32_t result = 0;
    if (header.recordSize == sizeof(DailySummary)) {
        DailySummary sample;
        result = countRecords(sample);
    } else if (header.recordSize == sizeof(LegacyV4DailySummary)) {
        LegacyV4DailySummary sample;
        result = countRecords(sample);
    } else if (header.recordSize == sizeof(LegacyV3DailySummary)) {
        LegacyV3DailySummary sample;
        result = countRecords(sample);
    }

    file.close();
    return result;
}

String StatisticsClass::findArchivedDailyFile() const
{
    String bestPath;
    uint32_t bestCount = 0;

    auto consider = [&](String const& path) {
        DailyFileHeader header;
        if (!readDailyHeaderFrom(path.c_str(), header) || !isRecoverableDailyHeader(header)) { return; }

        const auto recordCount = countDailyRecords(path.c_str(), header);
        if (recordCount <= bestCount) { return; }

        bestPath = path;
        bestCount = recordCount;
    };

    consider(DailyBackupFile);
    consider(DailyTempFile);
    for (uint8_t i = 1; i < 100; ++i) {
        const String archivePath = String(DailyFile) + ".v1." + String(static_cast<uint32_t>(i));
        consider(archivePath);
        consider(archivePath + ".migrated");
        consider(String(DailyFile) + ".empty." + String(static_cast<uint32_t>(i)));
    }

    return bestPath;
}

bool StatisticsClass::recoverArchivedSampleFileIfEmpty()
{
    SampleFileHeader header;
    const bool hasHeader = readSampleHeader(header);
    const auto archivePath = findArchivedSampleFile();
    if (archivePath.length() == 0) { return false; }

    SampleFileHeader archiveHeader;
    if (!readSampleHeaderFrom(archivePath.c_str(), archiveHeader) || !isRecoverableSampleHeader(archiveHeader)) {
        return false;
    }

    const uint32_t currentCount = hasHeader && header.magic == SampleFileMagic ? header.count : 0;
    if (currentCount >= archiveHeader.count) { return false; }

    return restoreFileFromArchive(SampleFile, SampleTempFile, archivePath);
}

bool StatisticsClass::recoverArchivedDailyFileIfEmpty()
{
    DailyFileHeader header;
    const bool hasHeader = readDailyHeaderFrom(DailyFile, header);
    const uint32_t currentCount = hasHeader ? countDailyRecords(DailyFile, header) : 0;

    const auto archivePath = findArchivedDailyFile();
    if (archivePath.length() == 0) { return false; }

    DailyFileHeader archiveHeader;
    if (!readDailyHeaderFrom(archivePath.c_str(), archiveHeader)) { return false; }
    if (currentCount >= countDailyRecords(archivePath.c_str(), archiveHeader)) { return false; }

    return restoreFileFromArchive(DailyFile, DailyTempFile, archivePath);
}

void StatisticsClass::recoverArchivedStatisticsFiles()
{
    recoverArchivedSampleFileIfEmpty();
    recoverArchivedDailyFileIfEmpty();
}

void StatisticsClass::migrateV2StatisticsFiles()
{
    auto sampleArchive = findV2Archive(SampleFile, SampleFileMagic, sizeof(LegacyV2Sample));
    if (sampleArchive.length() > 0) {
        _sampleCapacity = desiredSampleCapacity();
        migrateV2SampleFile(sampleArchive);
    }

    auto dailyArchive = findV2Archive(DailyFile, DailyFileMagic, sizeof(LegacyV2DailySummary));
    if (dailyArchive.length() > 0) {
        migrateV2DailyFile(dailyArchive);
    }
}

String StatisticsClass::findV2Archive(char const* path, uint32_t magic, uint16_t recordSize) const
{
    struct CommonHeader {
        uint32_t magic = 0;
        uint16_t version = 0;
        uint16_t recordSize = 0;
        uint32_t capacity = 0;
    };

    for (uint8_t i = 1; i < 100; ++i) {
        String archivedPath = String(path) + ".v1." + String(static_cast<uint32_t>(i));
        String migratedPath = archivedPath + ".migrated";
        if (!filesystem().exists(archivedPath.c_str()) || filesystem().exists(migratedPath.c_str())) {
            continue;
        }

        auto file = filesystem().open(archivedPath.c_str(), "r");
        if (!file) { continue; }
        CommonHeader header;
        const auto read = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
        file.close();

        if (read == sizeof(header)
                && header.magic == magic
                && header.version == LegacyV2FileVersion
                && header.recordSize == recordSize
                && header.capacity > 0) {
            return archivedPath;
        }
    }

    return String();
}

bool StatisticsClass::migrateV2SampleFile(String const& archivePath)
{
    if (_sampleCapacity == 0) { return false; }

    filesystem().remove(SampleTempFile);
    auto target = filesystem().open(SampleTempFile, "w");
    if (!target) { return false; }

    SampleFileHeader targetHeader;
    targetHeader.magic = SampleFileMagic;
    targetHeader.version = SampleFileVersion;
    targetHeader.recordSize = sizeof(Sample);
    targetHeader.capacity = _sampleCapacity;

    if (target.write(reinterpret_cast<uint8_t const*>(&targetHeader), sizeof(targetHeader)) != sizeof(targetHeader)) {
        target.close();
        filesystem().remove(SampleTempFile);
        return false;
    }

    uint32_t lastMigratedTimestamp = 0;
    auto appendSample = [&](Sample const& sample) -> bool {
        if (sample.timestamp == 0) { return true; }

        target.seek(sizeof(SampleFileHeader) + (targetHeader.nextIndex * sizeof(Sample)));
        if (target.write(reinterpret_cast<uint8_t const*>(&sample), sizeof(sample)) != sizeof(sample)) {
            return false;
        }

        targetHeader.nextIndex = (targetHeader.nextIndex + 1) % targetHeader.capacity;
        targetHeader.count = std::min<uint32_t>(targetHeader.count + 1, targetHeader.capacity);
        lastMigratedTimestamp = std::max(lastMigratedTimestamp, sample.timestamp);
        return true;
    };

    auto source = filesystem().open(archivePath.c_str(), "r");
    if (!source) {
        target.close();
        filesystem().remove(SampleTempFile);
        return false;
    }

    SampleFileHeader sourceHeader;
    auto read = source.read(reinterpret_cast<uint8_t*>(&sourceHeader), sizeof(sourceHeader));
    if (read != sizeof(sourceHeader)
            || sourceHeader.magic != SampleFileMagic
            || sourceHeader.version != LegacyV2FileVersion
            || sourceHeader.recordSize != sizeof(LegacyV2Sample)
            || sourceHeader.capacity == 0) {
        source.close();
        target.close();
        filesystem().remove(SampleTempFile);
        return false;
    }

    bool success = true;
    const auto legacyFirst = (sourceHeader.nextIndex + sourceHeader.capacity - sourceHeader.count) % sourceHeader.capacity;
    for (uint32_t i = 0; success && i < sourceHeader.count; ++i) {
        const auto index = (legacyFirst + i) % sourceHeader.capacity;
        LegacyV2Sample legacy;
        source.seek(sizeof(SampleFileHeader) + (index * sizeof(LegacyV2Sample)));
        read = source.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy));
        if (read != sizeof(legacy) || legacy.timestamp == 0) { continue; }

        Sample sample;
        sample.timestamp = legacy.timestamp;
        sample.batteryDischargePowerWatts = legacy.batteryDischargePowerWatts;
        sample.solarPowerWatts = legacy.solarPowerWatts;
        sample.batteryPowerWatts = legacy.batteryPowerWatts;
        sample.gridPowerWatts = legacy.gridPowerWatts;
        sample.gridChargerPowerWatts = legacy.gridChargerPowerWatts;
        sample.targetPowerWatts = legacy.targetPowerWatts;
        sample.gridImportPowerWatts = legacy.gridImportPowerWatts;
        sample.gridExportPowerWatts = legacy.gridExportPowerWatts;
        sample.batterySocPermille = legacy.batterySocPermille;
        sample.batteryVoltageDecivolt = legacy.batteryVoltageDecivolt;
        sample.batteryTemperatureDecicelsius = legacy.batteryTemperatureDecicelsius;
        sample.flags = legacy.flags;
        success = appendSample(sample);
    }
    source.close();

    SampleFileHeader currentHeader;
    auto current = filesystem().open(SampleFile, "r");
    if (success && current) {
        read = current.read(reinterpret_cast<uint8_t*>(&currentHeader), sizeof(currentHeader));
        if (read == sizeof(currentHeader)
                && currentHeader.magic == SampleFileMagic
                && currentHeader.version == SampleFileVersion
                && currentHeader.recordSize == sizeof(Sample)
                && currentHeader.capacity > 0) {
            const auto first = (currentHeader.nextIndex + currentHeader.capacity - currentHeader.count) % currentHeader.capacity;
            for (uint32_t i = 0; success && i < currentHeader.count; ++i) {
                const auto index = (first + i) % currentHeader.capacity;
                Sample sample;
                current.seek(sizeof(SampleFileHeader) + (index * sizeof(Sample)));
                read = current.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample));
                if (read == sizeof(sample) && sample.timestamp > lastMigratedTimestamp) {
                    success = appendSample(sample);
                }
            }
        } else if (read == sizeof(currentHeader)
                && currentHeader.magic == SampleFileMagic
                && currentHeader.version == LegacyV4SampleFileVersion
                && currentHeader.recordSize == sizeof(LegacyV4Sample)
                && currentHeader.capacity > 0) {
            const auto first = (currentHeader.nextIndex + currentHeader.capacity - currentHeader.count) % currentHeader.capacity;
            for (uint32_t i = 0; success && i < currentHeader.count; ++i) {
                const auto index = (first + i) % currentHeader.capacity;
                LegacyV4Sample legacy;
                current.seek(sizeof(SampleFileHeader) + (index * sizeof(LegacyV4Sample)));
                read = current.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy));
                if (read != sizeof(legacy) || legacy.timestamp <= lastMigratedTimestamp) { continue; }

                Sample sample;
                sample.timestamp = legacy.timestamp;
                sample.batteryDischargePowerWatts = legacy.batteryDischargePowerWatts;
                sample.solarPowerWatts = legacy.solarPowerWatts;
                sample.batteryPowerWatts = legacy.batteryPowerWatts;
                sample.gridPowerWatts = legacy.gridPowerWatts;
                sample.gridChargerPowerWatts = legacy.gridChargerPowerWatts;
                sample.targetPowerWatts = legacy.targetPowerWatts;
                for (uint8_t j = 0; j < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++j) {
                    sample.flexibleLoadPowerWatts[j] = legacy.flexibleLoadPowerWatts[j];
                }
                sample.gridImportPowerWatts = legacy.gridImportPowerWatts;
                sample.gridExportPowerWatts = legacy.gridExportPowerWatts;
                sample.batterySocPermille = legacy.batterySocPermille;
                sample.batteryVoltageDecivolt = legacy.batteryVoltageDecivolt;
                sample.batteryTemperatureDecicelsius = legacy.batteryTemperatureDecicelsius;
                for (uint8_t j = 0; j < INV_MAX_COUNT; ++j) {
                    sample.inverterTemperatureDecicelsius[j] = legacy.inverterTemperatureDecicelsius[j];
                }
                sample.inverterTemperatureFlags = legacy.inverterTemperatureFlags;
                sample.flags = legacy.flags;
                success = appendSample(sample);
            }
        } else if (read == sizeof(currentHeader)
                && currentHeader.magic == SampleFileMagic
                && currentHeader.version == LegacyV4SampleFileVersion
                && currentHeader.recordSize == sizeof(LegacyV4BoostSample)
                && currentHeader.capacity > 0) {
            const auto first = (currentHeader.nextIndex + currentHeader.capacity - currentHeader.count) % currentHeader.capacity;
            for (uint32_t i = 0; success && i < currentHeader.count; ++i) {
                const auto index = (first + i) % currentHeader.capacity;
                LegacyV4BoostSample legacy;
                current.seek(sizeof(SampleFileHeader) + (index * sizeof(LegacyV4BoostSample)));
                read = current.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy));
                if (read != sizeof(legacy) || legacy.timestamp <= lastMigratedTimestamp) { continue; }

                Sample sample;
                sample.timestamp = legacy.timestamp;
                sample.batteryDischargePowerWatts = legacy.batteryDischargePowerWatts;
                sample.solarPowerWatts = legacy.solarPowerWatts;
                sample.batteryPowerWatts = legacy.batteryPowerWatts;
                sample.gridPowerWatts = legacy.gridPowerWatts;
                sample.gridChargerPowerWatts = legacy.gridChargerPowerWatts;
                sample.targetPowerWatts = legacy.targetPowerWatts;
                for (uint8_t j = 0; j < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++j) {
                    sample.flexibleLoadPowerWatts[j] = legacy.flexibleLoadPowerWatts[j];
                }
                sample.gridImportPowerWatts = legacy.gridImportPowerWatts;
                sample.gridExportPowerWatts = legacy.gridExportPowerWatts;
                sample.batterySocPermille = legacy.batterySocPermille;
                sample.batteryVoltageDecivolt = legacy.batteryVoltageDecivolt;
                sample.batteryTemperatureDecicelsius = legacy.batteryTemperatureDecicelsius;
                sample.batteryBoostSavingsNormalPowerWatts = legacy.batteryBoostSavingsNormalPowerWatts;
                sample.batteryBoostSavingsRelaxedPowerWatts = legacy.batteryBoostSavingsRelaxedPowerWatts;
                sample.batteryBoostRelaxLimitedPowerWatts = legacy.batteryBoostRelaxLimitedPowerWatts;
                sample.batteryBoostSeconds = legacy.batteryBoostSeconds;
                sample.batteryBoostBudgetUsedPermille = legacy.batteryBoostBudgetUsedPermille;
                sample.flags = legacy.flags & ~LegacyBoostV4HasBatteryBoostSavings;
                if (legacy.flags & LegacyBoostV4HasBatteryBoostSavings) {
                    sample.flags |= HasBatteryBoostSavings;
                }
                success = appendSample(sample);
            }
        } else if (read == sizeof(currentHeader)
                && currentHeader.magic == SampleFileMagic
                && currentHeader.version == LegacyV3SampleFileVersion
                && currentHeader.recordSize == sizeof(LegacyV3Sample)
                && currentHeader.capacity > 0) {
            const auto first = (currentHeader.nextIndex + currentHeader.capacity - currentHeader.count) % currentHeader.capacity;
            for (uint32_t i = 0; success && i < currentHeader.count; ++i) {
                const auto index = (first + i) % currentHeader.capacity;
                LegacyV3Sample legacy;
                current.seek(sizeof(SampleFileHeader) + (index * sizeof(LegacyV3Sample)));
                read = current.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy));
                if (read != sizeof(legacy) || legacy.timestamp <= lastMigratedTimestamp) { continue; }

                Sample sample;
                sample.timestamp = legacy.timestamp;
                sample.batteryDischargePowerWatts = legacy.batteryDischargePowerWatts;
                sample.solarPowerWatts = legacy.solarPowerWatts;
                sample.batteryPowerWatts = legacy.batteryPowerWatts;
                sample.gridPowerWatts = legacy.gridPowerWatts;
                sample.gridChargerPowerWatts = legacy.gridChargerPowerWatts;
                sample.targetPowerWatts = legacy.targetPowerWatts;
                for (uint8_t j = 0; j < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++j) {
                    sample.flexibleLoadPowerWatts[j] = legacy.flexibleLoadPowerWatts[j];
                }
                sample.gridImportPowerWatts = legacy.gridImportPowerWatts;
                sample.gridExportPowerWatts = legacy.gridExportPowerWatts;
                sample.batterySocPermille = legacy.batterySocPermille;
                sample.batteryVoltageDecivolt = legacy.batteryVoltageDecivolt;
                sample.batteryTemperatureDecicelsius = legacy.batteryTemperatureDecicelsius;
                sample.flags = legacy.flags;
                success = appendSample(sample);
            }
        }
        current.close();
    }

    target.seek(0);
    success = success
        && target.write(reinterpret_cast<uint8_t const*>(&targetHeader), sizeof(targetHeader)) == sizeof(targetHeader);
    target.close();

    if (!success || targetHeader.count == 0) {
        filesystem().remove(SampleTempFile);
        return false;
    }

    filesystem().remove(SampleBackupFile);
    if (filesystem().exists(SampleFile) && !filesystem().rename(SampleFile, SampleBackupFile)) {
        filesystem().remove(SampleTempFile);
        return false;
    }
    if (!filesystem().rename(SampleTempFile, SampleFile)) {
        if (filesystem().exists(SampleBackupFile)) { filesystem().rename(SampleBackupFile, SampleFile); }
        filesystem().remove(SampleTempFile);
        return false;
    }

    filesystem().remove(SampleBackupFile);
    String migratedPath = archivePath + ".migrated";
    filesystem().rename(archivePath.c_str(), migratedPath.c_str());
    ESP_LOGI(TAG, "Migrated archived v2 statistics samples from %s", archivePath.c_str());
    return true;
}

bool StatisticsClass::migrateV4SampleFile()
{
    if (_sampleCapacity == 0) { return false; }

    auto source = filesystem().open(SampleFile, "r");
    if (!source) { return false; }

    SampleFileHeader sourceHeader;
    auto read = source.read(reinterpret_cast<uint8_t*>(&sourceHeader), sizeof(sourceHeader));
    if (read != sizeof(sourceHeader)
            || sourceHeader.magic != SampleFileMagic
            || sourceHeader.version != LegacyV4SampleFileVersion
            || (sourceHeader.recordSize != sizeof(LegacyV4Sample)
                    && sourceHeader.recordSize != sizeof(LegacyV4BoostSample))
            || sourceHeader.capacity == 0) {
        source.close();
        return false;
    }
    auto const hasLegacyBoostFields = sourceHeader.recordSize == sizeof(LegacyV4BoostSample);

    filesystem().remove(SampleTempFile);
    auto target = filesystem().open(SampleTempFile, "w");
    if (!target) {
        source.close();
        return false;
    }

    SampleFileHeader targetHeader;
    targetHeader.magic = SampleFileMagic;
    targetHeader.version = SampleFileVersion;
    targetHeader.recordSize = sizeof(Sample);
    targetHeader.capacity = _sampleCapacity;

    if (target.write(reinterpret_cast<uint8_t const*>(&targetHeader), sizeof(targetHeader)) != sizeof(targetHeader)) {
        source.close();
        target.close();
        filesystem().remove(SampleTempFile);
        return false;
    }

    auto appendSample = [&](Sample const& sample) -> bool {
        if (sample.timestamp == 0) { return true; }

        target.seek(sizeof(SampleFileHeader) + (targetHeader.nextIndex * sizeof(Sample)));
        if (target.write(reinterpret_cast<uint8_t const*>(&sample), sizeof(sample)) != sizeof(sample)) {
            return false;
        }

        targetHeader.nextIndex = (targetHeader.nextIndex + 1) % targetHeader.capacity;
        targetHeader.count = std::min<uint32_t>(targetHeader.count + 1, targetHeader.capacity);
        return true;
    };

    bool success = true;
    auto const samplesToCopy = std::min<uint32_t>(sourceHeader.count, targetHeader.capacity);
    auto const skippedSamples = sourceHeader.count - samplesToCopy;
    auto const first = (sourceHeader.nextIndex + sourceHeader.capacity - sourceHeader.count) % sourceHeader.capacity;
    for (uint32_t i = skippedSamples; success && i < sourceHeader.count; ++i) {
        const auto index = (first + i) % sourceHeader.capacity;
        Sample sample;
        if (hasLegacyBoostFields) {
            LegacyV4BoostSample legacy;
            source.seek(sizeof(SampleFileHeader) + (index * sizeof(LegacyV4BoostSample)));
            read = source.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy));
            if (read != sizeof(legacy) || legacy.timestamp == 0) { continue; }

            sample.timestamp = legacy.timestamp;
            sample.batteryDischargePowerWatts = legacy.batteryDischargePowerWatts;
            sample.solarPowerWatts = legacy.solarPowerWatts;
            sample.batteryPowerWatts = legacy.batteryPowerWatts;
            sample.gridPowerWatts = legacy.gridPowerWatts;
            sample.gridChargerPowerWatts = legacy.gridChargerPowerWatts;
            sample.targetPowerWatts = legacy.targetPowerWatts;
            for (uint8_t j = 0; j < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++j) {
                sample.flexibleLoadPowerWatts[j] = legacy.flexibleLoadPowerWatts[j];
            }
            sample.gridImportPowerWatts = legacy.gridImportPowerWatts;
            sample.gridExportPowerWatts = legacy.gridExportPowerWatts;
            sample.batterySocPermille = legacy.batterySocPermille;
            sample.batteryVoltageDecivolt = legacy.batteryVoltageDecivolt;
            sample.batteryTemperatureDecicelsius = legacy.batteryTemperatureDecicelsius;
            sample.batteryBoostSavingsNormalPowerWatts = legacy.batteryBoostSavingsNormalPowerWatts;
            sample.batteryBoostSavingsRelaxedPowerWatts = legacy.batteryBoostSavingsRelaxedPowerWatts;
            sample.batteryBoostRelaxLimitedPowerWatts = legacy.batteryBoostRelaxLimitedPowerWatts;
            sample.batteryBoostSeconds = legacy.batteryBoostSeconds;
            sample.batteryBoostBudgetUsedPermille = legacy.batteryBoostBudgetUsedPermille;
            sample.flags = legacy.flags & ~LegacyBoostV4HasBatteryBoostSavings;
            if (legacy.flags & LegacyBoostV4HasBatteryBoostSavings) {
                sample.flags |= HasBatteryBoostSavings;
            }
        } else {
            LegacyV4Sample legacy;
            source.seek(sizeof(SampleFileHeader) + (index * sizeof(LegacyV4Sample)));
            read = source.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy));
            if (read != sizeof(legacy) || legacy.timestamp == 0) { continue; }

            sample.timestamp = legacy.timestamp;
            sample.batteryDischargePowerWatts = legacy.batteryDischargePowerWatts;
            sample.solarPowerWatts = legacy.solarPowerWatts;
            sample.batteryPowerWatts = legacy.batteryPowerWatts;
            sample.gridPowerWatts = legacy.gridPowerWatts;
            sample.gridChargerPowerWatts = legacy.gridChargerPowerWatts;
            sample.targetPowerWatts = legacy.targetPowerWatts;
            for (uint8_t j = 0; j < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++j) {
                sample.flexibleLoadPowerWatts[j] = legacy.flexibleLoadPowerWatts[j];
            }
            sample.gridImportPowerWatts = legacy.gridImportPowerWatts;
            sample.gridExportPowerWatts = legacy.gridExportPowerWatts;
            sample.batterySocPermille = legacy.batterySocPermille;
            sample.batteryVoltageDecivolt = legacy.batteryVoltageDecivolt;
            sample.batteryTemperatureDecicelsius = legacy.batteryTemperatureDecicelsius;
            for (uint8_t j = 0; j < INV_MAX_COUNT; ++j) {
                sample.inverterTemperatureDecicelsius[j] = legacy.inverterTemperatureDecicelsius[j];
            }
            sample.inverterTemperatureFlags = legacy.inverterTemperatureFlags;
            sample.flags = legacy.flags;
        }
        success = appendSample(sample);
    }
    source.close();

    target.seek(0);
    success = success
        && target.write(reinterpret_cast<uint8_t const*>(&targetHeader), sizeof(targetHeader)) == sizeof(targetHeader);
    target.close();

    if (!success) {
        filesystem().remove(SampleTempFile);
        return false;
    }

    filesystem().remove(SampleBackupFile);
    if (!filesystem().rename(SampleFile, SampleBackupFile)) {
        filesystem().remove(SampleTempFile);
        return false;
    }
    if (!filesystem().rename(SampleTempFile, SampleFile)) {
        filesystem().rename(SampleBackupFile, SampleFile);
        filesystem().remove(SampleTempFile);
        return false;
    }

    filesystem().remove(SampleBackupFile);
    ESP_LOGI(TAG, "Migrated v4 statistics samples to v%u", SampleFileVersion);
    return true;
}

bool StatisticsClass::migrateV2DailyFile(String const& archivePath)
{
    auto merge = [](DailySummary& target, DailySummary const& source) {
        if (source.dayStart == 0 || source.sampleCount == 0) { return; }
        if (target.dayStart != source.dayStart || target.sampleCount == 0) {
            target = source;
            return;
        }

        target.from = target.from == 0 ? source.from : std::min(target.from, source.from);
        target.to = std::max(target.to, source.to);
        target.sampleCount += source.sampleCount;
        target.intervalCount += source.intervalCount;
        target.solarEnergyWh += source.solarEnergyWh;
        target.batteryChargeWh += source.batteryChargeWh;
        target.batteryDischargeWh += source.batteryDischargeWh;
        target.gridImportWh += source.gridImportWh;
        target.gridExportWh += source.gridExportWh;
        target.gridChargerEnergyWh += source.gridChargerEnergyWh;
        for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
            target.flexibleLoadEnergyWh[i] += source.flexibleLoadEnergyWh[i];
        }
        target.targetDeviationWh += source.targetDeviationWh;
        target.minBatterySoc = std::min(target.minBatterySoc, source.minBatterySoc);
        target.maxBatterySoc = std::max(target.maxBatterySoc, source.maxBatterySoc);
        target.minBatteryVoltage = std::min(target.minBatteryVoltage, source.minBatteryVoltage);
        target.maxBatteryVoltage = std::max(target.maxBatteryVoltage, source.maxBatteryVoltage);
        target.minBatteryTemperature = std::min(target.minBatteryTemperature, source.minBatteryTemperature);
        target.maxBatteryTemperature = std::max(target.maxBatteryTemperature, source.maxBatteryTemperature);
        target.maxGridImportWatts = std::max(target.maxGridImportWatts, source.maxGridImportWatts);
        target.maxGridExportWatts = std::max(target.maxGridExportWatts, source.maxGridExportWatts);
        target.maxBatteryChargeWatts = std::max(target.maxBatteryChargeWatts, source.maxBatteryChargeWatts);
        target.maxBatteryDischargeWatts = std::max(target.maxBatteryDischargeWatts, source.maxBatteryDischargeWatts);
        target.batterySocPercentSeconds += source.batterySocPercentSeconds;
        target.batterySocSeconds += source.batterySocSeconds;
        target.batteryBoostSavingsNormalWh += source.batteryBoostSavingsNormalWh;
        target.batteryBoostSavingsRelaxedWh += source.batteryBoostSavingsRelaxedWh;
        target.batteryBoostRelaxLimitedWh += source.batteryBoostRelaxLimitedWh;
        target.batteryBoostSeconds += source.batteryBoostSeconds;
    };

    filesystem().remove(DailyTempFile);
    auto target = filesystem().open(DailyTempFile, "w+");
    if (!target) { return false; }

    DailyFileHeader targetHeader;
    targetHeader.magic = DailyFileMagic;
    targetHeader.version = DailyFileVersion;
    targetHeader.recordSize = sizeof(DailySummary);
    targetHeader.capacity = DailyRetentionDays;
    if (target.write(reinterpret_cast<uint8_t const*>(&targetHeader), sizeof(targetHeader)) != sizeof(targetHeader)) {
        target.close();
        filesystem().remove(DailyTempFile);
        return false;
    }

    DailySummary empty;
    for (uint16_t i = 0; i < DailyRetentionDays; ++i) {
        if (target.write(reinterpret_cast<uint8_t const*>(&empty), sizeof(empty)) != sizeof(empty)) {
            target.close();
            filesystem().remove(DailyTempFile);
            return false;
        }
    }
    target.flush();

    auto writeMerged = [&](DailySummary const& summary) -> bool {
        if (summary.dayStart == 0 || summary.sampleCount == 0) { return true; }

        const auto index = (summary.dayStart / 86400) % DailyRetentionDays;
        DailySummary existing;
        target.seek(sizeof(DailyFileHeader) + (index * sizeof(DailySummary)));
        auto read = target.read(reinterpret_cast<uint8_t*>(&existing), sizeof(existing));
        if (read != sizeof(existing)) { return false; }

        merge(existing, summary);

        target.seek(sizeof(DailyFileHeader) + (index * sizeof(DailySummary)));
        return target.write(reinterpret_cast<uint8_t const*>(&existing), sizeof(existing)) == sizeof(existing);
    };

    auto source = filesystem().open(archivePath.c_str(), "r");
    if (!source) {
        target.close();
        filesystem().remove(DailyTempFile);
        return false;
    }

    DailyFileHeader sourceHeader;
    auto read = source.read(reinterpret_cast<uint8_t*>(&sourceHeader), sizeof(sourceHeader));
    if (read != sizeof(sourceHeader)
            || sourceHeader.magic != DailyFileMagic
            || sourceHeader.version != LegacyV2FileVersion
            || sourceHeader.recordSize != sizeof(LegacyV2DailySummary)
            || sourceHeader.capacity == 0) {
        source.close();
        target.close();
        filesystem().remove(DailyTempFile);
        return false;
    }

    bool success = true;
    const auto legacyCapacity = std::min<uint32_t>(sourceHeader.capacity, DailyRetentionDays);
    for (uint32_t i = 0; success && i < legacyCapacity; ++i) {
        LegacyV2DailySummary legacy;
        source.seek(sizeof(DailyFileHeader) + (i * sizeof(LegacyV2DailySummary)));
        read = source.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy));
        if (read != sizeof(legacy) || legacy.dayStart == 0 || legacy.sampleCount == 0) { continue; }

        DailySummary summary;
        summary.from = legacy.from;
        summary.to = legacy.to;
        summary.sampleCount = legacy.sampleCount;
        summary.intervalCount = legacy.intervalCount;
        summary.solarEnergyWh = legacy.solarEnergyWh;
        summary.batteryChargeWh = legacy.batteryChargeWh;
        summary.batteryDischargeWh = legacy.batteryDischargeWh;
        summary.gridImportWh = legacy.gridImportWh;
        summary.gridExportWh = legacy.gridExportWh;
        summary.gridChargerEnergyWh = legacy.gridChargerEnergyWh;
        summary.targetDeviationWh = legacy.targetDeviationWh;
        summary.minBatterySoc = legacy.minBatterySoc;
        summary.maxBatterySoc = legacy.maxBatterySoc;
        summary.minBatteryVoltage = legacy.minBatteryVoltage;
        summary.maxBatteryVoltage = legacy.maxBatteryVoltage;
        summary.minBatteryTemperature = legacy.minBatteryTemperature;
        summary.maxBatteryTemperature = legacy.maxBatteryTemperature;
        summary.maxGridImportWatts = legacy.maxGridImportWatts;
        summary.maxGridExportWatts = legacy.maxGridExportWatts;
        summary.maxBatteryChargeWatts = legacy.maxBatteryChargeWatts;
        summary.maxBatteryDischargeWatts = legacy.maxBatteryDischargeWatts;
        summary.dayStart = legacy.dayStart;
        success = writeMerged(summary);
    }
    source.close();

    DailyFileHeader currentHeader;
    auto current = filesystem().open(DailyFile, "r");
    if (success && current) {
        read = current.read(reinterpret_cast<uint8_t*>(&currentHeader), sizeof(currentHeader));
        if (read == sizeof(currentHeader)
                && currentHeader.magic == DailyFileMagic
                && currentHeader.version == DailyFileVersion
                && currentHeader.recordSize == sizeof(DailySummary)
                && currentHeader.capacity == DailyRetentionDays) {
            for (uint32_t i = 0; success && i < currentHeader.capacity; ++i) {
                DailySummary summary;
                current.seek(sizeof(DailyFileHeader) + (i * sizeof(DailySummary)));
                read = current.read(reinterpret_cast<uint8_t*>(&summary), sizeof(summary));
                if (read == sizeof(summary)) { success = writeMerged(summary); }
            }
        } else if (read == sizeof(currentHeader)
                && currentHeader.magic == DailyFileMagic
                && currentHeader.version == LegacyV4DailyFileVersion
                && currentHeader.recordSize == sizeof(LegacyV4DailySummary)
                && currentHeader.capacity > 0) {
            const auto currentCapacity = std::min<uint32_t>(currentHeader.capacity, DailyRetentionDays);
            for (uint32_t i = 0; success && i < currentCapacity; ++i) {
                LegacyV4DailySummary legacy;
                current.seek(sizeof(DailyFileHeader) + (i * sizeof(LegacyV4DailySummary)));
                read = current.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy));
                if (read != sizeof(legacy) || legacy.dayStart == 0 || legacy.sampleCount == 0) { continue; }

                DailySummary summary;
                summary.from = legacy.from;
                summary.to = legacy.to;
                summary.sampleCount = legacy.sampleCount;
                summary.intervalCount = legacy.intervalCount;
                summary.solarEnergyWh = legacy.solarEnergyWh;
                summary.batteryChargeWh = legacy.batteryChargeWh;
                summary.batteryDischargeWh = legacy.batteryDischargeWh;
                summary.gridImportWh = legacy.gridImportWh;
                summary.gridExportWh = legacy.gridExportWh;
                summary.gridChargerEnergyWh = legacy.gridChargerEnergyWh;
                for (uint8_t j = 0; j < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++j) {
                    summary.flexibleLoadEnergyWh[j] = legacy.flexibleLoadEnergyWh[j];
                }
                summary.targetDeviationWh = legacy.targetDeviationWh;
                summary.minBatterySoc = legacy.minBatterySoc;
                summary.maxBatterySoc = legacy.maxBatterySoc;
                summary.minBatteryVoltage = legacy.minBatteryVoltage;
                summary.maxBatteryVoltage = legacy.maxBatteryVoltage;
                summary.minBatteryTemperature = legacy.minBatteryTemperature;
                summary.maxBatteryTemperature = legacy.maxBatteryTemperature;
                summary.maxGridImportWatts = legacy.maxGridImportWatts;
                summary.maxGridExportWatts = legacy.maxGridExportWatts;
                summary.maxBatteryChargeWatts = legacy.maxBatteryChargeWatts;
                summary.maxBatteryDischargeWatts = legacy.maxBatteryDischargeWatts;
                summary.batterySocPercentSeconds = legacy.batterySocPercentSeconds;
                summary.batterySocSeconds = legacy.batterySocSeconds;
                summary.dayStart = legacy.dayStart;
                success = writeMerged(summary);
            }
        } else if (read == sizeof(currentHeader)
                && currentHeader.magic == DailyFileMagic
                && currentHeader.version == LegacyV3DailyFileVersion
                && currentHeader.recordSize == sizeof(LegacyV3DailySummary)
                && currentHeader.capacity > 0) {
            const auto currentCapacity = std::min<uint32_t>(currentHeader.capacity, DailyRetentionDays);
            for (uint32_t i = 0; success && i < currentCapacity; ++i) {
                LegacyV3DailySummary legacy;
                current.seek(sizeof(DailyFileHeader) + (i * sizeof(LegacyV3DailySummary)));
                read = current.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy));
                if (read != sizeof(legacy) || legacy.dayStart == 0 || legacy.sampleCount == 0) { continue; }

                DailySummary summary;
                summary.from = legacy.from;
                summary.to = legacy.to;
                summary.sampleCount = legacy.sampleCount;
                summary.intervalCount = legacy.intervalCount;
                summary.solarEnergyWh = legacy.solarEnergyWh;
                summary.batteryChargeWh = legacy.batteryChargeWh;
                summary.batteryDischargeWh = legacy.batteryDischargeWh;
                summary.gridImportWh = legacy.gridImportWh;
                summary.gridExportWh = legacy.gridExportWh;
                summary.gridChargerEnergyWh = legacy.gridChargerEnergyWh;
                for (uint8_t j = 0; j < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++j) {
                    summary.flexibleLoadEnergyWh[j] = legacy.flexibleLoadEnergyWh[j];
                }
                summary.targetDeviationWh = legacy.targetDeviationWh;
                summary.minBatterySoc = legacy.minBatterySoc;
                summary.maxBatterySoc = legacy.maxBatterySoc;
                summary.minBatteryVoltage = legacy.minBatteryVoltage;
                summary.maxBatteryVoltage = legacy.maxBatteryVoltage;
                summary.minBatteryTemperature = legacy.minBatteryTemperature;
                summary.maxBatteryTemperature = legacy.maxBatteryTemperature;
                summary.maxGridImportWatts = legacy.maxGridImportWatts;
                summary.maxGridExportWatts = legacy.maxGridExportWatts;
                summary.maxBatteryChargeWatts = legacy.maxBatteryChargeWatts;
                summary.maxBatteryDischargeWatts = legacy.maxBatteryDischargeWatts;
                summary.dayStart = legacy.dayStart;
                success = writeMerged(summary);
            }
        }
        current.close();
    }

    target.close();
    if (!success) {
        filesystem().remove(DailyTempFile);
        return false;
    }

    filesystem().remove(DailyBackupFile);
    if (filesystem().exists(DailyFile) && !filesystem().rename(DailyFile, DailyBackupFile)) {
        filesystem().remove(DailyTempFile);
        return false;
    }
    if (!filesystem().rename(DailyTempFile, DailyFile)) {
        if (filesystem().exists(DailyBackupFile)) { filesystem().rename(DailyBackupFile, DailyFile); }
        filesystem().remove(DailyTempFile);
        return false;
    }

    filesystem().remove(DailyBackupFile);
    String migratedPath = archivePath + ".migrated";
    filesystem().rename(archivePath.c_str(), migratedPath.c_str());
    ESP_LOGI(TAG, "Migrated archived v2 daily statistics from %s", archivePath.c_str());
    return true;
}

bool StatisticsClass::migrateV3SampleFile()
{
    if (_sampleCapacity == 0) { return false; }

    auto source = filesystem().open(SampleFile, "r");
    if (!source) { return false; }

    SampleFileHeader sourceHeader;
    auto read = source.read(reinterpret_cast<uint8_t*>(&sourceHeader), sizeof(sourceHeader));
    if (read != sizeof(sourceHeader)
            || sourceHeader.magic != SampleFileMagic
            || sourceHeader.version != LegacyV3SampleFileVersion
            || sourceHeader.recordSize != sizeof(LegacyV3Sample)
            || sourceHeader.capacity == 0
            || sourceHeader.nextIndex >= sourceHeader.capacity) {
        source.close();
        return false;
    }

    filesystem().remove(SampleTempFile);
    auto target = filesystem().open(SampleTempFile, "w+");
    if (!target) {
        source.close();
        return false;
    }

    SampleFileHeader targetHeader;
    targetHeader.magic = SampleFileMagic;
    targetHeader.version = SampleFileVersion;
    targetHeader.recordSize = sizeof(Sample);
    targetHeader.capacity = _sampleCapacity;
    if (target.write(reinterpret_cast<uint8_t const*>(&targetHeader), sizeof(targetHeader)) != sizeof(targetHeader)) {
        source.close();
        target.close();
        filesystem().remove(SampleTempFile);
        return false;
    }

    auto appendSample = [&](Sample const& sample) -> bool {
        if (sample.timestamp == 0) { return true; }

        target.seek(sizeof(SampleFileHeader) + (targetHeader.nextIndex * sizeof(Sample)));
        if (target.write(reinterpret_cast<uint8_t const*>(&sample), sizeof(sample)) != sizeof(sample)) {
            return false;
        }

        targetHeader.nextIndex = (targetHeader.nextIndex + 1) % targetHeader.capacity;
        targetHeader.count = std::min<uint32_t>(targetHeader.count + 1, targetHeader.capacity);
        return true;
    };

    bool success = true;
    const auto sourceCount = std::min(sourceHeader.count, sourceHeader.capacity);
    const auto first = (sourceHeader.nextIndex + sourceHeader.capacity - sourceCount) % sourceHeader.capacity;
    for (uint32_t i = 0; success && i < sourceCount; ++i) {
        const auto index = (first + i) % sourceHeader.capacity;
        LegacyV3Sample legacy;
        source.seek(sizeof(SampleFileHeader) + (index * sizeof(LegacyV3Sample)));
        read = source.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy));
        if (read != sizeof(legacy)) {
            success = false;
            break;
        }
        if (legacy.timestamp == 0) { continue; }

        Sample sample;
        sample.timestamp = legacy.timestamp;
        sample.batteryDischargePowerWatts = legacy.batteryDischargePowerWatts;
        sample.solarPowerWatts = legacy.solarPowerWatts;
        sample.batteryPowerWatts = legacy.batteryPowerWatts;
        sample.gridPowerWatts = legacy.gridPowerWatts;
        sample.gridChargerPowerWatts = legacy.gridChargerPowerWatts;
        sample.targetPowerWatts = legacy.targetPowerWatts;
        for (uint8_t load = 0; load < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++load) {
            sample.flexibleLoadPowerWatts[load] = legacy.flexibleLoadPowerWatts[load];
        }
        sample.gridImportPowerWatts = legacy.gridImportPowerWatts;
        sample.gridExportPowerWatts = legacy.gridExportPowerWatts;
        sample.batterySocPermille = legacy.batterySocPermille;
        sample.batteryVoltageDecivolt = legacy.batteryVoltageDecivolt;
        sample.batteryTemperatureDecicelsius = legacy.batteryTemperatureDecicelsius;
        sample.flags = legacy.flags;
        success = appendSample(sample);
    }
    source.close();

    if (success) {
        target.seek(0);
        success = target.write(reinterpret_cast<uint8_t const*>(&targetHeader), sizeof(targetHeader)) == sizeof(targetHeader);
    }
    target.close();

    if (!success) {
        filesystem().remove(SampleTempFile);
        return false;
    }

    filesystem().remove(SampleBackupFile);
    if (!filesystem().rename(SampleFile, SampleBackupFile)) {
        filesystem().remove(SampleTempFile);
        return false;
    }

    if (!filesystem().rename(SampleTempFile, SampleFile)) {
        filesystem().rename(SampleBackupFile, SampleFile);
        filesystem().remove(SampleTempFile);
        return false;
    }

    filesystem().remove(SampleBackupFile);
    _sampleCapacity = targetHeader.capacity;
    ESP_LOGI(TAG, "Migrated v3 statistics samples to v%u", SampleFileVersion);
    return true;
}

bool StatisticsClass::expandSampleFile(SampleFileHeader const& header, uint32_t capacity) const
{
    filesystem().remove(SampleTempFile);
    filesystem().remove(SampleBackupFile);

    auto target = filesystem().open(SampleTempFile, "w");
    if (!target) { return false; }

    SampleFileHeader expanded = header;
    expanded.capacity = capacity;
    expanded.count = 0;
    expanded.nextIndex = 0;

    if (target.write(reinterpret_cast<uint8_t const*>(&expanded), sizeof(expanded)) != sizeof(expanded)) {
        target.close();
        filesystem().remove(SampleTempFile);
        return false;
    }

    uint32_t written = 0;
    forEachRecentSample([&](Sample const& sample) {
        if (written >= header.count) { return; }
        if (target.write(reinterpret_cast<uint8_t const*>(&sample), sizeof(sample)) == sizeof(sample)) {
            written++;
        }
    });

    target.close();

    if (written != header.count) {
        filesystem().remove(SampleTempFile);
        return false;
    }

    expanded.count = written;
    expanded.nextIndex = written % capacity;
    auto update = filesystem().open(SampleTempFile, "r+");
    if (!update) {
        filesystem().remove(SampleTempFile);
        return false;
    }
    update.seek(0);
    const auto updated = update.write(reinterpret_cast<uint8_t const*>(&expanded), sizeof(expanded));
    update.close();
    if (updated != sizeof(expanded)) {
        filesystem().remove(SampleTempFile);
        return false;
    }

    if (!filesystem().rename(SampleFile, SampleBackupFile)) {
        filesystem().remove(SampleTempFile);
        return false;
    }

    if (!filesystem().rename(SampleTempFile, SampleFile)) {
        filesystem().rename(SampleBackupFile, SampleFile);
        filesystem().remove(SampleTempFile);
        return false;
    }

    filesystem().remove(SampleBackupFile);
    return true;
}

bool StatisticsClass::migrateV4DailyFile()
{
    auto source = filesystem().open(DailyFile, "r");
    if (!source) { return false; }

    DailyFileHeader sourceHeader;
    auto read = source.read(reinterpret_cast<uint8_t*>(&sourceHeader), sizeof(sourceHeader));
    if (read != sizeof(sourceHeader)
            || sourceHeader.magic != DailyFileMagic
            || sourceHeader.version != LegacyV4DailyFileVersion
            || sourceHeader.recordSize != sizeof(LegacyV4DailySummary)
            || sourceHeader.capacity == 0) {
        source.close();
        return false;
    }

    filesystem().remove(DailyTempFile);
    auto target = filesystem().open(DailyTempFile, "w+");
    if (!target) {
        source.close();
        return false;
    }

    DailyFileHeader targetHeader;
    targetHeader.magic = DailyFileMagic;
    targetHeader.version = DailyFileVersion;
    targetHeader.recordSize = sizeof(DailySummary);
    targetHeader.capacity = DailyRetentionDays;
    if (target.write(reinterpret_cast<uint8_t const*>(&targetHeader), sizeof(targetHeader)) != sizeof(targetHeader)) {
        source.close();
        target.close();
        filesystem().remove(DailyTempFile);
        return false;
    }

    DailySummary empty;
    for (uint16_t i = 0; i < DailyRetentionDays; ++i) {
        if (target.write(reinterpret_cast<uint8_t const*>(&empty), sizeof(empty)) != sizeof(empty)) {
            source.close();
            target.close();
            filesystem().remove(DailyTempFile);
            return false;
        }
    }
    target.flush();

    auto writeMigrated = [&](LegacyV4DailySummary const& legacy) -> bool {
        if (legacy.dayStart == 0 || legacy.sampleCount == 0) { return true; }

        DailySummary summary;
        summary.from = legacy.from;
        summary.to = legacy.to;
        summary.sampleCount = legacy.sampleCount;
        summary.intervalCount = legacy.intervalCount;
        summary.solarEnergyWh = legacy.solarEnergyWh;
        summary.batteryChargeWh = legacy.batteryChargeWh;
        summary.batteryDischargeWh = legacy.batteryDischargeWh;
        summary.gridImportWh = legacy.gridImportWh;
        summary.gridExportWh = legacy.gridExportWh;
        summary.gridChargerEnergyWh = legacy.gridChargerEnergyWh;
        for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
            summary.flexibleLoadEnergyWh[i] = legacy.flexibleLoadEnergyWh[i];
        }
        summary.targetDeviationWh = legacy.targetDeviationWh;
        summary.minBatterySoc = legacy.minBatterySoc;
        summary.maxBatterySoc = legacy.maxBatterySoc;
        summary.minBatteryVoltage = legacy.minBatteryVoltage;
        summary.maxBatteryVoltage = legacy.maxBatteryVoltage;
        summary.minBatteryTemperature = legacy.minBatteryTemperature;
        summary.maxBatteryTemperature = legacy.maxBatteryTemperature;
        summary.maxGridImportWatts = legacy.maxGridImportWatts;
        summary.maxGridExportWatts = legacy.maxGridExportWatts;
        summary.maxBatteryChargeWatts = legacy.maxBatteryChargeWatts;
        summary.maxBatteryDischargeWatts = legacy.maxBatteryDischargeWatts;
        summary.batterySocPercentSeconds = legacy.batterySocPercentSeconds;
        summary.batterySocSeconds = legacy.batterySocSeconds;
        summary.dayStart = legacy.dayStart;

        const auto index = (summary.dayStart / 86400) % DailyRetentionDays;
        target.seek(sizeof(DailyFileHeader) + (index * sizeof(DailySummary)));
        return target.write(reinterpret_cast<uint8_t const*>(&summary), sizeof(summary)) == sizeof(summary);
    };

    bool success = true;
    const auto capacity = std::min<uint32_t>(sourceHeader.capacity, DailyRetentionDays);
    for (uint32_t i = 0; success && i < capacity; ++i) {
        LegacyV4DailySummary legacy;
        source.seek(sizeof(DailyFileHeader) + (i * sizeof(LegacyV4DailySummary)));
        read = source.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy));
        if (read == sizeof(legacy)) {
            success = writeMigrated(legacy);
        } else {
            success = false;
        }
    }

    source.close();
    target.close();
    if (!success) {
        filesystem().remove(DailyTempFile);
        return false;
    }

    filesystem().remove(DailyBackupFile);
    if (!filesystem().rename(DailyFile, DailyBackupFile)) {
        filesystem().remove(DailyTempFile);
        return false;
    }
    if (!filesystem().rename(DailyTempFile, DailyFile)) {
        filesystem().rename(DailyBackupFile, DailyFile);
        filesystem().remove(DailyTempFile);
        return false;
    }

    filesystem().remove(DailyBackupFile);
    ESP_LOGI(TAG, "Migrated v4 daily statistics to v%u", DailyFileVersion);
    return true;
}

bool StatisticsClass::migrateV3DailyFile()
{
    auto source = filesystem().open(DailyFile, "r");
    if (!source) { return false; }

    DailyFileHeader sourceHeader;
    auto read = source.read(reinterpret_cast<uint8_t*>(&sourceHeader), sizeof(sourceHeader));
    if (read != sizeof(sourceHeader)
            || sourceHeader.magic != DailyFileMagic
            || sourceHeader.version != LegacyV3DailyFileVersion
            || sourceHeader.recordSize != sizeof(LegacyV3DailySummary)
            || sourceHeader.capacity == 0) {
        source.close();
        return false;
    }

    filesystem().remove(DailyTempFile);
    auto target = filesystem().open(DailyTempFile, "w+");
    if (!target) {
        source.close();
        return false;
    }

    DailyFileHeader targetHeader;
    targetHeader.magic = DailyFileMagic;
    targetHeader.version = DailyFileVersion;
    targetHeader.recordSize = sizeof(DailySummary);
    targetHeader.capacity = DailyRetentionDays;
    if (target.write(reinterpret_cast<uint8_t const*>(&targetHeader), sizeof(targetHeader)) != sizeof(targetHeader)) {
        source.close();
        target.close();
        filesystem().remove(DailyTempFile);
        return false;
    }

    DailySummary empty;
    for (uint16_t i = 0; i < DailyRetentionDays; ++i) {
        if (target.write(reinterpret_cast<uint8_t const*>(&empty), sizeof(empty)) != sizeof(empty)) {
            source.close();
            target.close();
            filesystem().remove(DailyTempFile);
            return false;
        }
    }
    target.flush();

    auto writeMigrated = [&](LegacyV3DailySummary const& legacy) -> bool {
        if (legacy.dayStart == 0 || legacy.sampleCount == 0) { return true; }

        DailySummary summary;
        summary.from = legacy.from;
        summary.to = legacy.to;
        summary.sampleCount = legacy.sampleCount;
        summary.intervalCount = legacy.intervalCount;
        summary.solarEnergyWh = legacy.solarEnergyWh;
        summary.batteryChargeWh = legacy.batteryChargeWh;
        summary.batteryDischargeWh = legacy.batteryDischargeWh;
        summary.gridImportWh = legacy.gridImportWh;
        summary.gridExportWh = legacy.gridExportWh;
        summary.gridChargerEnergyWh = legacy.gridChargerEnergyWh;
        for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
            summary.flexibleLoadEnergyWh[i] = legacy.flexibleLoadEnergyWh[i];
        }
        summary.targetDeviationWh = legacy.targetDeviationWh;
        summary.minBatterySoc = legacy.minBatterySoc;
        summary.maxBatterySoc = legacy.maxBatterySoc;
        summary.minBatteryVoltage = legacy.minBatteryVoltage;
        summary.maxBatteryVoltage = legacy.maxBatteryVoltage;
        summary.minBatteryTemperature = legacy.minBatteryTemperature;
        summary.maxBatteryTemperature = legacy.maxBatteryTemperature;
        summary.maxGridImportWatts = legacy.maxGridImportWatts;
        summary.maxGridExportWatts = legacy.maxGridExportWatts;
        summary.maxBatteryChargeWatts = legacy.maxBatteryChargeWatts;
        summary.maxBatteryDischargeWatts = legacy.maxBatteryDischargeWatts;
        summary.dayStart = legacy.dayStart;

        const auto index = (summary.dayStart / 86400) % DailyRetentionDays;
        target.seek(sizeof(DailyFileHeader) + (index * sizeof(DailySummary)));
        return target.write(reinterpret_cast<uint8_t const*>(&summary), sizeof(summary)) == sizeof(summary);
    };

    bool success = true;
    const auto capacity = std::min<uint32_t>(sourceHeader.capacity, DailyRetentionDays);
    for (uint32_t i = 0; success && i < capacity; ++i) {
        LegacyV3DailySummary legacy;
        source.seek(sizeof(DailyFileHeader) + (i * sizeof(LegacyV3DailySummary)));
        read = source.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy));
        if (read == sizeof(legacy)) {
            success = writeMigrated(legacy);
        } else {
            success = false;
        }
    }

    source.close();
    target.close();
    if (!success) {
        filesystem().remove(DailyTempFile);
        return false;
    }

    filesystem().remove(DailyBackupFile);
    if (!filesystem().rename(DailyFile, DailyBackupFile)) {
        filesystem().remove(DailyTempFile);
        return false;
    }
    if (!filesystem().rename(DailyTempFile, DailyFile)) {
        filesystem().rename(DailyBackupFile, DailyFile);
        filesystem().remove(DailyTempFile);
        return false;
    }

    filesystem().remove(DailyBackupFile);
    ESP_LOGI(TAG, "Migrated v3 daily statistics to v%u", DailyFileVersion);
    return true;
}

bool StatisticsClass::initDailyFile()
{
    DailyFileHeader header;
    const bool valid = [&]() {
        auto file = filesystem().open(DailyFile, "r");
        if (!file) { return false; }
        const auto read = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
        file.close();
        return read == sizeof(header)
            && header.magic == DailyFileMagic
            && header.version == DailyFileVersion
            && header.recordSize == sizeof(DailySummary)
            && header.capacity == DailyRetentionDays;
    }();

    if (valid) { return true; }

    const bool hasLegacyV4File = header.magic == DailyFileMagic
        && header.version == LegacyV4DailyFileVersion
        && header.recordSize == sizeof(LegacyV4DailySummary)
        && header.capacity > 0;

    if (hasLegacyV4File) {
        if (migrateV4DailyFile()) { return true; }

        ESP_LOGE(TAG, "Refusing to replace v4 daily statistics after migration failure");
        return false;
    }

    const bool hasLegacyV3File = header.magic == DailyFileMagic
        && header.version == LegacyV3DailyFileVersion
        && header.recordSize == sizeof(LegacyV3DailySummary)
        && header.capacity > 0;

    if (hasLegacyV3File) {
        if (migrateV3DailyFile()) { return true; }

        ESP_LOGE(TAG, "Refusing to replace v3 daily statistics after migration failure");
        return false;
    }

    if (!archiveIncompatibleFile(DailyFile)) { return false; }
    auto file = filesystem().open(DailyFile, "w");
    if (!file) { return false; }

    header.magic = DailyFileMagic;
    header.version = DailyFileVersion;
    header.recordSize = sizeof(DailySummary);
    header.capacity = DailyRetentionDays;
    file.write(reinterpret_cast<uint8_t const*>(&header), sizeof(header));

    DailySummary empty;
    for (uint16_t i = 0; i < DailyRetentionDays; ++i) {
        file.write(reinterpret_cast<uint8_t const*>(&empty), sizeof(empty));
    }

    file.close();
    return true;
}

uint32_t StatisticsClass::desiredPanelSampleCapacity() const
{
    const auto total = filesystem().totalBytes();
    if (total == 0) { return 0; }

    const auto reserve = std::min<size_t>(128 * 1024, total / 4);
    const auto panelBytes = fileSize(PanelSampleFile) + fileSize(PanelDailyFile);
    const auto usedWithoutPanelFiles = filesystem().usedBytes() > panelBytes
        ? filesystem().usedBytes() - panelBytes
        : 0;
    const auto dailyBytes = sizeof(PanelDailyFileHeader) + (DailyRetentionDays * sizeof(PanelDailySummary));
    if (total <= usedWithoutPanelFiles + reserve + dailyBytes) { return 0; }

    const auto availableForSamples = total - usedWithoutPanelFiles - reserve - dailyBytes;
    const auto maxSamples = DesiredSampleRetentionDays * 24 * 60 * 60 / SampleIntervalSeconds;
    return static_cast<uint32_t>(std::min<size_t>(maxSamples, availableForSamples / sizeof(PanelSample)));
}

bool StatisticsClass::initPanelSampleFile()
{
    const auto capacity = desiredPanelSampleCapacity();
    if (capacity == 0) { return false; }

    PanelSampleFileHeader header;
    const bool valid = readPanelSampleHeader(header)
        && header.magic == PanelSampleFileMagic
        && header.version == PanelSampleFileVersion
        && header.recordSize == sizeof(PanelSample)
        && header.capacity > 0
        && header.count <= header.capacity
        && header.nextIndex < header.capacity;

    if (valid) { return true; }

    if (!archiveIncompatibleFile(PanelSampleFile)) { return false; }

    header.magic = PanelSampleFileMagic;
    header.version = PanelSampleFileVersion;
    header.recordSize = sizeof(PanelSample);
    header.capacity = capacity;
    header.count = 0;
    header.nextIndex = 0;
    writePanelSampleHeader(header);
    return true;
}

bool StatisticsClass::initPanelDailyFile()
{
    PanelDailyFileHeader header;
    const bool valid = [&]() {
        auto file = filesystem().open(PanelDailyFile, "r");
        if (!file) { return false; }
        const auto read = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
        file.close();
        return read == sizeof(header)
            && header.magic == PanelDailyFileMagic
            && header.version == PanelDailyFileVersion
            && header.recordSize == sizeof(PanelDailySummary)
            && header.capacity == DailyRetentionDays;
    }();

    if (valid) { return true; }

    if (!archiveIncompatibleFile(PanelDailyFile)) { return false; }
    auto file = filesystem().open(PanelDailyFile, "w");
    if (!file) { return false; }

    header.magic = PanelDailyFileMagic;
    header.version = PanelDailyFileVersion;
    header.recordSize = sizeof(PanelDailySummary);
    header.capacity = DailyRetentionDays;
    file.write(reinterpret_cast<uint8_t const*>(&header), sizeof(header));

    PanelDailySummary empty;
    for (uint16_t i = 0; i < DailyRetentionDays; ++i) {
        file.write(reinterpret_cast<uint8_t const*>(&empty), sizeof(empty));
    }

    file.close();
    return true;
}

bool StatisticsClass::readSampleHeader(SampleFileHeader& header) const
{
    auto file = filesystem().open(SampleFile, "r");
    if (!file) { return false; }
    const auto read = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
    file.close();
    return read == sizeof(header);
}

void StatisticsClass::writeSampleHeader(SampleFileHeader const& header) const
{
    auto file = filesystem().open(SampleFile, filesystem().exists(SampleFile) ? "r+" : "w");
    if (!file) { return; }
    file.seek(0);
    file.write(reinterpret_cast<uint8_t const*>(&header), sizeof(header));
    file.close();
}

bool StatisticsClass::readPanelSampleHeader(PanelSampleFileHeader& header) const
{
    auto file = filesystem().open(PanelSampleFile, "r");
    if (!file) { return false; }
    const auto read = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
    file.close();
    return read == sizeof(header);
}

void StatisticsClass::writePanelSampleHeader(PanelSampleFileHeader const& header) const
{
    auto file = filesystem().open(PanelSampleFile, filesystem().exists(PanelSampleFile) ? "r+" : "w");
    if (!file) { return; }
    file.seek(0);
    file.write(reinterpret_cast<uint8_t const*>(&header), sizeof(header));
    file.close();
}

bool StatisticsClass::readSampleAt(uint32_t index, Sample& sample) const
{
    SampleFileHeader header;
    if (!readSampleHeader(header) || index >= header.capacity) { return false; }

    auto file = filesystem().open(SampleFile, "r");
    if (!file) { return false; }
    file.seek(sizeof(SampleFileHeader) + (index * sizeof(Sample)));
    const auto read = file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample));
    file.close();
    return read == sizeof(sample) && sample.timestamp > 0;
}

bool StatisticsClass::readPanelSampleAt(uint32_t index, PanelSample& sample) const
{
    PanelSampleFileHeader header;
    if (!readPanelSampleHeader(header) || index >= header.capacity) { return false; }

    auto file = filesystem().open(PanelSampleFile, "r");
    if (!file) { return false; }
    file.seek(sizeof(PanelSampleFileHeader) + (index * sizeof(PanelSample)));
    const auto read = file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample));
    file.close();
    return read == sizeof(sample) && sample.timestamp > 0;
}

bool StatisticsClass::writeSampleAt(uint32_t index, Sample const& sample) const
{
    auto file = filesystem().open(SampleFile, "r+");
    if (!file) { return false; }
    file.seek(sizeof(SampleFileHeader) + (index * sizeof(Sample)));
    const auto written = file.write(reinterpret_cast<uint8_t const*>(&sample), sizeof(sample));
    file.close();
    return written == sizeof(sample);
}

bool StatisticsClass::writePanelSampleAt(uint32_t index, PanelSample const& sample) const
{
    auto file = filesystem().open(PanelSampleFile, "r+");
    if (!file) { return false; }
    file.seek(sizeof(PanelSampleFileHeader) + (index * sizeof(PanelSample)));
    const auto written = file.write(reinterpret_cast<uint8_t const*>(&sample), sizeof(sample));
    file.close();
    return written == sizeof(sample);
}

bool StatisticsClass::appendArchivedSample(Sample const& sample) const
{
    if (sample.timestamp == 0) { return true; }

    const auto total = filesystem().totalBytes();
    const auto reserve = std::min<size_t>(16 * 1024, total / 64);
    if (total > 0 && total <= filesystem().usedBytes() + reserve + sizeof(Sample)) {
        return false;
    }

    const auto path = archivePath(SampleArchivePrefix, sample.timestamp);
    auto file = filesystem().open(path.c_str(), "a");
    if (!file) { return false; }

    const auto oldArchiveSize = file.size();
    const auto written = file.write(reinterpret_cast<uint8_t const*>(&sample), sizeof(sample));
    file.close();
    const bool success = written == sizeof(sample);
    if (success) {
        appendArchiveIndexRecord(filesystem(), path, sizeof(Sample), oldArchiveSize, sample.timestamp);
        invalidateArchivedSampleBoundsCache(sample.timestamp);
    }
    return success;
}

bool StatisticsClass::appendArchivedPanelSample(PanelSample const& sample) const
{
    if (sample.timestamp == 0 || sample.panelPowerFlags == 0) { return true; }

    const auto total = filesystem().totalBytes();
    const auto reserve = std::min<size_t>(16 * 1024, total / 64);
    if (total > 0 && total <= filesystem().usedBytes() + reserve + sizeof(PanelSample)) {
        return false;
    }

    const auto path = archivePath(PanelSampleArchivePrefix, sample.timestamp);
    auto file = filesystem().open(path.c_str(), "a");
    if (!file) { return false; }

    const auto oldArchiveSize = file.size();
    const auto written = file.write(reinterpret_cast<uint8_t const*>(&sample), sizeof(sample));
    file.close();
    const bool success = written == sizeof(sample);
    if (success) {
        appendArchiveIndexRecord(filesystem(), path, sizeof(PanelSample), oldArchiveSize, sample.timestamp);
    }
    return success;
}

void StatisticsClass::storeSample(Sample const& sample)
{
    if (!appendArchivedSample(sample)) {
        ESP_LOGE(TAG, "Failed to append linear statistics sample");
        return;
    }

    updateDailySummary(sample);
    _previousSample = sample;
    _hasPreviousSample = true;
}

void StatisticsClass::storePanelSample(PanelSample const& sample)
{
    if (sample.timestamp == 0 || sample.panelPowerFlags == 0) { return; }

    if (!appendArchivedPanelSample(sample)) {
        ESP_LOGE(TAG, "Failed to append linear panel statistics sample");
        return;
    }

    updatePanelDailySummary(sample);
    _previousPanelSample = sample;
    _hasPreviousPanelSample = true;
}

void StatisticsClass::loadPreviousSample()
{
    SampleFileHeader header;
    if (readSampleHeader(header) && header.count > 0 && header.capacity > 0) {
        const auto lastIndex = (header.nextIndex + header.capacity - 1) % header.capacity;
        Sample sample;
        if (readSampleAt(lastIndex, sample)) {
            _previousSample = sample;
            _hasPreviousSample = true;
        }
    }

    auto rootFs = filesystem().open("/");
    if (!rootFs) { return; }

    std::vector<String> archivePaths;
    File file = rootFs.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            const String name = file.name();
            const String path = name.startsWith("/") ? name : (String("/") + name);
            uint32_t monthFrom = 0;
            uint32_t monthTo = 0;
            if (archiveMonthBounds(path, SampleArchivePrefix, monthFrom, monthTo)) {
                archivePaths.push_back(path);
            }
        }
        file = rootFs.openNextFile();
    }
    rootFs.close();

    std::sort(archivePaths.begin(), archivePaths.end());
    for (auto it = archivePaths.rbegin(); it != archivePaths.rend(); ++it) {
        auto archive = filesystem().open(it->c_str(), "r");
        if (!archive) { continue; }

        bool found = false;
        Sample latest;
        Sample sample;
        const auto count = archive.size() / sizeof(Sample);
        for (uint32_t i = count; i > 0; --i) {
            archive.seek((i - 1) * sizeof(Sample));
            if (archive.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample)) != sizeof(sample)) {
                continue;
            }
            if (sample.timestamp == 0) { continue; }
            latest = sample;
            found = true;
            break;
        }
        archive.close();
        if (!found) { continue; }

        if (!_hasPreviousSample || latest.timestamp > _previousSample.timestamp) {
            _previousSample = latest;
            _hasPreviousSample = true;
        }
        break;
    }
}

void StatisticsClass::loadPreviousPanelSample()
{
    PanelSampleFileHeader header;
    if (readPanelSampleHeader(header) && header.count > 0 && header.capacity > 0) {
        const auto lastIndex = (header.nextIndex + header.capacity - 1) % header.capacity;
        PanelSample sample;
        if (readPanelSampleAt(lastIndex, sample)) {
            _previousPanelSample = sample;
            _hasPreviousPanelSample = true;
        }
    }

    auto rootFs = filesystem().open("/");
    if (!rootFs) { return; }

    std::vector<String> archivePaths;
    File file = rootFs.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            const String name = file.name();
            const String path = name.startsWith("/") ? name : (String("/") + name);
            uint32_t monthFrom = 0;
            uint32_t monthTo = 0;
            if (archiveMonthBounds(path, PanelSampleArchivePrefix, monthFrom, monthTo)) {
                archivePaths.push_back(path);
            }
        }
        file = rootFs.openNextFile();
    }
    rootFs.close();

    std::sort(archivePaths.begin(), archivePaths.end());
    for (auto it = archivePaths.rbegin(); it != archivePaths.rend(); ++it) {
        auto archive = filesystem().open(it->c_str(), "r");
        if (!archive) { continue; }

        bool found = false;
        PanelSample latest;
        PanelSample sample;
        const auto count = archive.size() / sizeof(PanelSample);
        for (uint32_t i = count; i > 0; --i) {
            archive.seek((i - 1) * sizeof(PanelSample));
            if (archive.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample)) != sizeof(sample)) {
                continue;
            }
            if (sample.timestamp == 0 || sample.panelPowerFlags == 0) { continue; }
            latest = sample;
            found = true;
            break;
        }
        archive.close();
        if (!found) { continue; }

        if (!_hasPreviousPanelSample || latest.timestamp > _previousPanelSample.timestamp) {
            _previousPanelSample = latest;
            _hasPreviousPanelSample = true;
        }
        break;
    }
}

void StatisticsClass::forEachRecentSample(std::function<void(Sample const&)> const& callback) const
{
    forEachRecentSample(0, std::numeric_limits<uint32_t>::max(), false, callback);
}

void StatisticsClass::forEachRecentSample(uint32_t from, uint32_t to, bool includePrevious, std::function<void(Sample const&)> const& callback) const
{
    SampleFileHeader header;
    if (to < from || !readSampleHeader(header) || header.count == 0 || header.capacity == 0) { return; }

    auto file = filesystem().open(SampleFile, "r");
    if (!file) { return; }

    const auto count = std::min(header.count, header.capacity);
    const auto first = (header.nextIndex + header.capacity - count) % header.capacity;

    auto readLogicalSample = [&](uint32_t logicalIndex, Sample& sample) {
        if (logicalIndex >= count) { return false; }

        const auto index = (first + logicalIndex) % header.capacity;
        file.seek(sizeof(SampleFileHeader) + (index * sizeof(Sample)));
        const auto read = file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample));
        return read == sizeof(sample) && sample.timestamp > 0;
    };

    uint32_t start = 0;
    if (from > 0) {
        uint32_t low = 0;
        uint32_t high = count;
        while (low < high) {
            const auto mid = low + ((high - low) / 2);

            Sample sample;
            if (!readLogicalSample(mid, sample) || sample.timestamp < from) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }

        start = low;
        if (includePrevious && start > 0) { start--; }
    }

    bool emittedPrevious = false;
    bool stop = false;
    uint32_t logicalIndex = start;
    while (logicalIndex < count && !stop) {
        const auto physicalIndex = (first + logicalIndex) % header.capacity;
        const auto chunk = std::min<uint32_t>(count - logicalIndex, header.capacity - physicalIndex);
        file.seek(sizeof(SampleFileHeader) + (physicalIndex * sizeof(Sample)));

        for (uint32_t offset = 0; offset < chunk && logicalIndex < count; ++offset, ++logicalIndex) {
            Sample sample;
            const auto read = file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample));
            if (read != sizeof(sample) || sample.timestamp == 0) { continue; }
            if (sample.timestamp < from) {
                if (includePrevious && !emittedPrevious) {
                    callback(sample);
                    emittedPrevious = true;
                }
                continue;
            }
            if (sample.timestamp > to) {
                stop = true;
                break;
            }

            callback(sample);
        }
    }

    file.close();
}

void StatisticsClass::forEachRecentPanelSample(
        uint32_t from,
        uint32_t to,
        bool includePrevious,
        std::function<void(PanelSample const&)> const& callback) const
{
    PanelSampleFileHeader header;
    if (to < from || !readPanelSampleHeader(header) || header.count == 0 || header.capacity == 0) { return; }

    auto file = filesystem().open(PanelSampleFile, "r");
    if (!file) { return; }

    const auto count = std::min(header.count, header.capacity);
    const auto first = (header.nextIndex + header.capacity - count) % header.capacity;

    auto readLogicalSample = [&](uint32_t logicalIndex, PanelSample& sample) {
        if (logicalIndex >= count) { return false; }

        const auto index = (first + logicalIndex) % header.capacity;
        file.seek(sizeof(PanelSampleFileHeader) + (index * sizeof(PanelSample)));
        const auto read = file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample));
        return read == sizeof(sample) && sample.timestamp > 0;
    };

    uint32_t start = 0;
    if (from > 0) {
        uint32_t low = 0;
        uint32_t high = count;
        while (low < high) {
            const auto mid = low + ((high - low) / 2);

            PanelSample sample;
            if (!readLogicalSample(mid, sample) || sample.timestamp < from) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }

        start = low;
        if (includePrevious && start > 0) { start--; }
    }

    bool emittedPrevious = false;
    bool stop = false;
    uint32_t logicalIndex = start;
    while (logicalIndex < count && !stop) {
        const auto physicalIndex = (first + logicalIndex) % header.capacity;
        const auto chunk = std::min<uint32_t>(count - logicalIndex, header.capacity - physicalIndex);
        file.seek(sizeof(PanelSampleFileHeader) + (physicalIndex * sizeof(PanelSample)));

        for (uint32_t offset = 0; offset < chunk && logicalIndex < count; ++offset, ++logicalIndex) {
            PanelSample sample;
            const auto read = file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample));
            if (read != sizeof(sample) || sample.timestamp == 0) { continue; }
            if (sample.timestamp < from) {
                if (includePrevious && !emittedPrevious) {
                    callback(sample);
                    emittedPrevious = true;
                }
                continue;
            }
            if (sample.timestamp > to) {
                stop = true;
                break;
            }

            callback(sample);
        }
    }

    file.close();
}

void StatisticsClass::forEachStoredSample(
        uint32_t from,
        uint32_t to,
        bool includePrevious,
        std::function<void(Sample const&)> const& callback) const
{
    if (to < from) { return; }

    std::vector<Sample> samples;
    std::optional<Sample> previous;
    uint32_t archivedOldest = 0;
    uint32_t archivedNewest = 0;

    auto updateArchivedBounds = [&](uint32_t timestamp) {
        archivedOldest = archivedOldest == 0
            ? timestamp
            : std::min(archivedOldest, timestamp);
        archivedNewest = std::max(archivedNewest, timestamp);
    };

    auto consider = [&](Sample const& sample, bool archived) {
        if (sample.timestamp == 0) { return; }
        if (archived && sample.timestamp >= from && sample.timestamp <= to) {
            updateArchivedBounds(sample.timestamp);
        }
        if (sample.timestamp < from) {
            if (includePrevious && (!previous || previous->timestamp < sample.timestamp)) {
                previous = sample;
            }
            return;
        }
        if (sample.timestamp > to) { return; }

        samples.push_back(sample);
    };

    forEachArchivePathInRange(SampleArchivePrefix, from, to, includePrevious, [&](String const& path) {
        auto file = filesystem().open(path.c_str(), "r");
        if (!file) { return; }

        const auto count = file.size() / sizeof(Sample);
        const auto archiveSize = file.size();
        auto readAt = [&](uint32_t index, Sample& sample) {
            if (index >= count) { return false; }
            file.seek(index * sizeof(Sample));
            return file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample)) == sizeof(sample);
        };

        uint32_t searchLow = 0;
        uint32_t searchHigh = count;
        ArchiveReadRange indexedRange;
        ArchiveIndexHeader indexHeader;
        std::vector<ArchiveDayIndexEntry> indexEntries;
        const bool hasIndex = ensureArchiveIndex(
                filesystem(),
                path,
                sizeof(Sample),
                archiveSize,
                [](uint8_t const* data) {
                    return reinterpret_cast<Sample const*>(data)->timestamp;
                },
                [](uint8_t const* data) {
                    return reinterpret_cast<Sample const*>(data)->timestamp > 0;
                },
                indexHeader,
                indexEntries);
        if (hasIndex) {
            indexedRange = archiveReadRange(indexHeader, indexEntries, from, to, includePrevious);
            if (!indexedRange.hasRange) {
                if (includePrevious && indexedRange.previousIndex != InvalidArchiveIndex) {
                    Sample previousSample;
                    if (readAt(indexedRange.previousIndex, previousSample)) {
                        consider(previousSample, true);
                    }
                }
                file.close();
                return;
            }
            searchLow = std::min(indexedRange.startIndex, count);
            searchHigh = std::min(indexedRange.endIndex, count);
        }

        uint32_t low = searchLow;
        uint32_t high = searchHigh;
        while (low < high) {
            const auto mid = low + ((high - low) / 2);
            Sample sample;
            if (!readAt(mid, sample) || sample.timestamp < from) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }

        if (includePrevious) {
            Sample previousSample;
            if (low > searchLow) {
                if (readAt(low - 1, previousSample)) {
                    consider(previousSample, true);
                }
            } else if (hasIndex && indexedRange.previousIndex != InvalidArchiveIndex) {
                if (readAt(indexedRange.previousIndex, previousSample)) {
                    consider(previousSample, true);
                }
            }
        }

        Sample sample;
        file.seek(low * sizeof(Sample));
        for (uint32_t i = low; i < searchHigh; ++i) {
            if (file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample)) != sizeof(sample)) { break; }
            if (sample.timestamp > to) { break; }
            consider(sample, true);
        }
        file.close();
    });

    // During the ring-buffer to archive transition both stores can cover the
    // same time range. Prefer archive samples and use the ring buffer only for
    // older, not-yet-archived ranges.
    auto overlapsArchive = [&](uint32_t timestamp) {
        return archivedOldest > 0
            && timestamp >= archivedOldest
            && timestamp <= archivedNewest;
    };

    forEachRecentSample(from, to, includePrevious, [&](Sample const& sample) {
        if (overlapsArchive(sample.timestamp)) { return; }
        consider(sample, false);
    });

    std::sort(samples.begin(), samples.end(), [](Sample const& a, Sample const& b) {
        return a.timestamp < b.timestamp;
    });

    bool hasLast = false;
    uint32_t lastTimestamp = 0;
    auto emit = [&](Sample const& sample) {
        if (hasLast && sample.timestamp == lastTimestamp) { return; }
        callback(sample);
        hasLast = true;
        lastTimestamp = sample.timestamp;
    };

    if (includePrevious && previous) {
        emit(*previous);
    }
    for (auto const& sample : samples) {
        emit(sample);
    }
}

void StatisticsClass::forEachStoredPanelSample(
        uint32_t from,
        uint32_t to,
        bool includePrevious,
        std::function<void(PanelSample const&)> const& callback) const
{
    if (to < from) { return; }

    std::vector<PanelSample> samples;
    std::optional<PanelSample> previous;
    uint32_t archivedOldest = 0;
    uint32_t archivedNewest = 0;

    auto updateArchivedBounds = [&](uint32_t timestamp) {
        archivedOldest = archivedOldest == 0
            ? timestamp
            : std::min(archivedOldest, timestamp);
        archivedNewest = std::max(archivedNewest, timestamp);
    };

    auto consider = [&](PanelSample const& sample, bool archived) {
        if (sample.timestamp == 0 || sample.panelPowerFlags == 0) { return; }
        if (archived && sample.timestamp >= from && sample.timestamp <= to) {
            updateArchivedBounds(sample.timestamp);
        }
        if (sample.timestamp < from) {
            if (includePrevious && (!previous || previous->timestamp < sample.timestamp)) {
                previous = sample;
            }
            return;
        }
        if (sample.timestamp > to) { return; }

        samples.push_back(sample);
    };

    forEachArchivePathInRange(PanelSampleArchivePrefix, from, to, includePrevious, [&](String const& path) {
        auto file = filesystem().open(path.c_str(), "r");
        if (!file) { return; }

        const auto count = file.size() / sizeof(PanelSample);
        const auto archiveSize = file.size();
        auto readAt = [&](uint32_t index, PanelSample& sample) {
            if (index >= count) { return false; }
            file.seek(index * sizeof(PanelSample));
            return file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample)) == sizeof(sample);
        };

        uint32_t searchLow = 0;
        uint32_t searchHigh = count;
        ArchiveReadRange indexedRange;
        ArchiveIndexHeader indexHeader;
        std::vector<ArchiveDayIndexEntry> indexEntries;
        const bool hasIndex = ensureArchiveIndex(
                filesystem(),
                path,
                sizeof(PanelSample),
                archiveSize,
                [](uint8_t const* data) {
                    return reinterpret_cast<PanelSample const*>(data)->timestamp;
                },
                [](uint8_t const* data) {
                    auto const sample = reinterpret_cast<PanelSample const*>(data);
                    return sample->timestamp > 0 && sample->panelPowerFlags != 0;
                },
                indexHeader,
                indexEntries);
        if (hasIndex) {
            indexedRange = archiveReadRange(indexHeader, indexEntries, from, to, includePrevious);
            if (!indexedRange.hasRange) {
                if (includePrevious && indexedRange.previousIndex != InvalidArchiveIndex) {
                    PanelSample previousSample;
                    if (readAt(indexedRange.previousIndex, previousSample)) {
                        consider(previousSample, true);
                    }
                }
                file.close();
                return;
            }
            searchLow = std::min(indexedRange.startIndex, count);
            searchHigh = std::min(indexedRange.endIndex, count);
        }

        uint32_t low = searchLow;
        uint32_t high = searchHigh;
        while (low < high) {
            const auto mid = low + ((high - low) / 2);
            PanelSample sample;
            if (!readAt(mid, sample) || sample.timestamp < from) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }

        if (includePrevious) {
            PanelSample previousSample;
            if (low > searchLow) {
                if (readAt(low - 1, previousSample)) {
                    consider(previousSample, true);
                }
            } else if (hasIndex && indexedRange.previousIndex != InvalidArchiveIndex) {
                if (readAt(indexedRange.previousIndex, previousSample)) {
                    consider(previousSample, true);
                }
            }
        }

        PanelSample sample;
        file.seek(low * sizeof(PanelSample));
        for (uint32_t i = low; i < searchHigh; ++i) {
            if (file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample)) != sizeof(sample)) { break; }
            if (sample.timestamp > to) { break; }
            consider(sample, true);
        }
        file.close();
    });

    // During the ring-buffer to archive transition both stores can cover the
    // same time range. Prefer archive samples and use the ring buffer only for
    // older, not-yet-archived ranges.
    auto overlapsArchive = [&](uint32_t timestamp) {
        return archivedOldest > 0
            && timestamp >= archivedOldest
            && timestamp <= archivedNewest;
    };

    forEachRecentPanelSample(from, to, includePrevious, [&](PanelSample const& sample) {
        if (overlapsArchive(sample.timestamp)) { return; }
        consider(sample, false);
    });

    std::sort(samples.begin(), samples.end(), [](PanelSample const& a, PanelSample const& b) {
        return a.timestamp < b.timestamp;
    });

    bool hasLast = false;
    uint32_t lastTimestamp = 0;
    auto emit = [&](PanelSample const& sample) {
        if (hasLast && sample.timestamp == lastTimestamp) { return; }
        callback(sample);
        hasLast = true;
        lastTimestamp = sample.timestamp;
    };

    if (includePrevious && previous) {
        emit(*previous);
    }
    for (auto const& sample : samples) {
        emit(sample);
    }
}

void StatisticsClass::ensureArchiveIndexes() const
{
    auto rootFs = filesystem().open("/");
    if (!rootFs) { return; }

    std::vector<String> sampleArchivePaths;
    std::vector<String> panelArchivePaths;
    File file = rootFs.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            const String name = file.name();
            const String path = name.startsWith("/") ? name : (String("/") + name);
            uint32_t monthFrom = 0;
            uint32_t monthTo = 0;
            if (archiveMonthBounds(path, SampleArchivePrefix, monthFrom, monthTo)) {
                sampleArchivePaths.push_back(path);
            } else if (archiveMonthBounds(path, PanelSampleArchivePrefix, monthFrom, monthTo)) {
                panelArchivePaths.push_back(path);
            }
        }
        file = rootFs.openNextFile();
    }
    rootFs.close();

    for (auto const& path : sampleArchivePaths) {
        auto file = filesystem().open(path.c_str(), "r");
        if (!file) { continue; }
        const auto archiveSize = file.size();
        file.close();

        ArchiveIndexHeader header;
        std::vector<ArchiveDayIndexEntry> entries;
        ensureArchiveIndex(
                filesystem(),
                path,
                sizeof(Sample),
                archiveSize,
                [](uint8_t const* data) {
                    return reinterpret_cast<Sample const*>(data)->timestamp;
                },
                [](uint8_t const* data) {
                    return reinterpret_cast<Sample const*>(data)->timestamp > 0;
                },
                header,
                entries);
    }

    for (auto const& path : panelArchivePaths) {
        auto file = filesystem().open(path.c_str(), "r");
        if (!file) { continue; }
        const auto archiveSize = file.size();
        file.close();

        ArchiveIndexHeader header;
        std::vector<ArchiveDayIndexEntry> entries;
        ensureArchiveIndex(
                filesystem(),
                path,
                sizeof(PanelSample),
                archiveSize,
                [](uint8_t const* data) {
                    return reinterpret_cast<PanelSample const*>(data)->timestamp;
                },
                [](uint8_t const* data) {
                    auto const sample = reinterpret_cast<PanelSample const*>(data);
                    return sample->timestamp > 0 && sample->panelPowerFlags != 0;
                },
                header,
                entries);
    }
}

void StatisticsClass::invalidateArchivedSampleBoundsCache(uint32_t timestamp) const
{
    if (timestamp == 0 || !_archivedSampleBoundsValid) { return; }

    _archivedSampleBoundsOldest = _archivedSampleBoundsOldest == 0
        ? timestamp
        : std::min(_archivedSampleBoundsOldest, timestamp);
    _archivedSampleBoundsNewest = std::max(_archivedSampleBoundsNewest, timestamp);
    _archivedSampleBoundsCheckedMillis = millis();
}

void StatisticsClass::updateArchivedSampleBounds(uint32_t& oldest, uint32_t& newest) const
{
    auto mergeCachedBounds = [&]() {
        if (_archivedSampleBoundsOldest > 0) {
            oldest = oldest == 0
                ? _archivedSampleBoundsOldest
                : std::min(oldest, _archivedSampleBoundsOldest);
        }
        if (_archivedSampleBoundsNewest > 0) {
            newest = std::max(newest, _archivedSampleBoundsNewest);
        }
    };

    const auto nowMillis = millis();
    if (_archivedSampleBoundsValid
            && nowMillis - _archivedSampleBoundsCheckedMillis < 30 * 1000) {
        mergeCachedBounds();
        return;
    }

    auto rootFs = filesystem().open("/");
    if (!rootFs) {
        if (_archivedSampleBoundsValid) {
            mergeCachedBounds();
        }
        return;
    }

    std::vector<String> paths;
    File file = rootFs.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            const String name = file.name();
            const String path = name.startsWith("/") ? name : (String("/") + name);
            uint32_t monthFrom = 0;
            uint32_t monthTo = 0;
            if (archiveMonthBounds(path, SampleArchivePrefix, monthFrom, monthTo)) {
                paths.push_back(path);
            }
        }
        file = rootFs.openNextFile();
    }
    rootFs.close();

    if (paths.empty()) {
        _archivedSampleBoundsOldest = 0;
        _archivedSampleBoundsNewest = 0;
        _archivedSampleBoundsCheckedMillis = nowMillis;
        _archivedSampleBoundsValid = true;
        return;
    }

    std::sort(paths.begin(), paths.end());

    auto readFirst = [&](String const& path, Sample& sample) {
        auto file = filesystem().open(path.c_str(), "r");
        if (!file) { return false; }

        while (file.read(reinterpret_cast<uint8_t*>(&sample), sizeof(sample)) == sizeof(sample)) {
            if (sample.timestamp > 0) {
                file.close();
                return true;
            }
        }

        file.close();
        return false;
    };

    auto readLast = [&](String const& path, Sample& sample) {
        auto file = filesystem().open(path.c_str(), "r");
        if (!file) { return false; }

        const auto count = file.size() / sizeof(Sample);
        for (uint32_t i = count; i > 0; --i) {
            Sample current;
            file.seek((i - 1) * sizeof(Sample));
            if (file.read(reinterpret_cast<uint8_t*>(&current), sizeof(current)) != sizeof(current)) {
                continue;
            }
            if (current.timestamp == 0) { continue; }
            sample = current;
            file.close();
            return true;
        }

        file.close();
        return false;
    };

    uint32_t archiveOldest = 0;
    uint32_t archiveNewest = 0;
    Sample sample;
    for (auto const& path : paths) {
        if (!readFirst(path, sample)) { continue; }
        archiveOldest = sample.timestamp;
        break;
    }

    for (auto it = paths.rbegin(); it != paths.rend(); ++it) {
        if (!readLast(*it, sample)) { continue; }
        archiveNewest = sample.timestamp;
        break;
    }

    _archivedSampleBoundsOldest = archiveOldest;
    _archivedSampleBoundsNewest = archiveNewest;
    _archivedSampleBoundsCheckedMillis = nowMillis;
    _archivedSampleBoundsValid = true;
    mergeCachedBounds();
}

void StatisticsClass::forEachDailySummary(uint32_t from, uint32_t to, std::function<void(DailySummary const&)> const& callback) const
{
    if (to < from) { return; }

    auto file = filesystem().open(DailyFile, "r");
    if (!file) { return; }

    DailyFileHeader header;
    const auto headerRead = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
    if (headerRead != sizeof(header)
            || header.magic != DailyFileMagic
            || header.version != DailyFileVersion
            || header.recordSize != sizeof(DailySummary)
            || header.capacity == 0) {
        file.close();
        return;
    }

    for (auto dayStart = localDayStart(from); dayStart <= to;) {
        const auto index = (dayStart / 86400) % header.capacity;
        DailySummary summary;
        file.seek(sizeof(DailyFileHeader) + (index * sizeof(DailySummary)));
        const auto read = file.read(reinterpret_cast<uint8_t*>(&summary), sizeof(summary));
        if (read == sizeof(summary) && summary.dayStart == dayStart && summary.sampleCount > 0) {
            callback(summary);
        }

        const auto nextDayStart = nextLocalDayStart(dayStart);
        if (nextDayStart <= dayStart) { break; }
        dayStart = nextDayStart;
    }

    file.close();
}

void StatisticsClass::forEachPanelDailySummary(uint32_t from, uint32_t to, std::function<void(PanelDailySummary const&)> const& callback) const
{
    if (to < from) { return; }

    auto file = filesystem().open(PanelDailyFile, "r");
    if (!file) { return; }

    PanelDailyFileHeader header;
    const auto headerRead = file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header));
    if (headerRead != sizeof(header)
            || header.magic != PanelDailyFileMagic
            || header.version != PanelDailyFileVersion
            || header.recordSize != sizeof(PanelDailySummary)
            || header.capacity == 0) {
        file.close();
        return;
    }

    for (auto dayStart = localDayStart(from); dayStart <= to;) {
        const auto index = (dayStart / 86400) % header.capacity;
        PanelDailySummary summary;
        file.seek(sizeof(PanelDailyFileHeader) + (index * sizeof(PanelDailySummary)));
        const auto read = file.read(reinterpret_cast<uint8_t*>(&summary), sizeof(summary));
        if (read == sizeof(summary) && summary.dayStart == dayStart && summary.sampleCount > 0) {
            callback(summary);
        }

        const auto nextDayStart = nextLocalDayStart(dayStart);
        if (nextDayStart <= dayStart) { break; }
        dayStart = nextDayStart;
    }

    file.close();
}

StatisticsClass::DailySummary StatisticsClass::getDailySummary(uint32_t dayStart) const
{
    DailySummary summary;

    auto file = filesystem().open(DailyFile, "r");
    if (file) {
        const auto index = (dayStart / 86400) % DailyRetentionDays;
        file.seek(sizeof(DailyFileHeader) + (index * sizeof(DailySummary)));
        const auto read = file.read(reinterpret_cast<uint8_t*>(&summary), sizeof(summary));
        file.close();

        if (read == sizeof(summary) && summary.dayStart == dayStart) { return summary; }
    }

    summary = DailySummary();
    summary.dayStart = dayStart;
    summary.from = dayStart;
    summary.to = dayStart;
    return summary;
}

StatisticsClass::PanelDailySummary StatisticsClass::getPanelDailySummary(uint32_t dayStart) const
{
    PanelDailySummary summary;

    auto file = filesystem().open(PanelDailyFile, "r");
    if (file) {
        const auto index = (dayStart / 86400) % DailyRetentionDays;
        file.seek(sizeof(PanelDailyFileHeader) + (index * sizeof(PanelDailySummary)));
        const auto read = file.read(reinterpret_cast<uint8_t*>(&summary), sizeof(summary));
        file.close();

        if (read == sizeof(summary) && summary.dayStart == dayStart) { return summary; }
    }

    summary = PanelDailySummary();
    summary.dayStart = dayStart;
    summary.from = dayStart;
    summary.to = dayStart;
    return summary;
}

void StatisticsClass::writeDailySummary(DailySummary const& summary) const
{
    auto file = filesystem().open(DailyFile, "r+");
    if (!file) { return; }

    const auto index = (summary.dayStart / 86400) % DailyRetentionDays;
    file.seek(sizeof(DailyFileHeader) + (index * sizeof(DailySummary)));
    file.write(reinterpret_cast<uint8_t const*>(&summary), sizeof(summary));
    file.close();
}

void StatisticsClass::writePanelDailySummary(PanelDailySummary const& summary) const
{
    auto file = filesystem().open(PanelDailyFile, "r+");
    if (!file) { return; }

    const auto index = (summary.dayStart / 86400) % DailyRetentionDays;
    file.seek(sizeof(PanelDailyFileHeader) + (index * sizeof(PanelDailySummary)));
    file.write(reinterpret_cast<uint8_t const*>(&summary), sizeof(summary));
    file.close();
}

void StatisticsClass::backfillDailySocAveragesFromSamples()
{
    struct SocAverageBucket {
        uint32_t dayStart = 0;
        float batterySocPercentSeconds = 0;
        float batterySocSeconds = 0;
    };

    std::vector<SocAverageBucket> buckets;

    auto addToBucket = [&](uint32_t dayStart, float socPercent, uint32_t seconds) {
        if (seconds == 0) { return; }

        auto existing = std::find_if(buckets.begin(), buckets.end(),
                [dayStart](SocAverageBucket const& bucket) {
                    return bucket.dayStart == dayStart;
                });
        if (existing == buckets.end()) {
            buckets.push_back(SocAverageBucket());
            existing = buckets.end() - 1;
            existing->dayStart = dayStart;
        }

        existing->batterySocPercentSeconds += socPercent * seconds;
        existing->batterySocSeconds += seconds;
    };

    auto addSegments = [&](uint32_t intervalStart, uint32_t intervalEnd, float socPercent) {
        while (intervalStart < intervalEnd) {
            auto const dayStart = localDayStart(intervalStart);
            auto const segmentEnd = std::min(intervalEnd, nextLocalDayStart(dayStart));
            if (segmentEnd <= intervalStart) { break; }

            addToBucket(dayStart, socPercent, segmentEnd - intervalStart);
            intervalStart = segmentEnd;
        }
    };

    bool hasPrevious = false;
    Sample previous;
    forEachRecentSample([&](Sample const& sample) {
        if (sample.flags & HasIntegratedPower) {
            if (sample.flags & HasBatterySoc) {
                auto const intervalStart = sample.timestamp > SampleIntervalSeconds
                    ? sample.timestamp - SampleIntervalSeconds
                    : 0;
                addSegments(intervalStart, sample.timestamp, sample.batterySocPermille / 10.0f);
            }

            previous = sample;
            hasPrevious = true;
            return;
        }

        if (hasPrevious
                && previous.timestamp < sample.timestamp
                && (previous.flags & HasBatterySoc)
                && (sample.flags & HasBatterySoc)) {
            addSegments(previous.timestamp,
                    sample.timestamp,
                    (previous.batterySocPermille + sample.batterySocPermille) / 20.0f);
        }

        previous = sample;
        hasPrevious = true;
    });

    for (auto const& bucket : buckets) {
        if (bucket.batterySocSeconds <= 0.0f) { continue; }

        auto day = getDailySummary(bucket.dayStart);
        if (day.sampleCount == 0 || hasBatterySocAverage(day)) { continue; }

        day.batterySocPercentSeconds = bucket.batterySocPercentSeconds;
        day.batterySocSeconds = bucket.batterySocSeconds;
        writeDailySummary(day);
    }
}

void StatisticsClass::rebuildDailySummariesFromSamplesIfEmpty()
{
    DailyFileHeader dailyHeader;
    if (!readDailyHeaderFrom(DailyFile, dailyHeader) || countDailyRecords(DailyFile, dailyHeader) > 0) {
        return;
    }

    SampleFileHeader sampleHeader;
    if (!readSampleHeader(sampleHeader) || sampleHeader.count == 0) { return; }

    ESP_LOGW(TAG, "Rebuilding empty daily statistics from %u recent samples", sampleHeader.count);

    _hasPreviousSample = false;
    _previousSample = Sample();
    forEachRecentSample([&](Sample const& sample) {
        updateDailySummary(sample);
        _previousSample = sample;
        _hasPreviousSample = true;
    });
}

void StatisticsClass::updateDailySummary(Sample const& sample)
{
    if (sample.flags & HasIntegratedPower) {
        auto intervalStart = sample.timestamp > SampleIntervalSeconds
            ? sample.timestamp - SampleIntervalSeconds
            : 0;
        auto const intervalEnd = sample.timestamp;
        auto const sampleDayStart = localDayStart(sample.timestamp);
        bool countedSample = false;

        while (intervalStart < intervalEnd) {
            auto const dayStart = localDayStart(intervalStart);
            auto const segmentEnd = std::min(intervalEnd, nextLocalDayStart(dayStart));
            if (segmentEnd <= intervalStart) { break; }
            auto day = getDailySummary(dayStart);

            if (dayStart == sampleDayStart && !countedSample) {
                day.sampleCount++;
                addExtrema(day, sample);
                countedSample = true;
            }

            day.to = std::max(day.to, segmentEnd);
            addIntegratedInterval(day, sample, segmentEnd - intervalStart);
            writeDailySummary(day);

            intervalStart = segmentEnd;
        }

        if (!countedSample) {
            auto day = getDailySummary(sampleDayStart);
            day.sampleCount++;
            day.to = std::max(day.to, sample.timestamp);
            addExtrema(day, sample);
            writeDailySummary(day);
        }

        return;
    }

    auto currentDay = getDailySummary(localDayStart(sample.timestamp));
    currentDay.to = sample.timestamp;
    if (currentDay.from == 0) { currentDay.from = sample.timestamp; }
    currentDay.sampleCount++;
    addExtrema(currentDay, sample);

    if (!_hasPreviousSample || _previousSample.timestamp >= sample.timestamp) {
        writeDailySummary(currentDay);
        return;
    }

    const auto previousDayStart = localDayStart(_previousSample.timestamp);
    const auto currentDayStart = localDayStart(sample.timestamp);

    if (previousDayStart == currentDayStart) {
        addInterval(currentDay, _previousSample, sample, sample.timestamp - _previousSample.timestamp);
        writeDailySummary(currentDay);
        return;
    }

    const auto split = currentDayStart;
    if (split > _previousSample.timestamp) {
        auto previousDay = getDailySummary(previousDayStart);
        previousDay.to = split;
        addInterval(previousDay, _previousSample, sample, split - _previousSample.timestamp);
        writeDailySummary(previousDay);
    }

    if (sample.timestamp > split) {
        addInterval(currentDay, _previousSample, sample, sample.timestamp - split);
    }
    writeDailySummary(currentDay);
}

void StatisticsClass::updatePanelDailySummary(PanelSample const& sample)
{
    auto currentDay = getPanelDailySummary(localDayStart(sample.timestamp));
    currentDay.to = sample.timestamp;
    if (currentDay.from == 0) { currentDay.from = sample.timestamp; }
    currentDay.sampleCount++;

    if (!_hasPreviousPanelSample || _previousPanelSample.timestamp >= sample.timestamp) {
        writePanelDailySummary(currentDay);
        return;
    }

    const auto previousDayStart = localDayStart(_previousPanelSample.timestamp);
    const auto currentDayStart = localDayStart(sample.timestamp);

    if (previousDayStart == currentDayStart) {
        addPanelInterval(currentDay, _previousPanelSample, sample, sample.timestamp - _previousPanelSample.timestamp);
        writePanelDailySummary(currentDay);
        return;
    }

    const auto split = currentDayStart;
    if (split > _previousPanelSample.timestamp) {
        auto previousDay = getPanelDailySummary(previousDayStart);
        previousDay.to = split;
        addPanelInterval(previousDay, _previousPanelSample, sample, split - _previousPanelSample.timestamp);
        writePanelDailySummary(previousDay);
    }

    if (sample.timestamp > split) {
        addPanelInterval(currentDay, _previousPanelSample, sample, sample.timestamp - split);
    }
    writePanelDailySummary(currentDay);
}

StatisticsClass::EnergySummary StatisticsClass::calculateRecentSummary(uint32_t from, uint32_t to) const
{
    EnergySummary summary;
    summary.from = from;
    summary.to = to;

    bool hasPrevious = false;
    Sample previous;

    forEachStoredSample(from, to, true, [&](Sample const& sample) {
        if (sample.timestamp < from || sample.timestamp > to) {
            if (sample.timestamp < from) {
                previous = sample;
                hasPrevious = true;
            }
            return;
        }

        summary.sampleCount++;
        addExtrema(summary, sample);

        if (sample.flags & HasIntegratedPower) {
            auto const intervalStart = sample.timestamp > SampleIntervalSeconds
                ? sample.timestamp - SampleIntervalSeconds
                : 0;
            auto const intervalEnd = sample.timestamp;
            auto const overlapStart = std::max(intervalStart, from);
            auto const overlapEnd = std::min(intervalEnd, to);
            if (overlapEnd > overlapStart) {
                addIntegratedInterval(summary, sample, overlapEnd - overlapStart);
            }

            previous = sample;
            hasPrevious = true;
            return;
        }

        if (hasPrevious && previous.timestamp < sample.timestamp) {
            const auto intervalStart = std::max(previous.timestamp, from);
            const auto intervalEnd = std::min(sample.timestamp, to);
            if (intervalEnd > intervalStart) {
                addInterval(summary, previous, sample, intervalEnd - intervalStart);
            }
        }

        previous = sample;
        hasPrevious = true;
    });

    return summary;
}

StatisticsClass::EnergySummary StatisticsClass::calculateDailySummary(uint32_t from, uint32_t to) const
{
    EnergySummary summary;
    summary.from = from;
    summary.to = to;

    forEachDailySummary(from, to, [&](DailySummary const& day) {
        summary.sampleCount += day.sampleCount;
        summary.intervalCount += day.intervalCount;
        summary.solarEnergyWh += day.solarEnergyWh;
        summary.batteryChargeWh += day.batteryChargeWh;
        summary.batteryDischargeWh += day.batteryDischargeWh;
        summary.gridImportWh += day.gridImportWh;
        summary.gridExportWh += day.gridExportWh;
        summary.gridChargerEnergyWh += day.gridChargerEnergyWh;
        for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
            summary.flexibleLoadEnergyWh[i] += day.flexibleLoadEnergyWh[i];
        }
        summary.targetDeviationWh += day.targetDeviationWh;
        summary.minBatterySoc = std::min(summary.minBatterySoc, day.minBatterySoc);
        summary.maxBatterySoc = std::max(summary.maxBatterySoc, day.maxBatterySoc);
        summary.minBatteryVoltage = std::min(summary.minBatteryVoltage, day.minBatteryVoltage);
        summary.maxBatteryVoltage = std::max(summary.maxBatteryVoltage, day.maxBatteryVoltage);
        summary.minBatteryTemperature = std::min(summary.minBatteryTemperature, day.minBatteryTemperature);
        summary.maxBatteryTemperature = std::max(summary.maxBatteryTemperature, day.maxBatteryTemperature);
        summary.maxGridImportWatts = std::max(summary.maxGridImportWatts, day.maxGridImportWatts);
        summary.maxGridExportWatts = std::max(summary.maxGridExportWatts, day.maxGridExportWatts);
        summary.maxBatteryChargeWatts = std::max(summary.maxBatteryChargeWatts, day.maxBatteryChargeWatts);
        summary.maxBatteryDischargeWatts = std::max(summary.maxBatteryDischargeWatts, day.maxBatteryDischargeWatts);
        summary.batterySocPercentSeconds += day.batterySocPercentSeconds;
        summary.batterySocSeconds += day.batterySocSeconds;
        summary.batteryBoostSavingsNormalWh += day.batteryBoostSavingsNormalWh;
        summary.batteryBoostSavingsRelaxedWh += day.batteryBoostSavingsRelaxedWh;
        summary.batteryBoostRelaxLimitedWh += day.batteryBoostRelaxLimitedWh;
        summary.batteryBoostSeconds += day.batteryBoostSeconds;
    });

    return summary;
}

StatisticsClass::PanelEnergySummary StatisticsClass::calculateRecentPanelSummary(uint32_t from, uint32_t to) const
{
    PanelEnergySummary summary;
    summary.from = from;
    summary.to = to;

    bool hasPrevious = false;
    PanelSample previous;

    forEachStoredPanelSample(from, to, true, [&](PanelSample const& sample) {
        if (sample.timestamp < from || sample.timestamp > to) {
            if (sample.timestamp < from) {
                previous = sample;
                hasPrevious = true;
            }
            return;
        }

        summary.sampleCount++;
        if (hasPrevious && previous.timestamp < sample.timestamp) {
            const auto intervalStart = std::max(previous.timestamp, from);
            const auto intervalEnd = std::min(sample.timestamp, to);
            if (intervalEnd > intervalStart) {
                addPanelInterval(summary, previous, sample, intervalEnd - intervalStart);
            }
        }

        previous = sample;
        hasPrevious = true;
    });

    return summary;
}

StatisticsClass::PanelEnergySummary StatisticsClass::calculateDailyPanelSummary(uint32_t from, uint32_t to) const
{
    PanelEnergySummary summary;
    summary.from = from;
    summary.to = to;

    forEachPanelDailySummary(from, to, [&](PanelDailySummary const& day) {
        summary.sampleCount += day.sampleCount;
        summary.intervalCount += day.intervalCount;
        for (uint8_t i = 0; i < PanelCount; ++i) {
            summary.panelEnergyWh[i] += day.panelEnergyWh[i];
        }
    });

    return summary;
}

std::vector<StatisticsClass::PanelDescriptor> StatisticsClass::collectPanelDescriptors(PanelEnergySummary const& summary) const
{
    std::vector<PanelDescriptor> panels;
    auto const& config = Configuration.get();

    for (uint8_t i = 0; i < INV_MAX_COUNT; ++i) {
        auto const& inverter = config.Inverter[i];
        if (inverter.Serial == 0) { continue; }
        if (isBatteryBackedPowerSource(getGovernedPowerSource(config, inverter.Serial))) { continue; }

        auto inv = Hoymiles.getInverterBySerial(inverter.Serial);
        std::list<ChannelNum_t> channels;
        if (inv != nullptr) {
            channels = inv->Statistics()->getChannelsByType(TYPE_DC);
        }

        for (uint8_t c = 0; c < INV_MAX_CHAN_COUNT; ++c) {
            const auto storageIndex = panelStorageIndex(i, c);
            if (!isValidPanelStorageIndex(storageIndex)) { continue; }

            const bool hasLiveChannel = std::find(channels.begin(), channels.end(), static_cast<ChannelNum_t>(c)) != channels.end();
            const bool hasConfiguredChannel = inverter.channel[c].Name[0] != '\0'
                || inverter.channel[c].MaxChannelPower > 0;
            const bool hasEnergy = summary.panelEnergyWh[storageIndex] > 0.0f;
            if (!hasLiveChannel && !hasConfiguredChannel && !hasEnergy) { continue; }

            PanelDescriptor panel;
            panel.inverterIndex = i;
            panel.channel = c;
            panel.storageIndex = storageIndex;
            panel.order = inverter.Order;
            panel.serial = serialString(inverter.Serial);
            panel.inverterName = inverter.Name[0] != '\0' ? String(inverter.Name) : panel.serial;
            panel.name = inverter.channel[c].Name[0] != '\0'
                ? String(inverter.channel[c].Name)
                : (String("String ") + String(c + 1));
            panel.enabled = inverter.Poll_Enable;
            panel.maxPower = inverter.channel[c].MaxChannelPower;
            panels.push_back(panel);
        }
    }

    std::sort(panels.begin(), panels.end(), [](PanelDescriptor const& a, PanelDescriptor const& b) {
        if (a.order != b.order) { return a.order < b.order; }
        if (a.inverterIndex != b.inverterIndex) { return a.inverterIndex < b.inverterIndex; }
        return a.channel < b.channel;
    });

    return panels;
}

void StatisticsClass::addInterval(EnergySummary& summary, Sample const& previous, Sample const& current, uint32_t seconds) const
{
    if (current.flags & HasIntegratedPower) {
        addIntegratedInterval(summary, current, std::min<uint32_t>(seconds, SampleIntervalSeconds));
        return;
    }

    if (seconds == 0) { return; }
    const auto hours = seconds / 3600.0f;

    auto addPositiveEnergy = [&](uint16_t flag, int16_t previousWatts, int16_t currentWatts, float& energyWh) {
        if (!((previous.flags & flag) && (current.flags & flag))) { return; }

        const auto watts = (std::max<int32_t>(0, previousWatts) + std::max<int32_t>(0, currentWatts)) / 2.0f;
        energyWh += watts * hours;
    };

    addPositiveEnergy(HasBatteryDischargePower, previous.batteryDischargePowerWatts, current.batteryDischargePowerWatts, summary.batteryDischargeWh);
    addPositiveEnergy(HasSolarPower, previous.solarPowerWatts, current.solarPowerWatts, summary.solarEnergyWh);
    addPositiveEnergy(HasGridChargerPower, previous.gridChargerPowerWatts, current.gridChargerPowerWatts, summary.gridChargerEnergyWh);

    for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
        addPositiveEnergy(flexibleLoadPowerFlag(i),
                previous.flexibleLoadPowerWatts[i],
                current.flexibleLoadPowerWatts[i],
                summary.flexibleLoadEnergyWh[i]);
    }

    if ((previous.flags & HasBatteryPower) && (current.flags & HasBatteryPower)) {
        const auto previousCharge = std::max<int32_t>(0, previous.batteryPowerWatts);
        const auto currentCharge = std::max<int32_t>(0, current.batteryPowerWatts);
        summary.batteryChargeWh += ((previousCharge + currentCharge) / 2.0f) * hours;
    }

    if ((previous.flags & HasGridPower) && (current.flags & HasGridPower)) {
        const auto previousImport = std::max<int32_t>(0, previous.gridPowerWatts);
        const auto currentImport = std::max<int32_t>(0, current.gridPowerWatts);
        summary.gridImportWh += ((previousImport + currentImport) / 2.0f) * hours;

        const auto previousExport = std::max<int32_t>(0, -static_cast<int32_t>(previous.gridPowerWatts));
        const auto currentExport = std::max<int32_t>(0, -static_cast<int32_t>(current.gridPowerWatts));
        summary.gridExportWh += ((previousExport + currentExport) / 2.0f) * hours;
    }

    if ((previous.flags & HasBatteryBoostSavings) && (current.flags & HasBatteryBoostSavings)) {
        summary.batteryBoostSavingsNormalWh += (
                previous.batteryBoostSavingsNormalPowerWatts
                + current.batteryBoostSavingsNormalPowerWatts) / 2.0f * hours;
        summary.batteryBoostSavingsRelaxedWh += (
                previous.batteryBoostSavingsRelaxedPowerWatts
                + current.batteryBoostSavingsRelaxedPowerWatts) / 2.0f * hours;
        summary.batteryBoostRelaxLimitedWh += (
                previous.batteryBoostRelaxLimitedPowerWatts
                + current.batteryBoostRelaxLimitedPowerWatts) / 2.0f * hours;
        summary.batteryBoostSeconds += (previous.batteryBoostSeconds + current.batteryBoostSeconds) / 2.0f;
    }

    if ((previous.flags & HasGridPower) && (current.flags & HasGridPower)
            && (previous.flags & HasTargetPower) && (current.flags & HasTargetPower)) {
        const auto previousDeviation = std::abs(static_cast<int32_t>(previous.gridPowerWatts) - previous.targetPowerWatts);
        const auto currentDeviation = std::abs(static_cast<int32_t>(current.gridPowerWatts) - current.targetPowerWatts);
        summary.targetDeviationWh += ((previousDeviation + currentDeviation) / 2.0f) * hours;
    }

    if ((previous.flags & HasBatterySoc) && (current.flags & HasBatterySoc)) {
        addBatterySocAverage(
                summary,
                (previous.batterySocPermille + current.batterySocPermille) / 20.0f,
                seconds);
    }

    summary.intervalCount++;
}

void StatisticsClass::addPanelInterval(
        PanelEnergySummary& summary,
        PanelSample const& previous,
        PanelSample const& current,
        uint32_t seconds) const
{
    if (seconds == 0) { return; }
    const auto hours = seconds / 3600.0f;

    for (uint8_t i = 0; i < PanelCount; ++i) {
        if (!((previous.panelPowerFlags & (1ULL << i)) && (current.panelPowerFlags & (1ULL << i)))) { continue; }

        const auto watts = (std::max<int32_t>(0, previous.panelPowerWatts[i])
                + std::max<int32_t>(0, current.panelPowerWatts[i])) / 2.0f;
        summary.panelEnergyWh[i] += watts * hours;
    }

    summary.intervalCount++;
}

void StatisticsClass::addIntegratedInterval(EnergySummary& summary, Sample const& sample, uint32_t seconds) const
{
    if (seconds == 0) { return; }
    const auto hours = seconds / 3600.0f;

    if (sample.flags & HasBatteryDischargePower) {
        summary.batteryDischargeWh += std::max<int32_t>(0, sample.batteryDischargePowerWatts) * hours;
    }

    if (sample.flags & HasSolarPower) {
        summary.solarEnergyWh += std::max<int32_t>(0, sample.solarPowerWatts) * hours;
    }

    if (sample.flags & HasGridChargerPower) {
        summary.gridChargerEnergyWh += std::max<int32_t>(0, sample.gridChargerPowerWatts) * hours;
    }

    for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
        if (sample.flags & flexibleLoadPowerFlag(i)) {
            summary.flexibleLoadEnergyWh[i] += std::max<int32_t>(0, sample.flexibleLoadPowerWatts[i]) * hours;
        }
    }

    if (sample.flags & HasBatteryPower) {
        summary.batteryChargeWh += std::max<int32_t>(0, sample.batteryPowerWatts) * hours;
    }

    if (sample.flags & HasGridPower) {
        summary.gridImportWh += sample.gridImportPowerWatts * hours;
        summary.gridExportWh += sample.gridExportPowerWatts * hours;
    }

    if (sample.flags & HasBatteryBoostSavings) {
        auto const intervalRatio = seconds / static_cast<float>(SampleIntervalSeconds);
        summary.batteryBoostSavingsNormalWh += sample.batteryBoostSavingsNormalPowerWatts * hours;
        summary.batteryBoostSavingsRelaxedWh += sample.batteryBoostSavingsRelaxedPowerWatts * hours;
        summary.batteryBoostRelaxLimitedWh += sample.batteryBoostRelaxLimitedPowerWatts * hours;
        summary.batteryBoostSeconds += sample.batteryBoostSeconds * intervalRatio;
    }

    if ((sample.flags & HasGridPower) && (sample.flags & HasTargetPower)) {
        auto const deviation = std::abs(static_cast<int32_t>(sample.gridPowerWatts) - sample.targetPowerWatts);
        summary.targetDeviationWh += deviation * hours;
    }

    if (sample.flags & HasBatterySoc) {
        addBatterySocAverage(summary, sample.batterySocPermille / 10.0f, seconds);
    }

    summary.intervalCount++;
}

void StatisticsClass::addExtrema(EnergySummary& summary, Sample const& sample)
{
    if (sample.flags & HasBatterySoc) {
        const auto soc = sample.batterySocPermille / 10.0f;
        summary.minBatterySoc = std::min(summary.minBatterySoc, soc);
        summary.maxBatterySoc = std::max(summary.maxBatterySoc, soc);
    }

    if (sample.flags & HasBatteryVoltage) {
        const auto voltage = sample.batteryVoltageDecivolt / 10.0f;
        summary.minBatteryVoltage = std::min(summary.minBatteryVoltage, voltage);
        summary.maxBatteryVoltage = std::max(summary.maxBatteryVoltage, voltage);
    }

    if (sample.flags & HasBatteryTemperature) {
        const auto temperature = sample.batteryTemperatureDecicelsius / 10.0f;
        summary.minBatteryTemperature = std::min(summary.minBatteryTemperature, temperature);
        summary.maxBatteryTemperature = std::max(summary.maxBatteryTemperature, temperature);
    }

    if (sample.flags & HasGridPower) {
        if (sample.flags & HasIntegratedPower) {
            summary.maxGridImportWatts = std::max<int16_t>(
                    summary.maxGridImportWatts,
                    static_cast<int16_t>(std::min<uint16_t>(sample.gridImportPowerWatts, std::numeric_limits<int16_t>::max())));
            summary.maxGridExportWatts = std::max<int16_t>(
                    summary.maxGridExportWatts,
                    static_cast<int16_t>(std::min<uint16_t>(sample.gridExportPowerWatts, std::numeric_limits<int16_t>::max())));
        } else {
            summary.maxGridImportWatts = std::max<int16_t>(summary.maxGridImportWatts, std::max<int16_t>(0, sample.gridPowerWatts));
            auto const exportPower = std::min<int32_t>(
                    std::numeric_limits<int16_t>::max(),
                    std::max<int32_t>(0, -static_cast<int32_t>(sample.gridPowerWatts)));
            summary.maxGridExportWatts = std::max<int16_t>(summary.maxGridExportWatts, static_cast<int16_t>(exportPower));
        }
    }

    if (sample.flags & HasBatteryPower) {
        summary.maxBatteryChargeWatts = std::max<int16_t>(summary.maxBatteryChargeWatts, std::max<int16_t>(0, sample.batteryPowerWatts));
    }

    if (sample.flags & HasBatteryDischargePower) {
        summary.maxBatteryDischargeWatts = std::max<int16_t>(summary.maxBatteryDischargeWatts, std::max<int16_t>(0, sample.batteryDischargePowerWatts));
    }
}

void StatisticsClass::addBatterySocAverage(EnergySummary& summary, float socPercent, uint32_t seconds)
{
    if (seconds == 0 || std::isnan(socPercent)) { return; }

    summary.batterySocPercentSeconds += socPercent * seconds;
    summary.batterySocSeconds += seconds;
}

bool StatisticsClass::hasBatterySocAverage(EnergySummary const& summary)
{
    return summary.batterySocSeconds > 0.0f;
}

float StatisticsClass::averageBatterySoc(EnergySummary const& summary)
{
    return hasBatterySocAverage(summary)
        ? summary.batterySocPercentSeconds / summary.batterySocSeconds
        : 0.0f;
}

uint32_t StatisticsClass::localDayStart(uint32_t timestamp)
{
    time_t t = timestamp;
    struct tm timeinfo;
    localtime_r(&t, &timeinfo);
    timeinfo.tm_hour = 0;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    return static_cast<uint32_t>(mktime(&timeinfo));
}

int16_t StatisticsClass::clampInt16(float value)
{
    if (std::isnan(value)) { return 0; }
    value = std::max<float>(std::numeric_limits<int16_t>::min(), std::min<float>(std::numeric_limits<int16_t>::max(), value));
    return static_cast<int16_t>(std::round(value));
}

uint16_t StatisticsClass::clampUInt16(float value)
{
    if (std::isnan(value) || value < 0) { return 0; }
    value = std::min<float>(std::numeric_limits<uint16_t>::max(), value);
    return static_cast<uint16_t>(std::round(value));
}

uint16_t StatisticsClass::flexibleLoadPowerFlag(uint8_t index)
{
    if (index >= POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT) { return 0; }
    return FlexibleLoadPowerFlagBase << index;
}

uint8_t StatisticsClass::panelStorageIndex(uint8_t inverterIndex, uint8_t channel)
{
    return (inverterIndex * INV_MAX_CHAN_COUNT) + channel;
}

bool StatisticsClass::isValidPanelStorageIndex(uint8_t index)
{
    return index < PanelCount;
}

void StatisticsClass::addSummaryJson(JsonObject& root, EnergySummary const& summary)
{
    root["sample_count"] = summary.sampleCount;
    root["interval_count"] = summary.intervalCount;
    root["solar_energy_wh"] = summary.solarEnergyWh;
    root["battery_charge_wh"] = summary.batteryChargeWh;
    root["battery_discharge_wh"] = summary.batteryDischargeWh;
    root["grid_import_wh"] = summary.gridImportWh;
    root["grid_export_wh"] = summary.gridExportWh;
    root["grid_charger_energy_wh"] = summary.gridChargerEnergyWh;
    float flexibleLoadEnergyWh = 0;
    JsonArray flexibleLoads = root["flexible_load_energies_wh"].to<JsonArray>();
    for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
        flexibleLoadEnergyWh += summary.flexibleLoadEnergyWh[i];
        flexibleLoads.add(summary.flexibleLoadEnergyWh[i]);
    }
    root["flexible_load_energy_wh"] = flexibleLoadEnergyWh;
    root["target_deviation_wh"] = summary.targetDeviationWh;
    root["grid_balance_wh"] = summary.gridImportWh - summary.gridExportWh;
    root["max_grid_import_w"] = summary.maxGridImportWatts;
    root["max_grid_export_w"] = summary.maxGridExportWatts;
    root["max_battery_charge_w"] = summary.maxBatteryChargeWatts;
    root["max_battery_discharge_w"] = summary.maxBatteryDischargeWatts;
    root["battery_boost_savings_25a_wh"] = summary.batteryBoostSavingsNormalWh;
    root["battery_boost_savings_50a_wh"] = summary.batteryBoostSavingsRelaxedWh;
    root["battery_boost_extra_wh"] = std::max<float>(
            0.0f,
            summary.batteryBoostSavingsRelaxedWh - summary.batteryBoostSavingsNormalWh);
    root["battery_boost_relax_limited_wh"] = summary.batteryBoostRelaxLimitedWh;
    root["battery_boost_seconds"] = summary.batteryBoostSeconds;

    if (hasBatterySocAverage(summary)) { root["avg_battery_soc"] = averageBatterySoc(summary); }
    if (summary.minBatterySoc <= 100.0f) { root["min_battery_soc"] = summary.minBatterySoc; }
    if (summary.maxBatterySoc >= 0.0f) { root["max_battery_soc"] = summary.maxBatterySoc; }
    if (summary.minBatteryVoltage < 100000.0f) { root["min_battery_voltage"] = summary.minBatteryVoltage; }
    if (summary.maxBatteryVoltage >= 0.0f) { root["max_battery_voltage"] = summary.maxBatteryVoltage; }
    if (summary.minBatteryTemperature < 100000.0f) { root["min_battery_temperature"] = summary.minBatteryTemperature; }
    if (summary.maxBatteryTemperature > -100000.0f) { root["max_battery_temperature"] = summary.maxBatteryTemperature; }
}

void StatisticsClass::addPanelMetadataJson(JsonArray& root, std::vector<PanelDescriptor> const& panels, PanelEnergySummary const& summary) const
{
    for (auto const& panel : panels) {
        if (!isValidPanelStorageIndex(panel.storageIndex)) { continue; }

        auto item = root.add<JsonObject>();
        item["index"] = panel.storageIndex;
        item["inverter_index"] = panel.inverterIndex;
        item["channel"] = panel.channel;
        item["channel_number"] = panel.channel + 1;
        item["serial"] = panel.serial;
        item["inverter"] = panel.inverterName;
        item["name"] = panel.name;
        item["label"] = panel.inverterName + String(" / ") + panel.name;
        item["enabled"] = panel.enabled;
        item["order"] = panel.order;
        item["max_power"] = panel.maxPower;
        item["energy_wh"] = summary.panelEnergyWh[panel.storageIndex];
    }
}

void StatisticsClass::addStorageDiagnosticsJson(JsonArray& root) const
{
    auto rootFs = filesystem().open("/");
    if (!rootFs) { return; }

    File file = rootFs.openNextFile();
    while (file) {
            if (!file.isDirectory()) {
                auto item = root.add<JsonObject>();
                const String name = file.name();
                const String path = name.startsWith("/") ? name : (String("/") + name);
                item["name"] = name;
                item["size"] = file.size();

            SampleFileHeader sampleHeader;
            file.seek(0);
            if (file.read(reinterpret_cast<uint8_t*>(&sampleHeader), sizeof(sampleHeader)) == sizeof(sampleHeader)
                    && sampleHeader.magic == SampleFileMagic) {
                item["kind"] = "sample";
                item["version"] = sampleHeader.version;
                item["record_size"] = sampleHeader.recordSize;
                item["capacity"] = sampleHeader.capacity;
                item["count"] = sampleHeader.count;
                item["next_index"] = sampleHeader.nextIndex;
                item["recoverable"] = isRecoverableSampleHeader(sampleHeader);
            } else {
                DailyFileHeader dailyHeader;
                file.seek(0);
                if (file.read(reinterpret_cast<uint8_t*>(&dailyHeader), sizeof(dailyHeader)) == sizeof(dailyHeader)
                        && dailyHeader.magic == DailyFileMagic) {
                    item["kind"] = "daily";
                    item["version"] = dailyHeader.version;
                    item["record_size"] = dailyHeader.recordSize;
                    item["capacity"] = dailyHeader.capacity;
                    item["non_empty_days"] = countDailyRecords(path.c_str(), dailyHeader);
                    item["recoverable"] = isRecoverableDailyHeader(dailyHeader);
                }
            }
        }

        file = rootFs.openNextFile();
    }
    file.close();
    rootFs.close();
}

void StatisticsClass::addRecentSeriesJson(
        JsonArray& root,
        uint32_t from,
        uint32_t to,
        uint32_t resolutionSeconds,
        bool compact,
        std::vector<PanelDescriptor> const& panels,
        bool includePanelPowers,
        bool includeTemperatures) const
{
    if (to <= from || resolutionSeconds == 0) { return; }

    const auto bucketCount = std::min<uint32_t>(((to - from) / resolutionSeconds) + 1, 400);
    std::vector<Bucket> buckets(bucketCount);
    std::vector<std::vector<int32_t>> panelPowerSums;
    std::vector<std::vector<uint16_t>> panelPowerCounts;
    if (includePanelPowers && !panels.empty()) {
        panelPowerSums.resize(bucketCount);
        panelPowerCounts.resize(bucketCount);
        for (uint32_t i = 0; i < bucketCount; ++i) {
            panelPowerSums[i].resize(panels.size());
            panelPowerCounts[i].resize(panels.size());
        }
    }

    forEachStoredSample(from, to, false, [&](Sample const& sample) {
        if (sample.timestamp < from || sample.timestamp > to) { return; }

        const auto index = (sample.timestamp - from) / resolutionSeconds;
        if (index >= buckets.size()) { return; }

        auto& bucket = buckets[index];
        bucket.timestamp = from + (index * resolutionSeconds);
        bucket.sampleCount++;

        if (sample.flags & HasBatteryDischargePower) {
            bucket.batteryDischargePowerSum += sample.batteryDischargePowerWatts;
            bucket.batteryDischargePowerCount++;
        }
        if (sample.flags & HasSolarPower) {
            bucket.solarPowerSum += sample.solarPowerWatts;
            bucket.solarPowerCount++;
        }
        if (sample.flags & HasBatteryPower) {
            bucket.batteryPowerSum += sample.batteryPowerWatts;
            bucket.batteryPowerCount++;
        }
        if (sample.flags & HasGridPower) {
            bucket.gridPowerSum += sample.gridPowerWatts;
            if (sample.flags & HasIntegratedPower) {
                bucket.gridImportPowerSum += sample.gridImportPowerWatts;
                bucket.gridExportPowerSum += sample.gridExportPowerWatts;
            } else {
                bucket.gridImportPowerSum += std::max<int16_t>(0, sample.gridPowerWatts);
                bucket.gridExportPowerSum += std::min<int32_t>(
                        std::numeric_limits<uint16_t>::max(),
                        std::max<int32_t>(0, -static_cast<int32_t>(sample.gridPowerWatts)));
            }
            bucket.gridPowerCount++;
        }
        if (sample.flags & HasGridChargerPower) {
            bucket.gridChargerPowerSum += sample.gridChargerPowerWatts;
            bucket.gridChargerPowerCount++;
        }
        if (sample.flags & HasBatteryBoostSavings) {
            bucket.batteryBoostSavingsNormalPowerSum += sample.batteryBoostSavingsNormalPowerWatts;
            bucket.batteryBoostSavingsRelaxedPowerSum += sample.batteryBoostSavingsRelaxedPowerWatts;
            bucket.batteryBoostRelaxLimitedPowerSum += sample.batteryBoostRelaxLimitedPowerWatts;
            bucket.batteryBoostSecondsSum += sample.batteryBoostSeconds;
            bucket.batteryBoostSavingsCount++;
            bucket.batteryBoostBudgetUsedPermilleSum += sample.batteryBoostBudgetUsedPermille;
            bucket.batteryBoostBudgetUsedCount++;
        }
        for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
            if (!(sample.flags & flexibleLoadPowerFlag(i))) { continue; }

            bucket.flexibleLoadPowerSum[i] += sample.flexibleLoadPowerWatts[i];
            bucket.flexibleLoadPowerCount[i]++;
        }
        if (sample.flags & HasTargetPower) {
            bucket.targetPowerSum += sample.targetPowerWatts;
            bucket.targetPowerCount++;
        }
        if (sample.flags & HasBatterySoc) {
            bucket.batterySocPermilleSum += sample.batterySocPermille;
            bucket.batterySocCount++;
        }
        if (includeTemperatures && sample.flags & HasInverterTemperatures) {
            for (uint8_t i = 0; i < INV_MAX_COUNT; ++i) {
                if (!(sample.inverterTemperatureFlags & (1 << i))) { continue; }

                bucket.inverterTemperatureDecicelsiusSum[i] += sample.inverterTemperatureDecicelsius[i];
                bucket.inverterTemperatureCount[i]++;
            }
        }
    });

    if (includePanelPowers && !panels.empty()) {
        forEachStoredPanelSample(from, to, false, [&](PanelSample const& sample) {
            if (sample.timestamp < from || sample.timestamp > to) { return; }

            const auto index = (sample.timestamp - from) / resolutionSeconds;
            if (index >= buckets.size()) { return; }

            for (size_t panelPosition = 0; panelPosition < panels.size(); ++panelPosition) {
                auto const storageIndex = panels[panelPosition].storageIndex;
                if (!isValidPanelStorageIndex(storageIndex)) { continue; }
                if (!(sample.panelPowerFlags & (1ULL << storageIndex))) { continue; }

                panelPowerSums[index][panelPosition] += sample.panelPowerWatts[storageIndex];
                panelPowerCounts[index][panelPosition]++;
            }
        });
    }

    for (size_t bucketIndex = 0; bucketIndex < buckets.size(); ++bucketIndex) {
        auto const& bucket = buckets[bucketIndex];
        if (bucket.sampleCount == 0) { continue; }

        auto average = [](auto sum, uint16_t count) -> float {
            return count > 0 ? sum / static_cast<float>(count) : 0.0f;
        };

        const auto batteryDischargePower = average(bucket.batteryDischargePowerSum, bucket.batteryDischargePowerCount);
        const auto solarPower = average(bucket.solarPowerSum, bucket.solarPowerCount);
        const auto batteryPower = average(bucket.batteryPowerSum, bucket.batteryPowerCount);
        const auto gridPower = average(bucket.gridPowerSum, bucket.gridPowerCount);
        const auto gridImportPower = average(bucket.gridImportPowerSum, bucket.gridPowerCount);
        const auto gridExportPower = average(bucket.gridExportPowerSum, bucket.gridPowerCount);
        const auto gridChargerPower = average(bucket.gridChargerPowerSum, bucket.gridChargerPowerCount);
        const auto batteryBoostSavingsNormalPower = average(
                bucket.batteryBoostSavingsNormalPowerSum,
                bucket.batteryBoostSavingsCount);
        const auto batteryBoostSavingsRelaxedPower = average(
                bucket.batteryBoostSavingsRelaxedPowerSum,
                bucket.batteryBoostSavingsCount);
        const auto batteryBoostExtraPower = std::max<float>(
                0.0f,
                average(bucket.batteryBoostSavingsRelaxedPowerSum, bucket.batteryBoostSavingsCount)
                    - average(bucket.batteryBoostSavingsNormalPowerSum, bucket.batteryBoostSavingsCount));
        const auto batteryBoostRelaxLimitedPower = average(
                bucket.batteryBoostRelaxLimitedPowerSum,
                bucket.batteryBoostSavingsCount);
        const auto batteryBoostBudgetUsed = average(
                bucket.batteryBoostBudgetUsedPermilleSum,
                bucket.batteryBoostBudgetUsedCount) / 10.0f;

        float flexibleLoadPowerWatts = 0;
        float flexibleLoadPowers[POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT] = {};
        for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
            auto const power = average(bucket.flexibleLoadPowerSum[i], bucket.flexibleLoadPowerCount[i]);
            flexibleLoadPowerWatts += power;
            flexibleLoadPowers[i] = power;
        }
        const auto targetPower = average(bucket.targetPowerSum, bucket.targetPowerCount);
        const auto batterySoc = bucket.batterySocCount > 0
            ? (bucket.batterySocPermilleSum / static_cast<float>(bucket.batterySocCount)) / 10.0f
            : 0.0f;
        const auto plannedBatterySoc = GridCharger.getAutoPowerPlannedSoC(bucket.timestamp);
        bool hasInverterTemperatures = false;
        for (uint8_t i = 0; i < INV_MAX_COUNT; ++i) {
            if (bucket.inverterTemperatureCount[i] > 0) {
                hasInverterTemperatures = true;
                break;
            }
        }

        if (compact) {
            auto row = root.add<JsonArray>();
            row.add(bucket.timestamp);
            row.add(batteryDischargePower);
            row.add(solarPower);
            row.add(batteryPower);
            row.add(gridPower);
            row.add(gridImportPower);
            row.add(gridExportPower);
            row.add(gridChargerPower);
            row.add(batteryBoostSavingsNormalPower);
            row.add(batteryBoostSavingsRelaxedPower);
            row.add(batteryBoostExtraPower);
            row.add(batteryBoostRelaxLimitedPower);
            row.add(bucket.batteryBoostSecondsSum);
            row.add(batteryBoostBudgetUsed);
            row.add(flexibleLoadPowerWatts);
            auto flexibleLoadPowerValues = row.add<JsonArray>();
            for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
                flexibleLoadPowerValues.add(flexibleLoadPowers[i]);
            }
            row.add(targetPower);
            if (bucket.batterySocCount > 0) {
                row.add(batterySoc);
            } else {
                row.add(nullptr);
            }
            if (plannedBatterySoc) {
                row.add(*plannedBatterySoc);
            } else {
                row.add(nullptr);
            }

            if (includeTemperatures) {
                auto inverterTemperatures = row.add<JsonObject>();
                if (hasInverterTemperatures) {
                    for (uint8_t i = 0; i < INV_MAX_COUNT; ++i) {
                        if (bucket.inverterTemperatureCount[i] == 0) { continue; }

                        inverterTemperatures[String(i)] = average(
                                bucket.inverterTemperatureDecicelsiusSum[i],
                                bucket.inverterTemperatureCount[i]) / 10.0f;
                    }
                }
            }

            if (includePanelPowers) {
                auto panelPowers = row.add<JsonArray>();
                for (size_t i = 0; i < panels.size(); ++i) {
                    const auto count = panelPowerCounts[bucketIndex][i];
                    if (count > 0) {
                        panelPowers.add(average(panelPowerSums[bucketIndex][i], count));
                    } else {
                        panelPowers.add(nullptr);
                    }
                }
            }
            continue;
        }

        auto item = root.add<JsonObject>();
        item["t"] = bucket.timestamp;
        item["battery_discharge_power_w"] = batteryDischargePower;
        item["solar_power_w"] = solarPower;
        item["battery_power_w"] = batteryPower;
        item["grid_power_w"] = gridPower;
        item["grid_import_power_w"] = gridImportPower;
        item["grid_export_power_w"] = gridExportPower;
        item["grid_charger_power_w"] = gridChargerPower;
        item["battery_boost_savings_25a_power_w"] = batteryBoostSavingsNormalPower;
        item["battery_boost_savings_50a_power_w"] = batteryBoostSavingsRelaxedPower;
        item["battery_boost_extra_power_w"] = batteryBoostExtraPower;
        item["battery_boost_relax_limited_power_w"] = batteryBoostRelaxLimitedPower;
        item["battery_boost_seconds"] = bucket.batteryBoostSecondsSum;
        item["battery_boost_budget_used"] = batteryBoostBudgetUsed;
        JsonArray flexibleLoadPowerValues = item["flexible_load_powers_w"].to<JsonArray>();
        for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
            flexibleLoadPowerValues.add(flexibleLoadPowers[i]);
        }
        item["flexible_load_power_w"] = flexibleLoadPowerWatts;
        item["target_power_w"] = targetPower;
        if (bucket.batterySocCount > 0) { item["battery_soc"] = batterySoc; }
        if (plannedBatterySoc) { item["battery_planned_soc"] = *plannedBatterySoc; }
        if (includeTemperatures && hasInverterTemperatures) {
            auto inverterTemperatures = item["inverter_temperatures"].to<JsonObject>();
            for (uint8_t i = 0; i < INV_MAX_COUNT; ++i) {
                if (bucket.inverterTemperatureCount[i] == 0) { continue; }

                inverterTemperatures[String(i)] = average(
                        bucket.inverterTemperatureDecicelsiusSum[i],
                        bucket.inverterTemperatureCount[i]) / 10.0f;
            }
        }
        if (includePanelPowers) {
            JsonArray panelPowers = item["panel_powers_w"].to<JsonArray>();
            for (size_t i = 0; i < panels.size(); ++i) {
                const auto count = panelPowerCounts[bucketIndex][i];
                if (count > 0) {
                    panelPowers.add(average(panelPowerSums[bucketIndex][i], count));
                } else {
                    panelPowers.add(nullptr);
                }
            }
        }
    }
}

void StatisticsClass::addDailySeriesJson(
        JsonArray& root,
        uint32_t from,
        uint32_t to,
        std::vector<PanelDescriptor> const& panels) const
{
    forEachDailySummary(from, to, [&](DailySummary const& day) {
        auto item = root.add<JsonObject>();
        item["t"] = day.dayStart;
        item["solar_energy_wh"] = day.solarEnergyWh;
        auto const panelDay = getPanelDailySummary(day.dayStart);
        JsonArray panelEnergies = item["panel_energies_wh"].to<JsonArray>();
        for (auto const& panel : panels) {
            if (panelDay.sampleCount > 0 && isValidPanelStorageIndex(panel.storageIndex)) {
                panelEnergies.add(panelDay.panelEnergyWh[panel.storageIndex]);
            } else {
                panelEnergies.add(nullptr);
            }
        }
        item["battery_charge_wh"] = day.batteryChargeWh;
        item["battery_discharge_wh"] = day.batteryDischargeWh;
        item["grid_import_wh"] = day.gridImportWh;
        item["grid_export_wh"] = day.gridExportWh;
        item["grid_charger_energy_wh"] = day.gridChargerEnergyWh;
        item["battery_boost_savings_25a_wh"] = day.batteryBoostSavingsNormalWh;
        item["battery_boost_savings_50a_wh"] = day.batteryBoostSavingsRelaxedWh;
        item["battery_boost_extra_wh"] = std::max<float>(
                0.0f,
                day.batteryBoostSavingsRelaxedWh - day.batteryBoostSavingsNormalWh);
        item["battery_boost_relax_limited_wh"] = day.batteryBoostRelaxLimitedWh;
        item["battery_boost_seconds"] = day.batteryBoostSeconds;
        float flexibleLoadEnergyWh = 0;
        JsonArray flexibleLoadEnergies = item["flexible_load_energies_wh"].to<JsonArray>();
        for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
            flexibleLoadEnergyWh += day.flexibleLoadEnergyWh[i];
            flexibleLoadEnergies.add(day.flexibleLoadEnergyWh[i]);
        }
        item["flexible_load_energy_wh"] = flexibleLoadEnergyWh;
        if (hasBatterySocAverage(day)) { item["avg_battery_soc"] = averageBatterySoc(day); }
        if (day.minBatterySoc <= 100.0f) { item["min_battery_soc"] = day.minBatterySoc; }
        if (day.maxBatterySoc >= 0.0f) { item["max_battery_soc"] = day.maxBatterySoc; }
    });
}

void StatisticsClass::getStatus(
        JsonVariant& root,
        String const& period,
        bool compactSamples,
        uint32_t requestedFrom,
        String const& view) const
{
    const time_t nowTime = time(nullptr);
    const auto now = nowTime > 0 ? static_cast<uint32_t>(nowTime) : 0;
    const auto today = localDayStart(now);

    uint32_t from = today;
    uint32_t to = now;
    uint32_t resolution = SampleIntervalSeconds;
    bool daily = false;
    const char* normalizedPeriod = "today";
    const bool hasRequestedFrom = requestedFrom > 0;

    auto capToNow = [now](uint32_t value) {
        return value > now ? now : value;
    };
    auto windowEndAfterSeconds = [&](uint32_t start, uint32_t seconds) {
        if (seconds == 0) { return start; }
        return capToNow(saturatingAddSeconds(start, seconds - 1));
    };
    auto windowEndAfterLocalDays = [&](uint32_t start, uint16_t days) {
        const auto end = addLocalDays(start, days);
        return end > start ? capToNow(end - 1) : start;
    };

    if (period == "24h") {
        from = hasRequestedFrom
            ? requestedFrom
            : (now > 24 * 60 * 60 ? now - 24 * 60 * 60 : 0);
        if (hasRequestedFrom) {
            to = windowEndAfterSeconds(from, 24 * 60 * 60);
        }
        resolution = SampleIntervalSeconds;
        normalizedPeriod = "24h";
    } else if (period == "7d") {
        from = hasRequestedFrom
            ? requestedFrom
            : (now > 7 * 24 * 60 * 60 ? now - 7 * 24 * 60 * 60 : 0);
        if (hasRequestedFrom) {
            to = windowEndAfterSeconds(from, 7 * 24 * 60 * 60);
        }
        resolution = 60 * 60;
        normalizedPeriod = "7d";
    } else if (period == "30d") {
        from = localDayStart(hasRequestedFrom
            ? requestedFrom
            : (now > 30 * 24 * 60 * 60 ? now - 30 * 24 * 60 * 60 : 0));
        if (hasRequestedFrom) {
            to = windowEndAfterLocalDays(from, 30);
        }
        daily = true;
        normalizedPeriod = "30d";
    } else if (period == "365d") {
        from = localDayStart(hasRequestedFrom
            ? requestedFrom
            : (now > 365 * 24 * 60 * 60 ? now - 365 * 24 * 60 * 60 : 0));
        if (hasRequestedFrom) {
            to = windowEndAfterLocalDays(from, 365);
        }
        daily = true;
        normalizedPeriod = "365d";
    } else if (hasRequestedFrom) {
        from = requestedFrom;
        to = windowEndAfterSeconds(from, 24 * 60 * 60);
    }

    if (to < from) {
        to = from;
    }

    const auto nextRangeDayStart = nextLocalDayStart(localDayStart(from));
    const bool completeLocalDayRange = !daily
        && from == localDayStart(from)
        && nextRangeDayStart > from
        && to == nextRangeDayStart - 1;
    const bool useDailySummary = daily || completeLocalDayRange;
    const bool includePanelPowers = view == "panels";
    const bool includeTemperatures = view == "temperatures";

    std::lock_guard<std::mutex> lock(_mutex);

    SampleFileHeader header;
    readSampleHeader(header);

    root["period"] = normalizedPeriod;
    root["from"] = from;
    root["to"] = to;
    root["sample_interval_seconds"] = SampleIntervalSeconds;
    root["recent_retention_days"] = header.capacity * SampleIntervalSeconds / (24 * 60 * 60);
    root["daily_retention_days"] = DailyRetentionDays;
    root["resolution_seconds"] = daily ? 24 * 60 * 60 : resolution;
    root["sample_capacity"] = header.capacity;
    root["recent_samples"] = header.count;
    uint32_t storedOldest = 0;
    uint32_t storedNewest = 0;
    if (header.count > 0
            && header.capacity > 0
            && header.count <= header.capacity
            && header.nextIndex < header.capacity) {
        const auto count = std::min(header.count, header.capacity);
        const auto firstIndex = (header.nextIndex + header.capacity - count) % header.capacity;
        const auto lastIndex = (header.nextIndex + header.capacity - 1) % header.capacity;
        Sample boundary;
        if (readSampleAt(firstIndex, boundary)) {
            storedOldest = storedOldest == 0 ? boundary.timestamp : std::min(storedOldest, boundary.timestamp);
            storedNewest = std::max(storedNewest, boundary.timestamp);
        }
        if (readSampleAt(lastIndex, boundary)) {
            storedOldest = storedOldest == 0 ? boundary.timestamp : std::min(storedOldest, boundary.timestamp);
            storedNewest = std::max(storedNewest, boundary.timestamp);
        }
    }
    updateArchivedSampleBounds(storedOldest, storedNewest);
    if (storedOldest > 0) {
        root["recent_from"] = storedOldest;
    }
    if (storedNewest > 0) {
        root["recent_to"] = storedNewest;
    }
    if (storedOldest > 0 && storedNewest >= storedOldest) {
        root["recent_retention_days"] = (storedNewest - storedOldest) / (24 * 60 * 60);
    }
    root["storage_bytes"] = sizeof(SampleFileHeader) + (header.capacity * sizeof(Sample)) + sizeof(DailyFileHeader) + (DailyRetentionDays * sizeof(DailySummary));
    root["storage_dedicated"] = _usingDedicatedFs;
    if (period == "debug") {
        auto storageFiles = root["storage_files"].to<JsonArray>();
        addStorageDiagnosticsJson(storageFiles);
    }
    auto boostEstimator = root["battery_boost_estimator"].to<JsonObject>();
    boostEstimator["normal_current_a"] = BatteryBoostNormalCurrentAmps;
    boostEstimator["max_current_a"] = BatteryBoostMaxCurrentAmps;
    boostEstimator["boost_budget_seconds"] = BatteryBoostBudgetSeconds;
    boostEstimator["cooldown_seconds"] = BatteryBoostCooldownSeconds;
    boostEstimator["inverter_efficiency"] = BatteryBoostInverterEfficiency;
    boostEstimator["ac_limit_w"] = BatteryBoostAcLimitWatts;
    auto summary = root["summary"].to<JsonObject>();
    auto calculated = useDailySummary ? calculateDailySummary(from, to) : calculateRecentSummary(from, to);
    addSummaryJson(summary, calculated);
    PanelEnergySummary panelCalculated;
    panelCalculated.from = from;
    panelCalculated.to = to;
    if (includePanelPowers || useDailySummary) {
        panelCalculated = useDailySummary ? calculateDailyPanelSummary(from, to) : calculateRecentPanelSummary(from, to);
    }
    auto const panels = collectPanelDescriptors(panelCalculated);
    auto panelMetadata = root["panels"].to<JsonArray>();
    addPanelMetadataJson(panelMetadata, panels, panelCalculated);

    auto const& fullConfig = Configuration.get();
    auto flexibleLoads = root["flexible_loads"].to<JsonArray>();
    auto const& config = fullConfig.PowerLimiter;
    for (uint8_t i = 0; i < POWERLIMITER_FLEXIBLE_LOAD_MAX_COUNT; ++i) {
        auto const& load = config.FlexibleLoads[i];
        bool const configured = load.PowerMqttTopic[0] != '\0';
        bool const hasEnergy = calculated.flexibleLoadEnergyWh[i] > 0.0f;
        if (!configured && !hasEnergy) { continue; }

        auto item = flexibleLoads.add<JsonObject>();
        item["index"] = i;
        String name = load.Name[0] != '\0' ? String(load.Name) : (String("Load ") + String(i + 1));
        item["name"] = name;
        item["enabled"] = load.Enabled;
        item["energy_wh"] = calculated.flexibleLoadEnergyWh[i];
    }

    auto inverters = root["inverters"].to<JsonArray>();
    for (uint8_t i = 0; i < INV_MAX_COUNT; ++i) {
        auto const& inverter = fullConfig.Inverter[i];
        if (inverter.Serial == 0) { continue; }

        const String serial = serialString(inverter.Serial);
        auto item = inverters.add<JsonObject>();
        item["index"] = i;
        item["serial"] = serial;
        item["name"] = inverter.Name[0] != '\0' ? String(inverter.Name) : serial;
        item["enabled"] = inverter.Poll_Enable;
        item["order"] = inverter.Order;
    }

    if (daily) {
        auto days = root["days"].to<JsonArray>();
        addDailySeriesJson(days, from, to, panels);
    } else {
        if (compactSamples) {
            auto fields = root["sample_fields"].to<JsonArray>();
            fields.add("t");
            fields.add("battery_discharge_power_w");
            fields.add("solar_power_w");
            fields.add("battery_power_w");
            fields.add("grid_power_w");
            fields.add("grid_import_power_w");
            fields.add("grid_export_power_w");
            fields.add("grid_charger_power_w");
            fields.add("battery_boost_savings_25a_power_w");
            fields.add("battery_boost_savings_50a_power_w");
            fields.add("battery_boost_extra_power_w");
            fields.add("battery_boost_relax_limited_power_w");
            fields.add("battery_boost_seconds");
            fields.add("battery_boost_budget_used");
            fields.add("flexible_load_power_w");
            fields.add("flexible_load_powers_w");
            fields.add("target_power_w");
            fields.add("battery_soc");
            fields.add("battery_planned_soc");
            if (includeTemperatures) {
                fields.add("inverter_temperatures");
            }
            if (includePanelPowers) {
                fields.add("panel_powers_w");
            }
        }
        auto samples = root["samples"].to<JsonArray>();
        addRecentSeriesJson(samples, from, to, resolution, compactSamples, panels, includePanelPowers, includeTemperatures);
    }
}
