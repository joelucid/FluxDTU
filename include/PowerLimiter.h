// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <ArduinoJson.h>
#include "Configuration.h"
#include "PowerLimiterInverter.h"
#include <espMqttClient.h>
#include <Arduino.h>
#include <atomic>
#include <deque>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>
#include <TaskSchedulerDeclarations.h>
#include <frozen/string.h>
#include "PowerLimiterPredictiveControl.h"

namespace GridChargers {
struct PowerLimiterTargetDispatchEvent;
}

#define PL_UI_STATE_INACTIVE 0
#define PL_UI_STATE_CHARGING 1
#define PL_UI_STATE_USE_SOLAR_ONLY 2
#define PL_UI_STATE_USE_SOLAR_AND_BATTERY 3

class PowerLimiterClass {
public:
    PowerLimiterClass() = default;

    enum class Status : unsigned {
        Initializing,
        DisabledByConfig,
        DisabledByMqtt,
        WaitingForValidTimestamp,
        PowerMeterPending,
        InverterInvalid,
        InverterCmdPending,
        ConfigReload,
        InverterStatsPending,
        UnconditionalSolarPassthrough,
        EmergencyFullOutput,
        Stable,
    };

    void init(Scheduler& scheduler);
    void triggerReloadingConfig() { _reloadConfigFlag = true; }
    uint8_t getInverterUpdateTimeouts() const;
    uint8_t getPowerLimiterState() const;
    int32_t getInverterOutput() const { return _lastExpectedInverterOutput; }
    bool isFullSolarPassthroughActive() const { return _fullSolarPassThroughActive; }

    enum class Mode : unsigned {
        Normal = 0,
        Disabled = 1,
        UnconditionalFullSolarPassthrough = 2
    };

    void setMode(Mode m) { _mode = m; _reloadConfigFlag = true; }
    Mode getMode() const { return _mode; }
    bool usesBatteryPoweredInverter() const;
    bool usesSmartBufferPoweredInverter() const;
    float getTargetPowerConsumption() const;
    float getBatteryTargetPowerConsumption() const;
    float getStorageTargetPowerConsumption() const;
    float getStaticTargetPowerConsumption() const;
    float getStaticBatteryTargetPowerConsumption() const;
    float getFlexibleLoadDynamicReserveWatts() const;
    float getFlexibleLoadStopTargetPowerConsumption() const;
    bool isFlexibleLoadEmergencyStopActive() const;
    bool ownsGridChargerTarget() const;
    bool isGridChargerManaged() const;
    void updateDynamicBatteryTarget();
    void addThermalDebugJson(JsonObject root) const;
    void addRuntimeDebugJson(JsonObject root) const;
    void addStatePersistenceJson(JsonObject root) const;
    void addTraceJson(
            JsonObject root,
            uint32_t offsetSeconds = 0,
            bool compact = false,
            uint32_t requestedRangeSeconds = 0,
            bool compactV2 = false) const;
    void writeTraceCompactV2Json(
            Print& output,
            uint32_t offsetSeconds = 0,
            uint32_t requestedRangeSeconds = 0) const;
    struct TraceCompactV2ChunkCursor {
        static constexpr size_t PendingBufferSize = 512;

        bool initialized = false;
        bool headerWritten = false;
        bool footerWritten = false;
        bool firstSample = true;
        uint32_t now = 0;
        uint32_t nowEpoch = 0;
        uint32_t capacity = 0;
        uint32_t historySeconds = 0;
        uint32_t visibleWindowSeconds = 0;
        uint32_t offsetSeconds = 0;
        uint32_t rangeEndSeconds = 0;
        uint32_t traceBytes = 0;
        uint32_t sampleCountTotal = 0;
        uint32_t availableSeconds = 0;
        size_t capacitySnapshot = 0;
        size_t firstIndex = 0;
        size_t sampleIndex = 0;
        size_t sampleCount = 0;
        bool samplesInPsram = false;
        char pending[PendingBufferSize] = {};
        size_t pendingOffset = 0;
        size_t pendingLength = 0;
    };
    size_t writeTraceCompactV2JsonChunk(
            TraceCompactV2ChunkCursor& cursor,
            uint8_t* buffer,
            size_t maxLen,
            uint32_t offsetSeconds = 0,
            uint32_t requestedRangeSeconds = 0) const;
    void addPredictiveRequestHistoryJson(
            JsonObject root,
            uint32_t updatedFrom,
            uint32_t displayFrom,
            uint32_t to) const;
    bool clearPersistedRuntimeState();

    static constexpr uint32_t PredictiveRequestHistorySeconds = 2 * 60;

    // used to interlock Huawei R48xx grid charger against battery-powered inverters
    bool isGovernedBatteryPoweredInverterProducing() const;

private:
    void loop();
    void traceSampleLoop();

    Task _loopTask;
    Task _traceSampleTask;

    std::atomic<bool> _reloadConfigFlag = true;
    uint16_t _lastExpectedInverterOutput = 0;
    Status _lastStatus = Status::Initializing;
    uint32_t _lastStatusPrinted = 0;
    Status _lastTraceStatus = Status::Initializing;
    uint32_t _lastCalculation = 0;
    static constexpr uint32_t _calculationBackoffMsDefault = 128;
    uint32_t _calculationBackoffMs = _calculationBackoffMsDefault;
    static constexpr uint32_t _emergencyFullOutputReassertIntervalMs = 5 * 1000;
    bool _emergencyFullOutputActive = false;
    uint32_t _lastEmergencyFullOutputRequestMillis = 0;
    Mode _mode = Mode::Normal;

    std::deque<std::unique_ptr<PowerLimiterInverter>> _inverters;
    std::deque<std::unique_ptr<PowerLimiterInverter>> _retirees;

    enum class BatteryState : uint8_t { STOP = 0, NO_DISCHARGE = 1, DISCHARGE_ALLOWED = 2, DISCHARGE_NIGHT = 3 };
    BatteryState _batteryState = BatteryState::STOP;
    bool _fromStart = false;
    bool _oneStopPerNightDone = false;

    std::pair<bool, uint32_t> _nextInverterRestart = { false, 0 };
    bool _fullSolarPassThroughActive = false;
    float _loadCorrectedVoltage = 0.0f;

    struct DynamicBatteryTargetForecastState {
        bool initialized = false;
        uint32_t updateMillis = 0;
        float mean = 0.0f;
        float variance = 0.0f;
        float target = 0.0f;
    };
    struct DynamicBatteryTargetOracleProbe {
        bool active = false;
        uint32_t originMillis = 0;
        uint32_t targetMillis = 0;
        DynamicBatteryTargetForecastState state;
        bool forecastValid = false;
        int16_t forecastMin = 0;
        int16_t forecastNominal = 0;
        int16_t forecastMax = 0;
    };

    uint32_t _lastDynamicBatteryTargetPowerMeterUpdate = 0;
    float _dynamicBatteryTargetMean = 0.0f;
    float _dynamicBatteryTargetVariance = 0.0f;
    bool _dynamicBatteryTargetInitialized = false;
    float _batteryTargetPowerConsumption = 0.0f;
    static constexpr size_t _dynamicBatteryTargetOracleProbeCapacity = 8;
    DynamicBatteryTargetOracleProbe _dynamicBatteryTargetOracleProbes[
        _dynamicBatteryTargetOracleProbeCapacity];
    size_t _dynamicBatteryTargetOracleProbeWrite = 0;
    bool _lastDynamicBatteryTargetOracleForecastValid = false;
    float _lastDynamicBatteryTargetOracleForecast = 0.0f;
    uint32_t _lastDynamicBatteryTargetOracleForecastMillis = 0;
    uint32_t _lastDynamicBatteryTargetOracleForecastOriginMillis = 0;
    bool _lastDynamicBatteryTargetDueForecastValid = false;
    int16_t _lastDynamicBatteryTargetDueForecastMin = 0;
    int16_t _lastDynamicBatteryTargetDueForecastNominal = 0;
    int16_t _lastDynamicBatteryTargetDueForecastMax = 0;
    uint32_t _lastDynamicBatteryTargetDueForecastMillis = 0;
    uint32_t _lastDynamicBatteryTargetDueForecastOriginMillis = 0;
    float _batteryDischargeCurrentLimitBudgetAmpSeconds = 0.0f;
    float _batteryDischargeCurrentLimitBudgetCapacityAmpSeconds = 0.0f;
    uint32_t _lastBatteryDischargeCurrentLimitBudgetUpdateMillis = 0;
    bool _batteryDischargeCurrentLimitBudgetInitialized = false;
    float _lastBatteryDischargeCurrentEstimateAmps = 0.0f;
    float _lastBatteryDischargeCurrentLimitAmps = 0.0f;
    uint16_t _lastBatteryDischargeCurrentLimitBudgetUsedPermille = 0;

    uint32_t _lastRegulationPowerMeterUpdate = 0;
    float _lastRegulationPowerMeterValue = 0.0f;
    float _lastExpectedPowerMeterValue = 0.0f;
    bool _lastExpectedPowerMeterValueValid = false;
    static constexpr uint8_t _gridMedianFilterCapacity = 3;
    static constexpr uint32_t _gridMedianFilterObservationLatencyMs = 0;
    float _gridMedianFilterSamples[_gridMedianFilterCapacity] = {};
    uint8_t _gridMedianFilterCount = 0;
    uint8_t _gridMedianFilterWrite = 0;
    uint32_t _gridMedianFilterPowerMeterUpdate = 0;
    float _gridMedianFilterPowerMeterValue = 0.0f;
    bool _gridMedianFilterPowerMeterValueValid = false;
    uint32_t _lastPlannerRunMillis = 0;
    uint32_t _lastPlannerPowerMeterUpdate = 0;
    float _lastPlannerPowerMeterValue = 0.0f;
    float _lastPlannerStorageTargetPowerConsumption = 0.0f;
    float _lastPlannerTargetPowerConsumption = 0.0f;
    uint32_t _lastPlannerGridErrorUpdateMillis = 0;
    float _plannerGridOutsideErrorIntegralWattMillis = 0.0f;
    uint32_t _lastPlannerTargetBandErrorUpdateMillis = 0;
    float _plannerTargetBandErrorIntegralWattMillis = 0.0f;
    uint16_t _solarOutputLimitChargerHeadroomWatts = 0;

    static constexpr uint32_t _runtimeStateRestoreMaxAgeSeconds = 5 * 60;
    static constexpr uint32_t _runtimeStatePersistIntervalMs = 15 * 1000;
    static constexpr uint32_t _runtimeStateUnchangedRefreshIntervalMs = 60 * 1000;
    bool _runtimeStateRestoreChecked = false;
    bool _runtimeStateRestoreValid = false;
    bool _runtimeStateRestoreExpired = false;
    bool _runtimeStateRestoreConsumed = false;
    bool _runtimeStateRestoreApplied = false;
    uint32_t _runtimeStateRestoreTimestamp = 0;
    uint32_t _runtimeStateRestoreAgeSeconds = 0;
    uint32_t _runtimeStateRestoredInverterCount = 0;
    uint32_t _runtimeStateAppliedInverterCount = 0;
    std::optional<int16_t> _restoredBatteryTargetPowerConsumption = std::nullopt;
    std::vector<PowerLimiterInverter::RuntimeState> _restoredRuntimeStates;
    String _runtimeStateRestoreError;
    uint32_t _lastRuntimeStatePersistCheckMillis = 0;
    uint32_t _lastRuntimeStatePersistMillis = 0;
    uint32_t _lastRuntimeStatePersistHash = 0;
    bool _lastRuntimeStatePersistHashValid = false;
    bool _lastRuntimeStatePersistSucceeded = false;
    uint32_t _lastRuntimeStatePersistTimestamp = 0;
    uint32_t _lastRuntimeStatePersistInverterCount = 0;
    String _lastRuntimeStatePersistError;

    PowerLimiterPredictiveControl::Model _predictiveModel;
    bool _predictiveModelInitialized = false;
    PowerLimiterPredictiveControl::ControlForecast _lastPredictiveStorageForecast;
    PowerLimiterPredictiveControl::ControlForecast _lastPredictiveGlobalForecast;
    bool _lastPredictiveChargerMayBeActive = false;
    bool _lastPredictiveStorageOutputMayBeActive = false;
    std::optional<uint16_t> _oUnexpectedGridChargerZeroTargetRecoveryWatts = std::nullopt;
    uint32_t _lastLoopRuntimeMicros = 0;
    uint32_t _maxLoopRuntimeMicros = 0;
    uint32_t _lastGridChargerApplyRuntimeMicros = 0;
    uint32_t _maxGridChargerApplyRuntimeMicros = 0;

    struct PredictiveRequestHistoryRecord {
        uint32_t seq = 0;
        uint64_t serial = 0;
        uint32_t createdMillis = 0;
        uint32_t sentMillis = 0;
        uint32_t ackMillis = 0;
        uint32_t notBeforeEffectMillis = 0;
        uint32_t earliestExpectedMillis = 0;
        uint32_t typicalEffectMillis = 0;
        uint32_t latestEffectMillis = 0;
        uint32_t updatedMillis = 0;
        int16_t baseSetpointWatts = 0;
        int16_t targetSetpointWatts = 0;
        int16_t effectiveDeltaWatts = 0;
        uint8_t inverterIndex = 0xff;
        uint8_t source = 0;
        uint8_t domain = 0;
        uint8_t cause = 0;
        uint8_t state = 0;
        uint8_t effectKind = 0;
        uint8_t transitionKind = 0;
        bool capacityFullOutputPossible = false;
        bool dispatchPending = false;
    };

    static constexpr size_t _predictiveRequestHistoryDesiredCapacity = 4096;
    static constexpr size_t _predictiveRequestHistoryFallbackCapacity = 128;
    mutable std::mutex _predictiveRequestHistoryMutex;
    PredictiveRequestHistoryRecord _predictiveRequestHistoryFallback[_predictiveRequestHistoryFallbackCapacity];
    PredictiveRequestHistoryRecord* _predictiveRequestHistory = _predictiveRequestHistoryFallback;
    size_t _predictiveRequestHistoryCapacity = _predictiveRequestHistoryFallbackCapacity;
    size_t _predictiveRequestHistoryWrite = 0;
    size_t _predictiveRequestHistoryCount = 0;
    bool _predictiveRequestHistoryInPsram = false;

    struct TraceProviderSample {
        uint16_t output = 0;
        uint16_t expectedOutput = 0;
        uint16_t limit = 0;
        uint16_t expectedLimit = 0;
        uint8_t pending = 0;
        uint8_t assumed = 0;
    };

    struct TraceForecastSample {
        bool valid = false;
        int16_t virtualMeter = 0;
        int16_t gridMin = 0;
        int16_t gridNominal = 0;
        int16_t gridMax = 0;
        uint16_t pending = 0;
    };

    struct TraceStorageTargetForecastSample {
        bool valid = false;
        int16_t min = 0;
        int16_t nominal = 0;
        int16_t max = 0;
        bool dueValid = false;
        int16_t dueMin = 0;
        int16_t dueNominal = 0;
        int16_t dueMax = 0;
        bool oracleValid = false;
        int16_t oracle = 0;
        uint16_t horizonMillis = 0;
        uint16_t dueAgeMillis = 0;
        uint16_t oracleAgeMillis = 0;
    };

    struct TraceSample {
        uint32_t millis = 0;
        int16_t powerMeter = 0;
        bool powerMeterValid = false;
        int16_t target = 0;
        int16_t storageTarget = 0;
        int16_t expectedMeter = 0;
        bool expectedMeterValid = false;
        int16_t batteryBmsPower = 0;
        bool batteryBmsPowerValid = false;
        uint16_t chargerInput = 0;
        uint16_t chargerTargetInput = 0;
        bool chargerTargetInputValid = false;
        uint16_t chargerMaxInput = 0;
        uint8_t status = 0;
        TraceProviderSample battery;
        TraceProviderSample solar;
        TraceProviderSample smartBuffer;
        TraceForecastSample predictiveStorage;
        TraceForecastSample predictiveGlobal;
        TraceStorageTargetForecastSample predictiveStorageTarget;
        bool predictiveChargerMayBeActive = false;
        bool predictiveStorageOutputMayBeActive = false;
        uint16_t predictiveLedgerSize = 0;
        uint16_t predictiveActiveRequests = 0;
        uint32_t loopRuntimeMicros = 0;
    };

    static constexpr uint32_t _traceSampleWindowSeconds = 5 * 60;
    static constexpr uint32_t _traceSampleHistorySeconds = 30 * 60;
    // Keep the internal fallback small enough for ESP32 variants without PSRAM.
    static constexpr size_t _traceSampleFallbackCapacity = 60;
    mutable std::mutex _traceMutex;
    TraceSample _traceSampleFallback[_traceSampleFallbackCapacity];
    TraceSample* _traceSamples = _traceSampleFallback;
    size_t _traceSampleCapacity = _traceSampleFallbackCapacity;
    size_t _traceSampleWrite = 0;
    size_t _traceSampleCount = 0;
    bool _traceSamplesInPsram = false;

    frozen::string const& getStatusText(Status status) const;
    void announceStatus(Status status);
    void reloadConfig();
    std::pair<float, char const*> getInverterDcVoltage() const;
    float getBatteryVoltage(bool log = false) const;
    uint16_t dcPowerBusToInverterAc(uint16_t dcPower) const;
    float estimateBatteryDischargeCurrent() const;
    void resetBatteryDischargeCurrentLimitBudget();
    std::optional<float> getConfiguredBatteryDischargeCurrentLimit();
    bool shouldRequestEmergencyFullOutput();
    void resetEmergencyFullOutputThrottle();
    void emergencyFullOutput();
    void unconditionalFullSolarPassthrough();
    void resetDynamicBatteryTargetState();
    float calcBatteryTargetPowerConsumption() const;
    float calcBatteryTargetPowerConsumption(float variance, bool initialized) const;
    DynamicBatteryTargetForecastState currentDynamicBatteryTargetForecastState() const;
    void advanceDynamicBatteryTargetForecastState(
            DynamicBatteryTargetForecastState& state,
            float meterValue,
            uint32_t updateMillis) const;
    bool calcDynamicStorageTargetForecastRange(
            PowerLimiterPredictiveControl::ControlForecast const& gridForecast,
            DynamicBatteryTargetForecastState const& baseState,
            uint32_t targetMillis,
            int16_t& min,
            int16_t& nominal,
            int16_t& max) const;
    TraceStorageTargetForecastSample calcDynamicStorageTargetForecast(
            PowerLimiterPredictiveControl::ControlForecast const& gridForecast,
            uint32_t nowMillis) const;
    void resetDynamicBatteryTargetOracleProbes();
    void advanceDynamicBatteryTargetOracleProbes(
            float meterValue,
            uint32_t powerMeterUpdate);
    void enqueueDynamicBatteryTargetOracleProbe(uint32_t powerMeterUpdate);
    void rememberRegulationOutcome(float expectedPowerMeterValue, bool expectedPowerMeterValueValid);
    void resetRegulationState();
    void recordLoopRuntime(uint32_t startedMicros);
    void recordGridChargerApplyRuntime(uint32_t startedMicros);
    void loadPersistedRuntimeState();
    void persistRuntimeStateIfDue();
    bool persistRuntimeState(bool force = false);
    std::optional<PowerLimiterInverter::RuntimeState> findRestoredRuntimeState(uint64_t serial) const;
    void applyRestoredRuntimeState(PowerLimiterInverter& inverter);
    void resetPredictiveControlState();
    void syncPredictiveActuators(bool gridChargerManaged);
    void advancePredictiveLedger();
    PowerLimiterPredictiveControl::MeterSnapshot makePredictiveMeterSnapshot() const;
    void updatePredictiveForecasts();
    PowerLimiterPredictiveControl::ControlForecast getPlannerGridEstimate(
            uint32_t meterSampleMillis) const;
    bool predictiveStorageOutputMayBeActive() const;
    bool predictiveChargerMayBeActive() const;
    void recordPredictiveRequestHistory(
            PowerLimiterPredictiveControl::PendingControlRequest const& request);
    void recordPredictiveRequestHistoryChanges(
            std::vector<std::pair<uint32_t, PowerLimiterPredictiveControl::RequestState>> const& before);
    std::vector<std::pair<uint32_t, PowerLimiterPredictiveControl::RequestState>>
        predictiveRequestStates() const;
    void recordPredictiveInverterRequest(
            PowerLimiterInverter const& inverter,
            uint16_t targetSetpointWatts,
            PowerLimiterPredictiveControl::RequestDomain domain,
            PowerLimiterPredictiveControl::RequestCause cause);
    void markPredictiveInverterRequestDispatched(
            PowerLimiterInverter const& inverter,
            PowerLimiterInverter::TargetDispatchEvent const& event);
    void discardPredictiveQueuedInverterRequests(
            PowerLimiterInverter const& inverter);
    void recordPredictiveGridChargerRequest(
            uint16_t targetSetpointWatts,
            PowerLimiterPredictiveControl::RequestCause cause);
    void markPredictiveGridChargerRequestDispatched(
            GridChargers::PowerLimiterTargetDispatchEvent const& event);
    void processPredictiveGridChargerDispatchEvents();
    uint16_t applyGridChargerInputPowerIncrease(
            uint16_t increase,
            PowerLimiterPredictiveControl::RequestCause cause =
                PowerLimiterPredictiveControl::RequestCause::Normal);
    uint16_t applyGridChargerInputPowerReduction(
            uint16_t reduction,
            PowerLimiterPredictiveControl::RequestCause cause =
                PowerLimiterPredictiveControl::RequestCause::Normal);
    void initTraceBuffer();
    void initPredictiveRequestHistoryBuffer();
    void recordTraceSample(Status status);
    void updateGridMedianFilter();
    void resetGridMedianFilter();
    float getRegulationPowerMeterTotal() const;
    uint32_t getRegulationPowerMeterUpdate() const;
    uint16_t calcTargetOutput() const;
    uint16_t calcTargetOutput(float targetConsumption) const;
    uint16_t calcTargetOutput(float targetConsumption, int32_t meterAdjustmentWatts) const;
    using inverter_filter_t = std::function<bool(PowerLimiterInverter const&)>;
    uint16_t updateInverterLimits(uint16_t powerRequested, inverter_filter_t filter,
            std::string const& filterExpression, bool allowStandby = true,
            bool capOutputLimit = false,
            uint16_t outputLimitHeadroom = 0,
            PowerLimiterPredictiveControl::RequestDomain requestDomain =
                PowerLimiterPredictiveControl::RequestDomain::Storage,
            PowerLimiterPredictiveControl::RequestCause requestCause =
                PowerLimiterPredictiveControl::RequestCause::Normal,
            bool forceStandbyReassert = false);
    uint16_t getCurrentInvertersOutputAcWatts(inverter_filter_t filter) const;
    uint16_t calcPowerBusUsage(uint16_t powerRequested);
    uint32_t getMaxInverterRadioQueueSize() const;
    bool isInverterRadioQueueCongested() const;
    bool updateInverters(bool throttleRfQueue = true);
    uint16_t getSolarPassthroughPower() const;
    std::optional<uint16_t> getBatteryDischargeLimit();
    float getBatteryInvertersOutputAcWatts() const;
    uint16_t getCurrentInvertersOutputAcWatts() const;
    uint16_t getBatteryInvertersMinOutputAcWatts() const;

    bool testThreshold(float socThreshold, float voltThreshold,
            std::function<bool(float, float)> compare) const;
    bool isStartThresholdReached() const;
    bool isStopThresholdReached() const;
    bool isBelowStopThreshold() const;
    void calcNextInverterRestart();
    bool isSolarPassThroughEnabled() const;
};

extern PowerLimiterClass PowerLimiter;
