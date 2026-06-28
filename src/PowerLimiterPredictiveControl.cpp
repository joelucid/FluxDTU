// SPDX-License-Identifier: GPL-2.0-or-later

#include "PowerLimiterPredictiveControl.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>

namespace PowerLimiterPredictiveControl {

namespace {

constexpr uint16_t GridChargerAvailablePowerLimitMinimumHeadroomWatts = 10;
bool isPendingRequestedState(RequestState state)
{
    return state == RequestState::Queued
        || state == RequestState::Sent
        || state == RequestState::Accepted
        || state == RequestState::FailedAmbiguous;
}

bool isActiveForForecast(PendingControlRequest const& request)
{
    return isPendingRequestedState(request.state);
}

bool hasUncertainFinalEffect(PendingControlRequest const& request)
{
    if (request.state == RequestState::Queued && request.dispatchPending) {
        return true;
    }

    if (request.state == RequestState::FailedAmbiguous
            || request.effectKind == EffectKind::Ambiguous) {
        return true;
    }

    return request.effectKind == EffectKind::SolarCapacityLimited
        && request.effectiveDeltaWatts > 0.0f;
}

bool floatEqual(float lhs, float rhs)
{
    return std::fabs(lhs - rhs) <= 0.001f;
}

float dampSmallCorrectionResidual(
        float residualWatts,
        float smallCorrectionDampingThresholdWatts)
{
    if (residualWatts == 0.0f) { return residualWatts; }

    auto const magnitude = std::fabs(residualWatts);
    if (magnitude >= smallCorrectionDampingThresholdWatts) {
        return residualWatts;
    }

    return residualWatts * 0.5f;
}

CorrectionDecision dampSmallCorrection(
        CorrectionDecision decision,
        float smallCorrectionDampingThresholdWatts)
{
    if (decision.action == CorrectionAction::Hold) { return decision; }

    decision.residualEffectiveWatts =
        dampSmallCorrectionResidual(
                decision.residualEffectiveWatts,
                smallCorrectionDampingThresholdWatts);
    return decision;
}

void appendUniqueFloat(std::vector<float>& values, float value)
{
    auto const iter = std::find_if(values.begin(), values.end(),
            [value](float existing) { return floatEqual(existing, value); });
    if (iter == values.end()) {
        values.push_back(value);
    }
}

} // namespace

float ActuatorCapabilities::legalizeSetpoint(float desiredSetpointWatts) const
{
    if (desiredSetpointWatts <= 0.0f) {
        return 0.0f;
    }

    auto legal = std::max(desiredSetpointWatts, minActiveSetpointWatts);
    if (maxSetpointWatts) {
        legal = std::min(legal, *maxSetpointWatts);
    }
    return legal;
}

void ActuatorState::normalize()
{
    safeSetpointWatts = capabilities.legalizeSetpoint(safeSetpointWatts);
    if (requestedSetpointWatts < 0.0f) {
        requestedSetpointWatts = safeSetpointWatts;
    } else {
        requestedSetpointWatts = capabilities.legalizeSetpoint(requestedSetpointWatts);
    }
}

float ActuatorState::legalizeSetpoint(float desiredSetpointWatts) const
{
    return capabilities.legalizeSetpoint(desiredSetpointWatts);
}

float ActuatorState::effectiveOutputForSetpoint(float setpointWatts) const
{
    auto const legalSetpoint = legalizeSetpoint(setpointWatts);
    if (legalSetpoint <= 0.0f) {
        return 0.0f;
    }

    if (kind == ActuatorKind::Charger) {
        return -(legalSetpoint + capabilities.selfConsumptionWatts);
    }

    return legalSetpoint;
}

float PendingControlRequest::setpointDeltaWatts() const
{
    return targetSetpointWatts - baseSetpointWatts;
}

float PendingControlRequest::meterDeltaWatts() const
{
    return -effectiveDeltaWatts;
}

bool PendingControlRequest::isPhysicalPending() const
{
    return dispatchPending
        || state == RequestState::Sent
        || state == RequestState::Accepted
        || state == RequestState::FailedAmbiguous;
}

Model::Model(std::vector<ActuatorState> actuators)
{
    for (auto& actuator : actuators) {
        addActuator(actuator);
    }
}

void Model::addActuator(ActuatorState actuator)
{
    actuator.normalize();
    _actuators.push_back(actuator);
}

ActuatorState const* Model::findActuator(std::string const& actuatorKey) const
{
    auto const iter = std::find_if(_actuators.begin(), _actuators.end(),
            [&actuatorKey](auto const& actuator) {
                return actuator.actuatorKey == actuatorKey;
            });
    return iter == _actuators.end() ? nullptr : &(*iter);
}

ActuatorState* Model::findActuator(std::string const& actuatorKey)
{
    auto const iter = std::find_if(_actuators.begin(), _actuators.end(),
            [&actuatorKey](auto const& actuator) {
                return actuator.actuatorKey == actuatorKey;
            });
    return iter == _actuators.end() ? nullptr : &(*iter);
}

PendingControlRequest& Model::queueRequest(
        std::string const& actuatorKey,
        RequestDomain domain,
        float targetSetpointWatts,
        float effectiveDeltaWatts,
        RequestCause cause,
        EffectKind effectKind,
        TransitionKind transitionKind,
        uint32_t createdMillis)
{
    auto* actuator = findActuator(actuatorKey);
    if (actuator == nullptr) {
        throw std::invalid_argument("unknown actuator");
    }

    PendingControlRequest request;
    request.seq = _nextSeq++;
    request.actuatorKey = actuatorKey;
    request.domain = domain;
    request.baseSetpointWatts = actuator->requestedSetpointWatts;
    request.targetSetpointWatts = actuator->legalizeSetpoint(targetSetpointWatts);
    request.effectiveDeltaWatts = effectiveDeltaWatts;
    request.cause = cause;
    request.state = RequestState::Queued;
    request.effectKind = effectKind;
    request.transitionKind = transitionKind;
    request.createdMillis = createdMillis;

    _ledger.push_back(request);
    actuator->requestedSetpointWatts = request.targetSetpointWatts;
    return _ledger.back();
}

PendingControlRequest& Model::markQueuedRequestSent(
        uint32_t seq,
        uint32_t sentMillis,
        RequestState state,
        std::optional<uint32_t> ackMillis,
        std::optional<uint32_t> notBeforeEffectMillis,
        std::optional<uint32_t> earliestExpectedMillis,
        std::optional<uint32_t> typicalEffectMillis,
        std::optional<uint32_t> latestEffectMillis)
{
    if (state != RequestState::Sent && state != RequestState::Accepted) {
        throw std::invalid_argument("sent queued request must become sent or accepted");
    }

    auto* request = requestBySeq(seq);
    if (request == nullptr) {
        throw std::out_of_range("unknown request sequence");
    }
    if (request->state != RequestState::Queued) {
        throw std::invalid_argument("only queued requests can be marked sent");
    }

    request->state = state;
    request->dispatchPending = false;
    request->sentMillis = sentMillis;
    request->ackMillis = ackMillis;
    request->notBeforeEffectMillis = notBeforeEffectMillis;
    request->earliestExpectedMillis = earliestExpectedMillis;
    request->typicalEffectMillis = typicalEffectMillis;
    request->latestEffectMillis = latestEffectMillis;
    return *request;
}

PendingControlRequest* Model::markLatestQueuedRequestSent(
        std::string const& actuatorKey,
        float targetSetpointWatts,
        TransitionKind transitionKind,
        uint32_t sentMillis,
        uint32_t latestEffectMillis)
{
    auto matches = [&](PendingControlRequest const& request) {
        return request.actuatorKey == actuatorKey
            && request.state == RequestState::Queued
            && request.transitionKind == transitionKind
            && floatEqual(request.targetSetpointWatts, targetSetpointWatts);
    };

    for (auto iter = _ledger.rbegin(); iter != _ledger.rend(); ++iter) {
        if (!matches(*iter) || !iter->dispatchPending) {
            continue;
        }

        return &markQueuedRequestSent(
                iter->seq,
                sentMillis,
                RequestState::Sent,
                std::nullopt,
                sentMillis,
                std::nullopt,
                std::nullopt,
                latestEffectMillis);
    }

    for (auto iter = _ledger.rbegin(); iter != _ledger.rend(); ++iter) {
        if (!matches(*iter)) {
            continue;
        }

        return &markQueuedRequestSent(
                iter->seq,
                sentMillis,
                RequestState::Sent,
                std::nullopt,
                sentMillis,
                std::nullopt,
                std::nullopt,
                latestEffectMillis);
    }

    return nullptr;
}

PendingControlRequest* Model::markLatestQueuedRequestDispatchPending(
        std::string const& actuatorKey,
        float targetSetpointWatts,
        TransitionKind transitionKind)
{
    for (auto iter = _ledger.rbegin(); iter != _ledger.rend(); ++iter) {
        if (iter->actuatorKey != actuatorKey
                || iter->state != RequestState::Queued
                || iter->transitionKind != transitionKind
                || !floatEqual(iter->targetSetpointWatts, targetSetpointWatts)) {
            continue;
        }

        iter->dispatchPending = true;
        return &(*iter);
    }

    return nullptr;
}

void Model::discardQueuedRequests(
        std::string const& actuatorKey,
        float currentSetpointWatts)
{
    auto* actuator = findActuator(actuatorKey);
    if (actuator == nullptr) {
        return;
    }

    bool discarded = false;
    for (auto& request : _ledger) {
        if (request.actuatorKey != actuatorKey
                || request.state != RequestState::Queued) {
            continue;
        }

        request.state = RequestState::SettledDiscarded;
        discarded = true;
    }

    if (!discarded) {
        return;
    }

    actuator->safeSetpointWatts =
        actuator->legalizeSetpoint(currentSetpointWatts);
    recomputeRequestedSetpoint(actuatorKey);
}

size_t Model::discardStaleQueuedRequests(uint32_t nowMillis, uint32_t maxAgeMillis)
{
    if (maxAgeMillis == 0) {
        return 0;
    }

    size_t discarded = 0;
    std::vector<std::string> affectedActuators;
    auto const halfOfAllMillis = std::numeric_limits<uint32_t>::max() / 2;
    for (auto& request : _ledger) {
        if (request.state != RequestState::Queued
                || request.createdMillis == 0
                || request.sentMillis) {
            continue;
        }

        auto const ageMillis = nowMillis - request.createdMillis;
        if (ageMillis >= halfOfAllMillis || ageMillis <= maxAgeMillis) {
            continue;
        }

        request.state = RequestState::SettledDiscarded;
        request.dispatchPending = false;
        ++discarded;
        if (std::find(affectedActuators.begin(), affectedActuators.end(), request.actuatorKey)
                == affectedActuators.end()) {
            affectedActuators.push_back(request.actuatorKey);
        }
    }

    for (auto const& actuatorKey : affectedActuators) {
        recomputeRequestedSetpoint(actuatorKey);
    }

    return discarded;
}

PendingControlRequest& Model::appendPhysicalRequest(PendingControlRequest request)
{
    auto* actuator = findActuator(request.actuatorKey);
    if (actuator == nullptr) {
        throw std::invalid_argument("unknown actuator");
    }

    auto const duplicateSeq = std::any_of(_ledger.begin(), _ledger.end(),
            [&request](auto const& existing) { return existing.seq == request.seq && request.seq != 0; });
    if (duplicateSeq) {
        throw std::invalid_argument("duplicate request sequence");
    }

    if (!floatEqual(request.baseSetpointWatts, actuator->requestedSetpointWatts)) {
        throw std::invalid_argument("request base does not match actuator requested setpoint");
    }

    if (request.seq == 0) {
        request.seq = _nextSeq++;
    } else {
        _nextSeq = std::max(_nextSeq, request.seq + 1);
    }

    _ledger.push_back(request);
    actuator->requestedSetpointWatts = request.targetSetpointWatts;
    return _ledger.back();
}

void Model::classifyFailure(uint32_t seq, RequestState state)
{
    if (state != RequestState::FailedNoEffect && state != RequestState::FailedAmbiguous) {
        throw std::invalid_argument("failure state must be failed-no-effect or ambiguous");
    }

    auto* request = requestBySeq(seq);
    if (request == nullptr) {
        throw std::out_of_range("unknown request sequence");
    }

    request->state = state;
    recomputeRequestedSetpoint(request->actuatorKey);
}

void Model::advanceLedger(uint32_t meterSampleMillis)
{
    for (auto& chain : activeChains()) {
        if (chain.empty()) {
            continue;
        }

        auto* actuator = findActuator(chain.front()->actuatorKey);
        if (actuator == nullptr) {
            continue;
        }

        for (auto* request : chain) {
            if (!request->latestEffectMillis || meterSampleMillis < *request->latestEffectMillis) {
                break;
            }

            if (request->state == RequestState::FailedAmbiguous) {
                break;
            }

            bool const timeoutProvesTarget =
                request->effectKind != EffectKind::SolarCapacityLimited
                || request->effectiveDeltaWatts <= 0.0f;
            request->state = RequestState::SettledByTimeout;
            if (timeoutProvesTarget) {
                actuator->safeSetpointWatts = request->targetSetpointWatts;
            }
        }

        recomputeRequestedSetpoint(actuator->actuatorKey);
    }
}

void Model::pruneInactiveRequests(size_t retainedInactiveRequests)
{
    size_t retained = 0;
    auto iter = _ledger.end();
    while (iter != _ledger.begin()) {
        --iter;

        if (isPendingRequestedState(iter->state)) {
            continue;
        }

        if (retained < retainedInactiveRequests) {
            ++retained;
            continue;
        }

        iter = _ledger.erase(iter);
    }
}

ControlForecast Model::forecast(MeterSnapshot const& snapshot, ForecastDomain domain) const
{
    auto const baseVirtualMeter = virtualMeter(snapshot, domain);
    std::vector<float> futureValues { baseVirtualMeter };
    uint16_t pendingCount = 0;

    for (auto const& chain : activeChains()) {
        std::vector<PendingControlRequest const*> domainChain;
        for (auto const* request : chain) {
            if (requestIsRelevantToDomain(*request, domain)) {
                domainChain.push_back(request);
            }
        }
        if (domainChain.empty()) {
            continue;
        }

        pendingCount += static_cast<uint16_t>(domainChain.size());
        auto const pairs = actuatorMeterDeltaPairs(
                domainChain,
                domain,
                snapshot.meterSampleMillis);

        std::vector<float> nextValues;
        nextValues.reserve(futureValues.size() * pairs.size());
        for (auto const value : futureValues) {
            for (auto const& pair : pairs) {
                auto const visible = pair.first;
                auto const final = pair.second;
                nextValues.push_back(value + final - visible);
            }
        }
        futureValues = nextValues;
    }

    auto const minmax = std::minmax_element(futureValues.begin(), futureValues.end());
    auto const sum = std::accumulate(futureValues.begin(), futureValues.end(), 0.0f);

    return {
        true,
        domain,
        snapshot.meterSampleMillis,
        baseVirtualMeter,
        *minmax.first,
        sum / static_cast<float>(futureValues.size()),
        *minmax.second,
        pendingCount,
    };
}

ControlForecast Model::currentEstimate(
        MeterSnapshot const& baseSnapshot,
        ForecastDomain domain,
        uint32_t meterSampleMillis) const
{
    auto const baseVirtualMeter = virtualMeter(baseSnapshot, domain);
    std::vector<float> currentValues { baseVirtualMeter };
    uint16_t pendingCount = 0;

    for (auto const& chain : activeChains()) {
        std::vector<PendingControlRequest const*> domainChain;
        for (auto const* request : chain) {
            if (requestIsRelevantToDomain(*request, domain)) {
                domainChain.push_back(request);
            }
        }
        if (domainChain.empty()) {
            continue;
        }

        pendingCount += static_cast<uint16_t>(domainChain.size());
        auto const pairs = actuatorVisibleDeltaPairs(
                domainChain,
                domain,
                baseSnapshot.meterSampleMillis,
                meterSampleMillis);

        std::vector<float> nextValues;
        nextValues.reserve(currentValues.size() * pairs.size());
        for (auto const value : currentValues) {
            for (auto const& pair : pairs) {
                auto const baseVisible = pair.first;
                auto const currentVisible = pair.second;
                nextValues.push_back(value + currentVisible - baseVisible);
            }
        }
        currentValues = nextValues;
    }

    auto const minmax = std::minmax_element(currentValues.begin(), currentValues.end());
    auto const sum = std::accumulate(currentValues.begin(), currentValues.end(), 0.0f);

    return {
        true,
        domain,
        meterSampleMillis,
        baseVirtualMeter,
        *minmax.first,
        sum / static_cast<float>(currentValues.size()),
        *minmax.second,
        pendingCount,
    };
}

std::vector<float> Model::possibleMeterDeltasForActuator(
        std::string const& actuatorKey,
        ForecastDomain domain,
        uint32_t meterSampleMillis) const
{
    std::vector<PendingControlRequest const*> chain;
    for (auto const& request : _ledger) {
        if (request.actuatorKey == actuatorKey && request.isPhysicalPending()) {
            chain.push_back(&request);
        }
    }
    std::sort(chain.begin(), chain.end(),
            [](auto const* lhs, auto const* rhs) { return lhs->seq < rhs->seq; });

    std::vector<float> result;
    for (auto const& pair : actuatorMeterDeltaPairs(chain, domain, meterSampleMillis)) {
        appendUniqueFloat(result, pair.first);
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool Model::actuatorMayBePhysicallyActive(
        std::string const& actuatorKey,
        float epsilonWatts,
        bool assumeDispatchedStandbyInactive) const
{
    auto const* actuator = findActuator(actuatorKey);
    if (actuator == nullptr) {
        return false;
    }

    if (assumeDispatchedStandbyInactive
            && latestPhysicalRequestAssumesStandbyInactive(actuatorKey, epsilonWatts)) {
        return false;
    }

    if (actuator->safeSetpointWatts > epsilonWatts) {
        return true;
    }

    if (actuator->measuredOutputWatts && *actuator->measuredOutputWatts > epsilonWatts) {
        return true;
    }

    for (auto const& request : _ledger) {
        if (request.actuatorKey != actuatorKey || !request.isPhysicalPending()) {
            continue;
        }

        if (request.baseSetpointWatts > epsilonWatts
                || request.targetSetpointWatts > epsilonWatts) {
            return true;
        }
    }

    return false;
}

bool Model::anyActuatorMayBePhysicallyActive(
        ActuatorKind kind,
        float epsilonWatts,
        bool assumeDispatchedStandbyInactive) const
{
    for (auto const& actuator : _actuators) {
        if (actuator.kind == kind
                && actuatorMayBePhysicallyActive(
                        actuator.actuatorKey,
                        epsilonWatts,
                        assumeDispatchedStandbyInactive)) {
            return true;
        }
    }
    return false;
}

bool Model::hasPendingGlobalIncrease(ActuatorKind kind, float epsilonWatts) const
{
    for (auto const& request : _ledger) {
        if (!isActiveForForecast(request)) {
            continue;
        }
        if (request.domain != RequestDomain::Global) {
            continue;
        }
        if (request.effectiveDeltaWatts <= epsilonWatts) {
            continue;
        }

        auto const* actuator = findActuator(request.actuatorKey);
        if (actuator != nullptr && actuator->kind == kind) {
            return true;
        }
    }
    return false;
}

bool Model::hasActivePowerStateTransition(std::string const& actuatorKey) const
{
    return std::any_of(_ledger.begin(), _ledger.end(),
            [&actuatorKey](auto const& request) {
                return request.actuatorKey == actuatorKey
                    && isActiveForForecast(request)
                    && (request.transitionKind == TransitionKind::Startup
                        || request.transitionKind == TransitionKind::Standby);
            });
}

bool Model::storageOutputAndChargerMayOverlap(float epsilonWatts) const
{
    auto storageOutputActive = false;
    auto chargerActive = false;

    for (auto const& actuator : _actuators) {
        if (!actuatorMayBePhysicallyActive(actuator.actuatorKey, epsilonWatts)) {
            continue;
        }

        if (actuator.kind == ActuatorKind::Charger) {
            chargerActive = true;
        } else if (actuator.kind == ActuatorKind::Battery
                || actuator.kind == ActuatorKind::SmartBuffer) {
            storageOutputActive = true;
        }
    }

    return storageOutputActive && chargerActive;
}

size_t Model::activeRequestCount() const
{
    return static_cast<size_t>(std::count_if(_ledger.begin(), _ledger.end(),
            [](auto const& request) {
                return isActiveForForecast(request);
            }));
}

float Model::virtualMeter(MeterSnapshot const& snapshot, ForecastDomain /*domain*/) const
{
    return snapshot.physicalGridWatts;
}

std::vector<std::vector<PendingControlRequest const*>> Model::activeChains() const
{
    std::map<std::string, std::vector<PendingControlRequest const*>> chainsByActuator;
    for (auto const& request : _ledger) {
        if (isActiveForForecast(request)) {
            chainsByActuator[request.actuatorKey].push_back(&request);
        }
    }

    std::vector<std::vector<PendingControlRequest const*>> chains;
    chains.reserve(chainsByActuator.size());
    for (auto& entry : chainsByActuator) {
        std::sort(entry.second.begin(), entry.second.end(),
                [](auto const* lhs, auto const* rhs) { return lhs->seq < rhs->seq; });
        chains.push_back(entry.second);
    }
    return chains;
}

std::vector<std::vector<PendingControlRequest*>> Model::activeChains()
{
    std::map<std::string, std::vector<PendingControlRequest*>> chainsByActuator;
    for (auto& request : _ledger) {
        if (isActiveForForecast(request)) {
            chainsByActuator[request.actuatorKey].push_back(&request);
        }
    }

    std::vector<std::vector<PendingControlRequest*>> chains;
    chains.reserve(chainsByActuator.size());
    for (auto& entry : chainsByActuator) {
        std::sort(entry.second.begin(), entry.second.end(),
                [](auto const* lhs, auto const* rhs) { return lhs->seq < rhs->seq; });
        chains.push_back(entry.second);
    }
    return chains;
}

bool Model::latestPhysicalRequestAssumesStandbyInactive(
        std::string const& actuatorKey,
        float epsilonWatts) const
{
    std::optional<bool> assumeInactive;
    for (auto const& request : _ledger) {
        if (request.actuatorKey != actuatorKey || !request.isPhysicalPending()) {
            continue;
        }

        assumeInactive =
            request.state != RequestState::FailedAmbiguous
            && request.transitionKind == TransitionKind::Standby
            && request.targetSetpointWatts <= epsilonWatts;
    }

    return assumeInactive.value_or(false);
}

std::vector<std::pair<float, float>> Model::actuatorMeterDeltaPairs(
        std::vector<PendingControlRequest const*> const& chain,
        ForecastDomain domain,
        uint32_t meterSampleMillis) const
{
    if (chain.empty()) {
        return { { 0.0f, 0.0f } };
    }

    auto const visibleDomain =
        domain == ForecastDomain::Storage ? ForecastDomain::Global : domain;
    auto const visibleDeltas = possibleVisiblePrefixStates(
            chain,
            visibleDomain,
            meterSampleMillis);
    auto const finalDeltas = possibleFinalPrefixStates(chain, domain);

    std::vector<std::pair<float, float>> result;
    for (auto const& visible : visibleDeltas) {
        for (auto const& final : finalDeltas) {
            if (finalStateIsConsistentWithVisibleState(visible, final)) {
                result.push_back({ visible.delta, final.delta });
            }
        }
    }
    return result;
}

std::vector<std::pair<float, float>> Model::actuatorVisibleDeltaPairs(
        std::vector<PendingControlRequest const*> const& chain,
        ForecastDomain domain,
        uint32_t baseMeterSampleMillis,
        uint32_t meterSampleMillis) const
{
    if (chain.empty()) {
        return { { 0.0f, 0.0f } };
    }

    auto const visibleDomain =
        domain == ForecastDomain::Storage ? ForecastDomain::Global : domain;
    auto const baseDeltas = possibleVisiblePrefixStates(
            chain,
            visibleDomain,
            baseMeterSampleMillis);
    auto const currentDeltas = possibleVisiblePrefixStates(
            chain,
            visibleDomain,
            meterSampleMillis);

    std::vector<std::pair<float, float>> result;
    for (auto const& base : baseDeltas) {
        for (auto const& current : currentDeltas) {
            if (finalStateIsConsistentWithVisibleState(base, current)) {
                result.push_back({ base.delta, current.delta });
            }
        }
    }

    if (result.empty()) {
        return { { 0.0f, 0.0f } };
    }

    return result;
}

std::vector<Model::PrefixState> Model::possibleVisiblePrefixStates(
        std::vector<PendingControlRequest const*> const& chain,
        ForecastDomain domain,
        uint32_t meterSampleMillis) const
{
    uint16_t maxPrefix = 0;
    for (auto const* request : chain) {
        if (!couldBeVisible(*request, meterSampleMillis)) {
            break;
        }
        ++maxPrefix;
    }

    uint16_t minPrefix = 0;
    for (uint16_t index = 0; index < chain.size(); ++index) {
        if (!mustBeVisible(*chain[index], meterSampleMillis)) {
            break;
        }
        minPrefix = index + 1;
    }

    return prefixDeltaOptions(chain, domain, minPrefix, maxPrefix);
}

std::vector<Model::PrefixState> Model::possibleFinalPrefixStates(
        std::vector<PendingControlRequest const*> const& chain,
        ForecastDomain domain) const
{
    auto const size = static_cast<uint16_t>(chain.size());
    auto minPrefix = size;
    for (uint16_t index = 0; index < size; ++index) {
        if (hasUncertainFinalEffect(*chain[index])) {
            minPrefix = index;
            break;
        }
    }

    return prefixDeltaOptions(chain, domain, minPrefix, size);
}

std::vector<Model::PrefixState> Model::prefixDeltaOptions(
        std::vector<PendingControlRequest const*> const& chain,
        ForecastDomain domain,
        uint16_t minPrefix,
        uint16_t maxPrefix) const
{
    std::vector<std::vector<PrefixState>> prefixOptions;
    std::vector<PrefixState> currentOptions { { 0, 0.0f, {} } };
    prefixOptions.push_back(currentOptions);

    uint16_t prefix = 0;
    for (auto const* request : chain) {
        ++prefix;
        std::vector<PrefixState> nextOptions;
        for (auto const& current : currentOptions) {
            for (auto const scale : effectScales(*request)) {
                auto next = current;
                next.prefix = prefix;
                next.delta += meterDelta(*request, domain, scale);
                next.scales.push_back(scale);
                nextOptions.push_back(next);
            }
        }
        currentOptions = nextOptions;
        prefixOptions.push_back(currentOptions);
    }

    std::vector<PrefixState> result;
    for (uint16_t index = minPrefix; index <= maxPrefix && index < prefixOptions.size(); ++index) {
        result.insert(result.end(), prefixOptions[index].begin(), prefixOptions[index].end());
    }
    return result;
}

bool Model::finalStateIsConsistentWithVisibleState(
        PrefixState const& visible,
        PrefixState const& final) const
{
    if (final.prefix < visible.prefix) {
        return false;
    }

    for (size_t index = 0; index < visible.scales.size(); ++index) {
        if (index >= final.scales.size()) {
            return false;
        }
        if (final.scales[index] + 0.001f < visible.scales[index]) {
            return false;
        }
    }

    return true;
}

float Model::meterDelta(
        PendingControlRequest const& request,
        ForecastDomain /*domain*/,
        float scale) const
{
    return request.meterDeltaWatts() * scale;
}

bool Model::requestIsRelevantToDomain(
        PendingControlRequest const& request,
        ForecastDomain /*domain*/) const
{
    return !floatEqual(request.effectiveDeltaWatts, 0.0f);
}

std::vector<float> Model::effectScales(PendingControlRequest const& request) const
{
    if (request.effectKind != EffectKind::SolarCapacityLimited) {
        return { 1.0f };
    }

    if (request.effectiveDeltaWatts <= 0.0f) {
        return { 1.0f };
    }

    return request.capacityFullOutputPossible
        ? std::vector<float> { 0.0f, 1.0f }
        : std::vector<float> { 0.0f };
}

bool Model::couldBeVisible(
        PendingControlRequest const& request,
        uint32_t meterSampleMillis) const
{
    if (!request.sentMillis) {
        return false;
    }
    if (meterSampleMillis < *request.sentMillis) {
        return false;
    }
    if (request.notBeforeEffectMillis && meterSampleMillis < *request.notBeforeEffectMillis) {
        return false;
    }
    return true;
}

bool Model::mustBeVisible(
        PendingControlRequest const& request,
        uint32_t meterSampleMillis) const
{
    if (!request.latestEffectMillis) {
        return false;
    }
    if (meterSampleMillis < *request.latestEffectMillis) {
        return false;
    }
    if (request.state == RequestState::FailedAmbiguous) {
        return false;
    }
    if (request.effectKind == EffectKind::SolarCapacityLimited
            || request.effectKind == EffectKind::Ambiguous) {
        return false;
    }
    return true;
}

void Model::recomputeRequestedSetpoint(std::string const& actuatorKey)
{
    auto* actuator = findActuator(actuatorKey);
    if (actuator == nullptr) {
        return;
    }

    auto requested = actuator->safeSetpointWatts;
    std::vector<PendingControlRequest const*> ordered;
    for (auto const& request : _ledger) {
        if (request.actuatorKey == actuatorKey) {
            ordered.push_back(&request);
        }
    }
    std::sort(ordered.begin(), ordered.end(),
            [](auto const* lhs, auto const* rhs) { return lhs->seq < rhs->seq; });

    for (auto const* request : ordered) {
        if (isPendingRequestedState(request->state)) {
            requested = request->targetSetpointWatts;
        }
    }

    actuator->requestedSetpointWatts = requested;
}

PendingControlRequest* Model::requestBySeq(uint32_t seq)
{
    auto const iter = std::find_if(_ledger.begin(), _ledger.end(),
            [seq](auto const& request) { return request.seq == seq; });
    return iter == _ledger.end() ? nullptr : &(*iter);
}

CorrectionDecision decideCorrection(
        float targetWatts,
        ControlForecast const& forecast,
        float hysteresisWatts,
        float smallCorrectionDampingThresholdWatts)
{
    if (targetWatts >= forecast.gridMinWatts - hysteresisWatts
            && targetWatts <= forecast.gridMaxWatts + hysteresisWatts) {
        return {
            CorrectionAction::Hold,
            0.0f,
            "inside_corridor",
            targetWatts,
        };
    }

    if (targetWatts < forecast.gridMinWatts - hysteresisWatts) {
        return dampSmallCorrection({
            CorrectionAction::MoreEffectiveOutput,
            forecast.gridMinWatts - targetWatts,
            "target_below_corridor",
            forecast.gridMinWatts,
        }, smallCorrectionDampingThresholdWatts);
    }

    return dampSmallCorrection({
        CorrectionAction::LessEffectiveOutput,
        forecast.gridMaxWatts - targetWatts,
        "target_above_corridor",
        forecast.gridMaxWatts,
    }, smallCorrectionDampingThresholdWatts);
}

float corridorUncertaintyWatts(ControlForecast const& forecast)
{
    if (!forecast.valid) {
        return 0.0f;
    }

    return std::max(0.0f, forecast.gridMaxWatts - forecast.gridMinWatts);
}

float distanceOutsideCorridor(
        float valueWatts,
        ControlForecast const& forecast)
{
    if (!forecast.valid) {
        return 0.0f;
    }

    if (valueWatts < forecast.gridMinWatts) {
        return forecast.gridMinWatts - valueWatts;
    }

    if (valueWatts > forecast.gridMaxWatts) {
        return valueWatts - forecast.gridMaxWatts;
    }

    return 0.0f;
}

float targetChangeDistanceWatts(
        float storageTargetWatts,
        float globalTargetWatts,
        float referenceStorageTargetWatts,
        float referenceGlobalTargetWatts)
{
    return std::max(
            std::fabs(storageTargetWatts - referenceStorageTargetWatts),
            std::fabs(globalTargetWatts - referenceGlobalTargetWatts));
}

float targetBandErrorWatts(
        float storageTargetWatts,
        float globalTargetWatts,
        float gridWatts)
{
    auto const validGlobalTargetWatts =
        validateTargetOrdering(storageTargetWatts, globalTargetWatts, true/*clamp*/);
    return std::max(
            std::max(0.0f, gridWatts - storageTargetWatts),
            std::max(0.0f, validGlobalTargetWatts - gridWatts));
}

float targetBandErrorWatts(
        float storageTargetWatts,
        float globalTargetWatts,
        ControlForecast const& forecast)
{
    if (!forecast.valid) {
        return 0.0f;
    }

    auto const validGlobalTargetWatts =
        validateTargetOrdering(storageTargetWatts, globalTargetWatts, true/*clamp*/);
    return std::max(
            std::max(0.0f, forecast.gridMinWatts - storageTargetWatts),
            std::max(0.0f, validGlobalTargetWatts - forecast.gridMaxWatts));
}

float targetBandErrorWatts(
        float storageTargetWatts,
        float globalTargetWatts,
        float gridWatts,
        ControlForecast const& forecast)
{
    if (!forecast.valid) {
        return 0.0f;
    }

    if (corridorUncertaintyWatts(forecast) <= 0.001f) {
        return targetBandErrorWatts(storageTargetWatts, globalTargetWatts, gridWatts);
    }

    return targetBandErrorWatts(storageTargetWatts, globalTargetWatts, forecast);
}

float outsideCorridorUncertaintyRatio(
        float distanceOutsideWatts,
        float uncertaintyWatts)
{
    if (distanceOutsideWatts <= 0.0f) {
        return 0.0f;
    }

    if (uncertaintyWatts <= 0.001f) {
        return std::numeric_limits<float>::infinity();
    }

    return distanceOutsideWatts / uncertaintyWatts;
}

CorrectionDecision decideSolarTargetCorrection(
        float targetWatts,
        ControlForecast const& forecast,
        float hysteresisWatts,
        float smallCorrectionDampingThresholdWatts)
{
    if (!forecast.valid) {
        return decideCorrection(
                targetWatts,
                forecast,
                hysteresisWatts,
                smallCorrectionDampingThresholdWatts);
    }

    auto const corridorDecision =
        decideCorrection(
                targetWatts,
                forecast,
                hysteresisWatts,
                smallCorrectionDampingThresholdWatts);
    if (corridorDecision.action == CorrectionAction::LessEffectiveOutput) {
        return corridorDecision;
    }
    if (corridorDecision.action == CorrectionAction::Hold
            && forecast.pendingCount > 0) {
        return corridorDecision;
    }

    auto const noFurtherEffectWatts = forecast.virtualMeterWatts;
    if (targetWatts < noFurtherEffectWatts - hysteresisWatts) {
        return dampSmallCorrection({
            CorrectionAction::MoreEffectiveOutput,
            noFurtherEffectWatts - targetWatts,
            "target_below_solar_probe_edge",
            noFurtherEffectWatts,
        }, smallCorrectionDampingThresholdWatts);
    }

    return corridorDecision;
}

ControlForecast forecastWithPlannedMeterDelta(
        ControlForecast forecast,
        float plannedMeterDeltaWatts)
{
    if (plannedMeterDeltaWatts == 0.0f) {
        return forecast;
    }

    forecast.virtualMeterWatts += plannedMeterDeltaWatts;
    forecast.gridMinWatts += plannedMeterDeltaWatts;
    forecast.gridNominalWatts += plannedMeterDeltaWatts;
    forecast.gridMaxWatts += plannedMeterDeltaWatts;
    return forecast;
}

bool dynamicTargetLearningAllowed(ControlForecast const& storageForecast)
{
    (void)storageForecast;
    return true;
}

bool gridChargerAvailablePowerLimitedForFlexibleLoad(
        CorrectionAction storageAction,
        uint16_t gridChargerExpectedInputWatts,
        uint16_t gridChargerMaxInputWatts)
{
    if (gridChargerMaxInputWatts <= gridChargerExpectedInputWatts) {
        return false;
    }
    if ((gridChargerMaxInputWatts - gridChargerExpectedInputWatts)
            < GridChargerAvailablePowerLimitMinimumHeadroomWatts) {
        return false;
    }

    // Charger headroom alone can come from SOC planning or BMS current limits.
    // Treat it as an available-power limit only while storage wants more sink.
    return storageAction == CorrectionAction::LessEffectiveOutput;
}

std::optional<float> deterministicExpectedMeterValue(
        ControlForecast const& forecast,
        float toleranceWatts)
{
    if (!forecast.valid) {
        return std::nullopt;
    }
    if (std::abs(forecast.gridMaxWatts - forecast.gridMinWatts) > toleranceWatts) {
        return std::nullopt;
    }
    return forecast.gridNominalWatts;
}

float globalTargetFromExportOffset(float storageTargetWatts, float exportOffsetWatts)
{
    if (exportOffsetWatts < 0.0f) {
        throw std::invalid_argument("global export offset must be non-negative");
    }
    return storageTargetWatts - exportOffsetWatts;
}

float validateTargetOrdering(float storageTargetWatts, float globalTargetWatts, bool clamp)
{
    if (globalTargetWatts <= storageTargetWatts) {
        return globalTargetWatts;
    }
    if (clamp) {
        return storageTargetWatts;
    }
    throw std::invalid_argument("global target must be less than or equal to storage target");
}

} // namespace PowerLimiterPredictiveControl
