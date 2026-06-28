// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace PowerLimiterPredictiveControl {

constexpr float DefaultSmallCorrectionDampingThresholdWatts = 150.0f;

enum class ActuatorKind : uint8_t {
    Battery,
    Charger,
    Solar,
    SmartBuffer,
};

enum class RequestDomain : uint8_t {
    Storage,
    Global,
};

enum class ForecastDomain : uint8_t {
    Storage,
    Global,
};

enum class RequestCause : uint8_t {
    Normal,
    Safety,
    Manual,
    Recovery,
    Thermal,
};

enum class RequestState : uint8_t {
    Queued,
    SupersededBeforeSend,
    Sent,
    Accepted,
    Rejected,
    FailedNoEffect,
    FailedAmbiguous,
    SettledByMeter,
    SettledByTelemetry,
    SettledByTimeout,
    SettledSuperseded,
    SettledDiscarded,
};

enum class EffectKind : uint8_t {
    Deterministic,
    SolarCapacityLimited,
    Ambiguous,
};

enum class TransitionKind : uint8_t {
    Setpoint,
    Startup,
    Standby,
};

enum class CorrectionAction : uint8_t {
    Hold,
    MoreEffectiveOutput,
    LessEffectiveOutput,
};

struct ActuatorCapabilities {
    float minActiveSetpointWatts = 0.0f;
    std::optional<float> maxSetpointWatts = std::nullopt;
    float selfConsumptionWatts = 0.0f;

    float legalizeSetpoint(float desiredSetpointWatts) const;
};

struct ActuatorState {
    std::string actuatorKey;
    ActuatorKind kind = ActuatorKind::Solar;
    float safeSetpointWatts = 0.0f;
    float requestedSetpointWatts = -1.0f;
    std::optional<float> measuredOutputWatts = std::nullopt;
    ActuatorCapabilities capabilities;

    void normalize();
    float legalizeSetpoint(float desiredSetpointWatts) const;
    float effectiveOutputForSetpoint(float setpointWatts) const;
};

struct PendingControlRequest {
    uint32_t seq = 0;
    std::string actuatorKey;
    RequestDomain domain = RequestDomain::Storage;
    float baseSetpointWatts = 0.0f;
    float targetSetpointWatts = 0.0f;
    float effectiveDeltaWatts = 0.0f;
    RequestCause cause = RequestCause::Normal;
    RequestState state = RequestState::Accepted;
    EffectKind effectKind = EffectKind::Deterministic;
    TransitionKind transitionKind = TransitionKind::Setpoint;
    uint32_t createdMillis = 0;
    std::optional<uint32_t> sentMillis = std::nullopt;
    std::optional<uint32_t> ackMillis = std::nullopt;
    std::optional<uint32_t> notBeforeEffectMillis = std::nullopt;
    std::optional<uint32_t> earliestExpectedMillis = std::nullopt;
    std::optional<uint32_t> typicalEffectMillis = std::nullopt;
    std::optional<uint32_t> latestEffectMillis = std::nullopt;
    bool capacityFullOutputPossible = true;
    bool dispatchPending = false;

    float setpointDeltaWatts() const;
    float meterDeltaWatts() const;
    bool isPhysicalPending() const;
};

struct MeterSnapshot {
    float physicalGridWatts = 0.0f;
    uint32_t meterSampleMillis = 0;
};

struct ControlForecast {
    bool valid = false;
    ForecastDomain domain = ForecastDomain::Storage;
    uint32_t meterSampleMillis = 0;
    float virtualMeterWatts = 0.0f;
    float gridMinWatts = 0.0f;
    float gridNominalWatts = 0.0f;
    float gridMaxWatts = 0.0f;
    uint16_t pendingCount = 0;
};

struct CorrectionDecision {
    CorrectionAction action = CorrectionAction::Hold;
    float residualEffectiveWatts = 0.0f;
    const char* reason = "inside_corridor";
    float nearestCorridorEdgeWatts = 0.0f;
};

class Model {
public:
    Model() = default;
    explicit Model(std::vector<ActuatorState> actuators);

    void addActuator(ActuatorState actuator);
    ActuatorState const* findActuator(std::string const& actuatorKey) const;
    ActuatorState* findActuator(std::string const& actuatorKey);

    PendingControlRequest& queueRequest(
            std::string const& actuatorKey,
            RequestDomain domain,
            float targetSetpointWatts,
            float effectiveDeltaWatts,
            RequestCause cause = RequestCause::Normal,
            EffectKind effectKind = EffectKind::Deterministic,
            TransitionKind transitionKind = TransitionKind::Setpoint,
            uint32_t createdMillis = 0);
    PendingControlRequest& markQueuedRequestSent(
            uint32_t seq,
            uint32_t sentMillis,
            RequestState state = RequestState::Sent,
            std::optional<uint32_t> ackMillis = std::nullopt,
            std::optional<uint32_t> notBeforeEffectMillis = std::nullopt,
            std::optional<uint32_t> earliestExpectedMillis = std::nullopt,
            std::optional<uint32_t> typicalEffectMillis = std::nullopt,
            std::optional<uint32_t> latestEffectMillis = std::nullopt);
    PendingControlRequest* markLatestQueuedRequestSent(
            std::string const& actuatorKey,
            float targetSetpointWatts,
            TransitionKind transitionKind,
            uint32_t sentMillis,
            uint32_t latestEffectMillis);
    PendingControlRequest* markLatestQueuedRequestDispatchPending(
            std::string const& actuatorKey,
            float targetSetpointWatts,
            TransitionKind transitionKind);
    void discardQueuedRequests(
            std::string const& actuatorKey,
            float currentSetpointWatts);
    size_t discardStaleQueuedRequests(uint32_t nowMillis, uint32_t maxAgeMillis);
    PendingControlRequest& appendPhysicalRequest(PendingControlRequest request);
    void classifyFailure(uint32_t seq, RequestState state);
    void advanceLedger(uint32_t meterSampleMillis);
    void pruneInactiveRequests(size_t retainedInactiveRequests = 0);

    ControlForecast forecast(MeterSnapshot const& snapshot, ForecastDomain domain) const;
    ControlForecast currentEstimate(
            MeterSnapshot const& baseSnapshot,
            ForecastDomain domain,
            uint32_t meterSampleMillis) const;
    std::vector<float> possibleMeterDeltasForActuator(
            std::string const& actuatorKey,
            ForecastDomain domain,
            uint32_t meterSampleMillis) const;
    bool actuatorMayBePhysicallyActive(
            std::string const& actuatorKey,
            float epsilonWatts = 0.5f,
            bool assumeDispatchedStandbyInactive = false) const;
    bool anyActuatorMayBePhysicallyActive(
            ActuatorKind kind,
            float epsilonWatts = 0.5f,
            bool assumeDispatchedStandbyInactive = false) const;
    bool hasPendingGlobalIncrease(
            ActuatorKind kind,
            float epsilonWatts = 0.5f) const;
    bool hasActivePowerStateTransition(std::string const& actuatorKey) const;
    bool storageOutputAndChargerMayOverlap(float epsilonWatts = 0.5f) const;

    std::vector<PendingControlRequest> const& ledger() const { return _ledger; }
    size_t activeRequestCount() const;

private:
    struct PrefixState {
        uint16_t prefix = 0;
        float delta = 0.0f;
        std::vector<float> scales;
    };

    float virtualMeter(MeterSnapshot const& snapshot, ForecastDomain domain) const;
    std::vector<std::vector<PendingControlRequest const*>> activeChains() const;
    std::vector<std::vector<PendingControlRequest*>> activeChains();
    bool latestPhysicalRequestAssumesStandbyInactive(
            std::string const& actuatorKey,
            float epsilonWatts) const;
    std::vector<std::pair<float, float>> actuatorMeterDeltaPairs(
            std::vector<PendingControlRequest const*> const& chain,
            ForecastDomain domain,
            uint32_t meterSampleMillis) const;
    std::vector<std::pair<float, float>> actuatorVisibleDeltaPairs(
            std::vector<PendingControlRequest const*> const& chain,
            ForecastDomain domain,
            uint32_t baseMeterSampleMillis,
            uint32_t meterSampleMillis) const;
    std::vector<PrefixState> possibleVisiblePrefixStates(
            std::vector<PendingControlRequest const*> const& chain,
            ForecastDomain domain,
            uint32_t meterSampleMillis) const;
    std::vector<PrefixState> possibleFinalPrefixStates(
            std::vector<PendingControlRequest const*> const& chain,
            ForecastDomain domain) const;
    std::vector<PrefixState> prefixDeltaOptions(
            std::vector<PendingControlRequest const*> const& chain,
            ForecastDomain domain,
            uint16_t minPrefix,
            uint16_t maxPrefix) const;
    bool finalStateIsConsistentWithVisibleState(
            PrefixState const& visible,
            PrefixState const& final) const;
    float meterDelta(
            PendingControlRequest const& request,
            ForecastDomain domain,
            float scale) const;
    bool requestIsRelevantToDomain(
            PendingControlRequest const& request,
            ForecastDomain domain) const;
    std::vector<float> effectScales(PendingControlRequest const& request) const;
    bool couldBeVisible(
            PendingControlRequest const& request,
            uint32_t meterSampleMillis) const;
    bool mustBeVisible(
            PendingControlRequest const& request,
            uint32_t meterSampleMillis) const;
    void recomputeRequestedSetpoint(std::string const& actuatorKey);
    PendingControlRequest* requestBySeq(uint32_t seq);

    std::vector<ActuatorState> _actuators;
    std::vector<PendingControlRequest> _ledger;
    uint32_t _nextSeq = 1;
};

CorrectionDecision decideCorrection(
        float targetWatts,
        ControlForecast const& forecast,
        float hysteresisWatts = 0.0f,
        float smallCorrectionDampingThresholdWatts =
            DefaultSmallCorrectionDampingThresholdWatts);
CorrectionDecision decideSolarTargetCorrection(
        float targetWatts,
        ControlForecast const& forecast,
        float hysteresisWatts = 0.0f,
        float smallCorrectionDampingThresholdWatts =
            DefaultSmallCorrectionDampingThresholdWatts);
ControlForecast forecastWithPlannedMeterDelta(
        ControlForecast forecast,
        float plannedMeterDeltaWatts);
float corridorUncertaintyWatts(ControlForecast const& forecast);
float distanceOutsideCorridor(
        float valueWatts,
        ControlForecast const& forecast);
float targetChangeDistanceWatts(
        float storageTargetWatts,
        float globalTargetWatts,
        float referenceStorageTargetWatts,
        float referenceGlobalTargetWatts);
float targetBandErrorWatts(
        float storageTargetWatts,
        float globalTargetWatts,
        float gridWatts);
float targetBandErrorWatts(
        float storageTargetWatts,
        float globalTargetWatts,
        ControlForecast const& forecast);
float targetBandErrorWatts(
        float storageTargetWatts,
        float globalTargetWatts,
        float gridWatts,
        ControlForecast const& forecast);
float outsideCorridorUncertaintyRatio(
        float distanceOutsideWatts,
        float uncertaintyWatts);

bool dynamicTargetLearningAllowed(ControlForecast const& storageForecast);
bool gridChargerAvailablePowerLimitedForFlexibleLoad(
        CorrectionAction storageAction,
        uint16_t gridChargerExpectedInputWatts,
        uint16_t gridChargerMaxInputWatts);
std::optional<float> deterministicExpectedMeterValue(
        ControlForecast const& forecast,
        float toleranceWatts = 1.0f);
float globalTargetFromExportOffset(float storageTargetWatts, float exportOffsetWatts);
float validateTargetOrdering(
        float storageTargetWatts,
        float globalTargetWatts,
        bool clamp = false);

} // namespace PowerLimiterPredictiveControl
