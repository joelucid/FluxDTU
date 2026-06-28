// SPDX-License-Identifier: GPL-2.0-or-later

#include "PowerLimiterPredictiveControl.h"
#include "gridcharger/PowerLimiterTargetTiming.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace PowerLimiterPredictiveControl;

namespace {

constexpr float Eps = 0.001f;

void fail(std::string const& message)
{
    throw std::runtime_error(message);
}

void expectNear(float actual, float expected, std::string const& label)
{
    if (std::fabs(actual - expected) > Eps) {
        std::ostringstream out;
        out << label << ": expected " << expected << ", got " << actual;
        fail(out.str());
    }
}

void expectTrue(bool condition, std::string const& label)
{
    if (!condition) {
        fail(label);
    }
}

void expectAction(CorrectionAction actual, CorrectionAction expected, std::string const& label)
{
    if (actual != expected) {
        fail(label);
    }
}

ActuatorState makeActuator(
        std::string const& key,
        ActuatorKind kind,
        float safeSetpointWatts,
        ActuatorCapabilities capabilities = {})
{
    ActuatorState actuator;
    actuator.actuatorKey = key;
    actuator.kind = kind;
    actuator.safeSetpointWatts = safeSetpointWatts;
    actuator.requestedSetpointWatts = safeSetpointWatts;
    actuator.capabilities = capabilities;
    actuator.normalize();
    return actuator;
}

PendingControlRequest makeRequest(
        std::string const& actuatorKey,
        RequestDomain domain,
        float baseSetpointWatts,
        float targetSetpointWatts,
        float effectiveDeltaWatts)
{
    PendingControlRequest request;
    request.actuatorKey = actuatorKey;
    request.domain = domain;
    request.baseSetpointWatts = baseSetpointWatts;
    request.targetSetpointWatts = targetSetpointWatts;
    request.effectiveDeltaWatts = effectiveDeltaWatts;
    request.state = RequestState::Accepted;
    request.sentMillis = 1000;
    request.latestEffectMillis = 4000;
    return request;
}

ControlForecast modelFreeForecast(float gridMinWatts, float gridMaxWatts)
{
    ControlForecast forecast;
    forecast.valid = true;
    forecast.domain = ForecastDomain::Storage;
    forecast.virtualMeterWatts = (gridMinWatts + gridMaxWatts) / 2.0f;
    forecast.gridMinWatts = gridMinWatts;
    forecast.gridNominalWatts = forecast.virtualMeterWatts;
    forecast.gridMaxWatts = gridMaxWatts;
    return forecast;
}

void testStorageForecastUsesPhysicalGrid()
{
    Model model;
    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = -300.0f;
    snapshot.meterSampleMillis = 10000;

    auto const storageForecast = model.forecast(snapshot, ForecastDomain::Storage);
    auto const globalForecast = model.forecast(snapshot, ForecastDomain::Global);

    expectNear(storageForecast.virtualMeterWatts, -300.0f, "storage virtual meter");
    expectNear(globalForecast.virtualMeterWatts, -300.0f, "global virtual meter");
    expectAction(decideCorrection(0.0f, storageForecast).action,
            CorrectionAction::LessEffectiveOutput,
            "storage target must use physical grid");
    expectAction(decideCorrection(-300.0f, globalForecast).action, CorrectionAction::Hold,
            "global target must hold");
}

void testActuatorRequestedSetpointDefaultsToSafeSetpoint()
{
    ActuatorState battery;
    battery.actuatorKey = "bat";
    battery.kind = ActuatorKind::Battery;
    battery.safeSetpointWatts = 700.0f;

    Model model({ battery });
    auto const* stored = model.findActuator("bat");

    expectTrue(stored != nullptr, "stored battery exists");
    expectNear(stored->requestedSetpointWatts, 700.0f, "requested setpoint default");
}

void testBatteryPendingCommandVisibleEarlyIsNotDoubleCounted()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    model.appendPhysicalRequest(makeRequest("bat", RequestDomain::Storage, 0.0f, 500.0f, 500.0f));

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 0.0f;
    snapshot.meterSampleMillis = 1500;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    expectNear(forecast.gridMinWatts, -500.0f, "early visible battery min");
    expectNear(forecast.gridMaxWatts, 0.0f, "early visible battery max");
    expectAction(decideCorrection(0.0f, forecast).action, CorrectionAction::Hold,
            "early visible pending request must not be sent again");
}

void testBatteryPendingCommandNotYetVisibleKeepsCorridorOpen()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    model.appendPhysicalRequest(makeRequest("bat", RequestDomain::Storage, 0.0f, 500.0f, 500.0f));

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 500.0f;
    snapshot.meterSampleMillis = 1500;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    expectNear(forecast.gridMinWatts, 0.0f, "not yet visible battery min");
    expectNear(forecast.gridMaxWatts, 500.0f, "not yet visible battery max");
    expectAction(decideCorrection(0.0f, forecast).action, CorrectionAction::Hold,
            "pending corridor contains target");
}

void testCurrentEstimateTracksVisiblePendingDeltasSinceBaseSample()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    model.appendPhysicalRequest(makeRequest("bat", RequestDomain::Storage, 0.0f, 500.0f, 500.0f));

    MeterSnapshot baseSnapshot;
    baseSnapshot.physicalGridWatts = 500.0f;
    baseSnapshot.meterSampleMillis = 900;

    auto const beforeVisible = model.currentEstimate(baseSnapshot, ForecastDomain::Global, 900);
    expectNear(beforeVisible.gridMinWatts, 500.0f, "current estimate before visible min");
    expectNear(beforeVisible.gridMaxWatts, 500.0f, "current estimate before visible max");

    auto const possiblyVisible = model.currentEstimate(baseSnapshot, ForecastDomain::Global, 1500);
    expectNear(possiblyVisible.gridMinWatts, 0.0f, "current estimate possibly visible min");
    expectNear(possiblyVisible.gridMaxWatts, 500.0f, "current estimate possibly visible max");
    expectTrue(610.0f > possiblyVisible.gridMaxWatts + 100.0f,
            "external increase is outside current estimate plus hysteresis");

    auto const mustBeVisible = model.currentEstimate(baseSnapshot, ForecastDomain::Global, 4000);
    expectNear(mustBeVisible.gridMinWatts, 0.0f, "current estimate visible min");
    expectNear(mustBeVisible.gridMaxWatts, 0.0f, "current estimate visible max");
}

void testCurrentEstimateDoesNotDoubleCountBaseVisibleDelta()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    model.appendPhysicalRequest(makeRequest("bat", RequestDomain::Storage, 0.0f, 500.0f, 500.0f));

    MeterSnapshot baseSnapshot;
    baseSnapshot.physicalGridWatts = 0.0f;
    baseSnapshot.meterSampleMillis = 4000;

    auto const estimate = model.currentEstimate(baseSnapshot, ForecastDomain::Global, 4500);

    expectNear(estimate.gridMinWatts, 0.0f, "current estimate settled visible min");
    expectNear(estimate.gridMaxWatts, 0.0f, "current estimate settled visible max");
}

void testHardNotBeforeTimeBlocksVisibleBranch()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    auto request = makeRequest("bat", RequestDomain::Storage, 0.0f, 500.0f, 500.0f);
    request.notBeforeEffectMillis = 2000;
    model.appendPhysicalRequest(request);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 500.0f;
    snapshot.meterSampleMillis = 1500;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    expectNear(forecast.gridMinWatts, 0.0f, "not-before min");
    expectNear(forecast.gridMaxWatts, 0.0f, "not-before max");
}

void testSoftTimingDoesNotCloseConservativeCorridor()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    auto request = makeRequest("bat", RequestDomain::Storage, 0.0f, 500.0f, 500.0f);
    request.earliestExpectedMillis = 2000;
    request.typicalEffectMillis = 3000;
    model.appendPhysicalRequest(request);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 500.0f;
    snapshot.meterSampleMillis = 1500;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    expectNear(forecast.gridMinWatts, 0.0f, "soft timing min");
    expectNear(forecast.gridMaxWatts, 500.0f, "soft timing max");
}

void testOrderedChainDoesNotCreateImpossibleSum()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 700.0f) });
    model.appendPhysicalRequest(makeRequest("bat", RequestDomain::Storage, 700.0f, 300.0f, -400.0f));
    auto second = makeRequest("bat", RequestDomain::Storage, 300.0f, 500.0f, 200.0f);
    second.sentMillis = 1100;
    second.latestEffectMillis = 4100;
    model.appendPhysicalRequest(second);

    auto const visibleDeltas = model.possibleMeterDeltasForActuator(
            "bat", ForecastDomain::Storage, 1500);

    expectTrue(visibleDeltas.size() == 3, "ordered chain visible state count");
    expectNear(visibleDeltas[0], 0.0f, "ordered chain delta 0");
    expectNear(visibleDeltas[1], 200.0f, "ordered chain delta 1");
    expectNear(visibleDeltas[2], 400.0f, "ordered chain delta 2");
}

void testAmbiguousEarlierRequestKeepsLaterDeltaRelativeToCommandChain()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 700.0f) });
    auto first = makeRequest("bat", RequestDomain::Storage, 700.0f, 300.0f, -400.0f);
    first.state = RequestState::FailedAmbiguous;
    model.appendPhysicalRequest(first);
    auto second = makeRequest("bat", RequestDomain::Storage, 300.0f, 500.0f, 200.0f);
    second.sentMillis = 1100;
    second.latestEffectMillis = 4100;
    model.appendPhysicalRequest(second);

    auto const visibleDeltas = model.possibleMeterDeltasForActuator(
            "bat", ForecastDomain::Storage, 1500);

    expectTrue(visibleDeltas.size() == 3, "ambiguous chain visible state count");
    expectNear(visibleDeltas[0], 0.0f, "ambiguous chain delta 0");
    expectNear(visibleDeltas[1], 200.0f, "ambiguous chain delta 1");
    expectNear(visibleDeltas[2], 400.0f, "ambiguous chain delta 2");
}

void testLaterRequestWithEarlierLatestDoesNotSettleAlone()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 700.0f) });
    auto first = makeRequest("bat", RequestDomain::Storage, 700.0f, 300.0f, -400.0f);
    first.latestEffectMillis = 5000;
    model.appendPhysicalRequest(first);
    auto second = makeRequest("bat", RequestDomain::Storage, 300.0f, 500.0f, 200.0f);
    second.sentMillis = 1100;
    second.latestEffectMillis = 3000;
    model.appendPhysicalRequest(second);

    model.advanceLedger(3000);

    auto const* battery = model.findActuator("bat");
    expectTrue(battery != nullptr, "battery exists");
    expectNear(battery->safeSetpointWatts, 700.0f, "safe setpoint before first latest");
    expectTrue(model.ledger()[0].state == RequestState::Accepted, "first request remains accepted");
    expectTrue(model.ledger()[1].state == RequestState::Accepted, "second request remains accepted");
}

void testFailedQueuedRequestDoesNotSuppressRetry()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    auto const seq = model.queueRequest(
            "bat",
            RequestDomain::Storage,
            1000.0f,
            1000.0f).seq;

    auto const* before = model.findActuator("bat");
    expectTrue(before != nullptr, "battery exists before failure");
    expectNear(before->requestedSetpointWatts, 1000.0f, "queued request target");

    model.classifyFailure(seq, RequestState::FailedNoEffect);
    auto const* after = model.findActuator("bat");
    expectTrue(after != nullptr, "battery exists after failure");
    expectNear(after->requestedSetpointWatts, 0.0f, "failed queued request releases retry suppression");
}

void testQueuedRequestsAreNotSupersededBeforeSend()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    auto const firstSeq = model.queueRequest(
            "bat",
            RequestDomain::Storage,
            500.0f,
            500.0f).seq;
    auto const secondSeq = model.queueRequest(
            "bat",
            RequestDomain::Storage,
            800.0f,
            800.0f).seq;

    expectTrue(firstSeq != secondSeq, "queued requests use distinct sequences");
    expectTrue(model.ledger()[0].state == RequestState::Queued,
            "first queued request remains queued before send");
    expectTrue(model.ledger()[1].state == RequestState::Queued,
            "second queued request stays queued");

    auto const* battery = model.findActuator("bat");
    expectTrue(battery != nullptr, "battery exists after queued supersession");
    expectNear(battery->requestedSetpointWatts, 800.0f, "requested setpoint follows latest queued");
}

void testDispatchedQueuedRequestIsNotSupersededBeforeCompletion()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    auto const firstSeq = model.queueRequest(
            "bat",
            RequestDomain::Storage,
            500.0f,
            500.0f).seq;

    auto* dispatched = model.markLatestQueuedRequestDispatchPending(
            "bat",
            500.0f,
            TransitionKind::Setpoint);
    expectTrue(dispatched != nullptr, "queued request marked RF dispatched");
    expectTrue(dispatched->dispatchPending, "RF dispatched request is internally pending");

    auto const secondSeq = model.queueRequest(
            "bat",
            RequestDomain::Storage,
            800.0f,
            300.0f).seq;

    expectTrue(firstSeq != secondSeq, "queued requests use distinct sequences after dispatch");
    expectTrue(model.ledger()[0].state == RequestState::Queued,
            "RF dispatched request is not superseded before completion");
    expectTrue(model.ledger()[0].dispatchPending,
            "RF dispatched request remains internally pending");
    expectTrue(model.ledger()[1].state == RequestState::Queued,
            "new target waits as queued request");

    auto* sent = model.markLatestQueuedRequestSent(
            "bat",
            500.0f,
            TransitionKind::Setpoint,
            2000,
            4000);
    expectTrue(sent != nullptr, "RF completion marks dispatched request sent");
    expectTrue(sent->seq == firstSeq, "RF completion targets original dispatched request");
    expectTrue(sent->state == RequestState::Sent, "dispatched request becomes sent");
    expectTrue(!sent->dispatchPending, "sent request clears internal RF pending flag");
    expectTrue(model.ledger()[1].state == RequestState::Queued,
            "later target remains queued after original completion");
}

void testUnsentQueuedRequestCanBeDiscarded()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    model.queueRequest(
            "bat",
            RequestDomain::Storage,
            800.0f,
            800.0f);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 50.0f;
    snapshot.meterSampleMillis = 5000;
    auto const staleForecast = model.forecast(snapshot, ForecastDomain::Storage);
    expectTrue(staleForecast.pendingCount == 1, "unsent request is forecast pending");
    expectNear(staleForecast.gridNominalWatts, -750.0f, "unsent queued request affects future forecast");

    model.discardQueuedRequests("bat", 0.0f);
    model.pruneInactiveRequests();

    auto const* battery = model.findActuator("bat");
    expectTrue(battery != nullptr, "battery exists after queued discard");
    expectNear(battery->requestedSetpointWatts, 0.0f, "discard releases requested setpoint");
    expectNear(battery->safeSetpointWatts, 0.0f, "discard keeps safe setpoint at physical target");

    auto const cleanForecast = model.forecast(snapshot, ForecastDomain::Storage);
    expectTrue(cleanForecast.pendingCount == 0, "discarded request leaves forecast");
    expectNear(cleanForecast.gridNominalWatts, 50.0f, "forecast returns to physical meter");
}

void testRfDispatchedQueuedRequestCanBeDiscardedAfterInverterReset()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    model.queueRequest(
            "bat",
            RequestDomain::Storage,
            800.0f,
            800.0f);
    auto* dispatched = model.markLatestQueuedRequestDispatchPending(
            "bat",
            800.0f,
            TransitionKind::Setpoint);
    expectTrue(dispatched != nullptr, "queued request marked RF dispatched");
    expectTrue(model.activeRequestCount() == 1, "RF-dispatched queued request is active");

    model.discardQueuedRequests("bat", 0.0f);
    model.pruneInactiveRequests();

    auto const* battery = model.findActuator("bat");
    expectTrue(battery != nullptr, "battery exists after dispatched discard");
    expectNear(battery->requestedSetpointWatts, 0.0f, "dispatched discard releases requested setpoint");
    expectTrue(model.activeRequestCount() == 0, "discarded RF-dispatched request is not active");

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 50.0f;
    snapshot.meterSampleMillis = 5000;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);
    expectTrue(forecast.pendingCount == 0, "discarded RF-dispatched request leaves forecast");
    expectNear(forecast.gridNominalWatts, 50.0f, "forecast returns to physical meter after dispatched discard");
}

void testQueuedRequestEntersForecastBeforeRfDispatch()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    auto const seq = model.queueRequest(
            "bat",
            RequestDomain::Storage,
            500.0f,
            500.0f).seq;

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 500.0f;
    snapshot.meterSampleMillis = 1500;
    auto const queuedForecast = model.forecast(snapshot, ForecastDomain::Storage);
    expectTrue(queuedForecast.pendingCount == 1, "queued request participates in forecast");
    expectNear(queuedForecast.gridMinWatts, 0.0f, "queued forecast min");
    expectNear(queuedForecast.gridMaxWatts, 0.0f, "queued forecast max");

    auto const visibleBeforeSend = model.possibleMeterDeltasForActuator(
            "bat", ForecastDomain::Storage, 1500);
    expectTrue(visibleBeforeSend.size() == 1, "queued request has one visible state before send");
    expectNear(visibleBeforeSend[0], 0.0f, "queued request visible delta before send");

    auto* dispatched = model.markLatestQueuedRequestDispatchPending(
            "bat",
            500.0f,
            TransitionKind::Setpoint);
    expectTrue(dispatched != nullptr, "queued request marked RF dispatched");

    auto const dispatchPendingForecast = model.forecast(snapshot, ForecastDomain::Storage);
    expectTrue(dispatchPendingForecast.pendingCount == 1, "RF-dispatched request participates in forecast");
    expectNear(dispatchPendingForecast.gridMinWatts, 0.0f, "RF-dispatched forecast min");
    expectNear(dispatchPendingForecast.gridMaxWatts, 500.0f, "RF-dispatched forecast max");

    model.markQueuedRequestSent(
            seq,
            2000,
            RequestState::Sent,
            std::nullopt,
            2000,
            std::nullopt,
            std::nullopt,
            5000);

    auto const beforeDispatchSample = model.forecast(snapshot, ForecastDomain::Storage);
    expectTrue(beforeDispatchSample.pendingCount == 1, "sent request pending before dispatch sample");
    expectNear(beforeDispatchSample.gridMinWatts, 0.0f, "before-dispatch forecast min");
    expectNear(beforeDispatchSample.gridMaxWatts, 0.0f, "before-dispatch forecast max");

    snapshot.meterSampleMillis = 2500;
    auto const sentForecast = model.forecast(snapshot, ForecastDomain::Storage);
    expectTrue(sentForecast.pendingCount == 1, "sent queued request is pending");
    expectNear(sentForecast.gridMinWatts, 0.0f, "sent queued forecast min");
    expectNear(sentForecast.gridMaxWatts, 500.0f, "sent queued forecast max");
}

void testStaleQueuedRequestIsDiscarded()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    model.queueRequest(
            "bat",
            RequestDomain::Storage,
            500.0f,
            500.0f,
            RequestCause::Normal,
            EffectKind::Deterministic,
            TransitionKind::Setpoint,
            1000);

    expectTrue(model.activeRequestCount() == 1, "fresh queued request is active");
    expectTrue(model.discardStaleQueuedRequests(20 * 1000, 35 * 1000) == 0,
            "fresh queued request is retained");
    expectTrue(model.activeRequestCount() == 1, "retained queued request remains active");

    expectTrue(model.discardStaleQueuedRequests(37 * 1000, 35 * 1000) == 1,
            "stale queued request is discarded");
    expectTrue(model.ledger()[0].state == RequestState::SettledDiscarded,
            "stale queued request state");
    expectTrue(model.activeRequestCount() == 0, "discarded stale request is inactive");

    auto const* battery = model.findActuator("bat");
    expectTrue(battery != nullptr, "battery exists after stale discard");
    expectNear(battery->requestedSetpointWatts, 0.0f, "stale discard releases requested setpoint");
}

void testMarkLatestQueuedRequestSentUsesDispatchTimeForLatency()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    model.queueRequest(
            "bat",
            RequestDomain::Storage,
            500.0f,
            500.0f,
            RequestCause::Normal,
            EffectKind::Deterministic,
            TransitionKind::Setpoint,
            1000);

    auto* sent = model.markLatestQueuedRequestSent(
            "bat",
            500.0f,
            TransitionKind::Setpoint,
            2500,
            5500);
    expectTrue(sent != nullptr, "queued request marked sent");
    expectTrue(sent->state == RequestState::Sent, "request state is sent");
    expectTrue(sent->sentMillis && *sent->sentMillis == 2500, "sent millis is dispatch time");
    expectTrue(sent->latestEffectMillis && *sent->latestEffectMillis == 5500,
            "latest effect is dispatch plus latency");

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 500.0f;
    snapshot.meterSampleMillis = 2400;
    auto const beforeDispatch = model.forecast(snapshot, ForecastDomain::Storage);
    expectNear(beforeDispatch.gridMinWatts, 0.0f, "before dispatch min");
    expectNear(beforeDispatch.gridMaxWatts, 0.0f, "before dispatch max");

    snapshot.meterSampleMillis = 3000;
    auto const afterDispatch = model.forecast(snapshot, ForecastDomain::Storage);
    expectNear(afterDispatch.gridMinWatts, 0.0f, "after dispatch min");
    expectNear(afterDispatch.gridMaxWatts, 500.0f, "after dispatch max");
}

void testLatestEffectAdvancesSafeSetpointAndClosesPendingCorridor()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    model.appendPhysicalRequest(makeRequest("bat", RequestDomain::Storage, 0.0f, 500.0f, 500.0f));

    model.advanceLedger(4000);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 0.0f;
    snapshot.meterSampleMillis = 4000;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    auto const* battery = model.findActuator("bat");
    expectTrue(battery != nullptr, "battery exists");
    expectNear(battery->safeSetpointWatts, 500.0f, "safe setpoint after timeout");
    expectTrue(forecast.pendingCount == 0, "pending corridor closed");
    expectNear(forecast.gridMinWatts, 0.0f, "settled min");
    expectNear(forecast.gridMaxWatts, 0.0f, "settled max");
}

void testPruningDropsSettledRequestsAndKeepsActiveOnes()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    model.appendPhysicalRequest(makeRequest("bat", RequestDomain::Storage, 0.0f, 500.0f, 500.0f));
    auto second = makeRequest("bat", RequestDomain::Storage, 500.0f, 300.0f, -200.0f);
    second.sentMillis = 4500;
    second.latestEffectMillis = 8000;
    model.appendPhysicalRequest(second);

    model.advanceLedger(4000);
    model.pruneInactiveRequests();

    expectTrue(model.ledger().size() == 1, "settled request is pruned");
    expectTrue(model.activeRequestCount() == 1, "active request survives pruning");
    expectNear(model.ledger().front().targetSetpointWatts, 300.0f,
            "active request target remains");
}

void testAmbiguousSendFailureRemainsInCorridor()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    auto request = makeRequest("bat", RequestDomain::Storage, 0.0f, 500.0f, 500.0f);
    request.state = RequestState::FailedAmbiguous;
    model.appendPhysicalRequest(request);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 500.0f;
    snapshot.meterSampleMillis = 1500;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    expectTrue(forecast.pendingCount == 1, "ambiguous request remains pending");
    expectNear(forecast.gridMinWatts, 0.0f, "ambiguous min");
    expectNear(forecast.gridMaxWatts, 500.0f, "ambiguous max");
}

void testSolarCapacityLimitedIncreaseWithoutHeadroomDoesNotBlockStorage()
{
    Model model({ makeActuator("solar", ActuatorKind::Solar, 300.0f) });
    auto request = makeRequest("solar", RequestDomain::Storage, 300.0f, 800.0f, 500.0f);
    request.effectKind = EffectKind::SolarCapacityLimited;
    request.capacityFullOutputPossible = false;
    model.appendPhysicalRequest(request);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 500.0f;
    snapshot.meterSampleMillis = 1500;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);
    auto const decision = decideCorrection(0.0f, forecast);

    expectNear(forecast.gridMinWatts, 500.0f, "no-headroom solar min");
    expectNear(forecast.gridMaxWatts, 500.0f, "no-headroom solar max");
    expectAction(decision.action, CorrectionAction::MoreEffectiveOutput,
            "no-headroom solar cannot block storage correction");
    expectNear(decision.residualEffectiveWatts, 500.0f, "no-headroom residual");
}

void testDeterministicEarlierRequestIsNotOptionalBecauseLaterRequestIsUncertain()
{
    Model model({ makeActuator("solar", ActuatorKind::Solar, 0.0f) });
    auto first = makeRequest("solar", RequestDomain::Storage, 0.0f, 300.0f, 300.0f);
    first.notBeforeEffectMillis = 2000;
    model.appendPhysicalRequest(first);

    auto second = makeRequest("solar", RequestDomain::Storage, 300.0f, 800.0f, 500.0f);
    second.effectKind = EffectKind::SolarCapacityLimited;
    second.capacityFullOutputPossible = true;
    second.sentMillis = 1100;
    second.notBeforeEffectMillis = 2000;
    second.latestEffectMillis = 4100;
    model.appendPhysicalRequest(second);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 0.0f;
    snapshot.meterSampleMillis = 1500;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    expectNear(forecast.gridMinWatts, -800.0f, "deterministic-before-uncertain min");
    expectNear(forecast.gridMaxWatts, -300.0f, "deterministic-before-uncertain max");
}

void testSolarReductionIsDeterministicEvenWithCapacityLimitedKind()
{
    Model model({ makeActuator("solar", ActuatorKind::Solar, 800.0f) });
    auto request = makeRequest("solar", RequestDomain::Storage, 800.0f, 300.0f, -500.0f);
    request.effectKind = EffectKind::SolarCapacityLimited;
    model.appendPhysicalRequest(request);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = -500.0f;
    snapshot.meterSampleMillis = 1500;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    expectNear(forecast.gridMinWatts, -500.0f, "solar reduction min");
    expectNear(forecast.gridMaxWatts, 0.0f, "solar reduction max");
}

void testChargerStartupUsesLongerHardNotBeforeWindow()
{
    Model model({ makeActuator("charger", ActuatorKind::Charger, 0.0f) });
    auto request = makeRequest("charger", RequestDomain::Storage, 0.0f, 1200.0f, -1200.0f);
    request.transitionKind = TransitionKind::Startup;
    request.notBeforeEffectMillis = 8000;
    request.latestEffectMillis = 15000;
    auto const& stored = model.appendPhysicalRequest(request);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 0.0f;
    snapshot.meterSampleMillis = 5000;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    expectTrue(stored.transitionKind == TransitionKind::Startup, "charger startup transition kind");
    expectNear(forecast.gridMinWatts, 1200.0f, "charger startup min");
    expectNear(forecast.gridMaxWatts, 1200.0f, "charger startup max");
}

void testGridChargerStandbyTransitionsUseLongerEffectWindow()
{
    expectTrue(
            GridChargers::powerLimiterTargetEffectAssumptionMillis(0, 300)
                == GridChargers::PowerLimiterStartupTargetEffectAssumptionMillis,
            "grid charger startup uses standby transition window");
    expectTrue(
            GridChargers::powerLimiterTargetEffectAssumptionMillis(300, 0)
                == GridChargers::PowerLimiterStartupTargetEffectAssumptionMillis,
            "grid charger shutdown uses standby transition window");
    expectTrue(
            GridChargers::powerLimiterTargetEffectAssumptionMillis(300, 500)
                == GridChargers::PowerLimiterNormalTargetEffectAssumptionMillis,
            "grid charger setpoint changes use normal effect window");
}

void testSolarStartupFromStandbyKeepsCapacityUncertaintyUntilTimeout()
{
    Model model({ makeActuator("solar", ActuatorKind::Solar, 0.0f) });
    auto request = makeRequest("solar", RequestDomain::Storage, 0.0f, 500.0f, 500.0f);
    request.effectKind = EffectKind::SolarCapacityLimited;
    request.transitionKind = TransitionKind::Startup;
    request.notBeforeEffectMillis = 8000;
    request.latestEffectMillis = 15000;
    request.capacityFullOutputPossible = true;
    model.appendPhysicalRequest(request);

    MeterSnapshot earlySnapshot;
    earlySnapshot.physicalGridWatts = 500.0f;
    earlySnapshot.meterSampleMillis = 5000;
    auto const early = model.forecast(earlySnapshot, ForecastDomain::Storage);
    expectNear(early.gridMinWatts, 0.0f, "solar startup early min");
    expectNear(early.gridMaxWatts, 500.0f, "solar startup early max");
    expectAction(decideCorrection(0.0f, early).action, CorrectionAction::Hold,
            "solar startup may still cover target");

    model.advanceLedger(15000);

    MeterSnapshot expiredSnapshot;
    expiredSnapshot.physicalGridWatts = 500.0f;
    expiredSnapshot.meterSampleMillis = 15000;
    auto const expired = model.forecast(expiredSnapshot, ForecastDomain::Storage);
    auto const decision = decideCorrection(0.0f, expired);
    expectTrue(expired.pendingCount == 0, "solar startup pending released at timeout");
    expectAction(decision.action, CorrectionAction::MoreEffectiveOutput,
            "after solar startup timeout storage may correct residual");
}

void testGlobalSolarContributesToStorageForecast()
{
    Model model({ makeActuator("solar", ActuatorKind::Solar, 300.0f) });
    auto request = makeRequest("solar", RequestDomain::Global, 300.0f, 600.0f, 300.0f);
    request.effectKind = EffectKind::SolarCapacityLimited;
    model.appendPhysicalRequest(request);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = -300.0f;
    snapshot.meterSampleMillis = 1500;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    expectNear(forecast.gridMinWatts, -600.0f, "global-domain visible min");
    expectNear(forecast.gridMaxWatts, -300.0f, "global-domain visible max");
    expectAction(decideCorrection(0.0f, forecast).action,
            CorrectionAction::LessEffectiveOutput,
            "pending global-domain solar contributes to storage forecast");
}

void testPendingGlobalSolarIncreaseIsDetectable()
{
    Model model({
        makeActuator("solar", ActuatorKind::Solar, 166.0f),
        makeActuator("bat", ActuatorKind::Battery, 0.0f),
    });
    auto request = makeRequest("solar", RequestDomain::Global, 166.0f, 1497.0f, 1331.0f);
    request.effectKind = EffectKind::SolarCapacityLimited;
    request.sentMillis = 1000;
    request.latestEffectMillis = 4000;
    model.appendPhysicalRequest(request);

    expectTrue(model.hasPendingGlobalIncrease(ActuatorKind::Solar),
            "pending global-domain solar increase is detected");
    expectTrue(!model.hasPendingGlobalIncrease(ActuatorKind::Battery),
            "pending global-domain solar increase is not reported for battery");

    model.advanceLedger(4000);

    expectTrue(!model.hasPendingGlobalIncrease(ActuatorKind::Solar),
            "settled global-domain solar increase is no longer pending");
}

void testSolarCapacityLimitedTimeoutDoesNotAdvanceSafeSetpoint()
{
    Model model({ makeActuator("solar", ActuatorKind::Solar, 300.0f) });
    auto request = makeRequest("solar", RequestDomain::Global, 300.0f, 600.0f, 300.0f);
    request.effectKind = EffectKind::SolarCapacityLimited;
    request.capacityFullOutputPossible = true;
    model.appendPhysicalRequest(request);

    model.advanceLedger(4000);

    auto const* solar = model.findActuator("solar");
    expectTrue(solar != nullptr, "solar actuator exists");
    expectNear(solar->safeSetpointWatts, 300.0f,
            "solar capacity timeout keeps safe setpoint at measured baseline");
    expectNear(solar->requestedSetpointWatts, 300.0f,
            "solar capacity timeout releases requested setpoint");
    expectTrue(model.activeRequestCount() == 0,
            "solar capacity timeout releases active request");
}

void testChargerIncreaseRaisesMeterAndReducesEffectiveOutput()
{
    Model model({ makeActuator("charger", ActuatorKind::Charger, 300.0f) });
    auto const& request = model.appendPhysicalRequest(
            makeRequest("charger", RequestDomain::Storage, 300.0f, 500.0f, -200.0f));

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 0.0f;
    snapshot.meterSampleMillis = 1500;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    expectNear(request.setpointDeltaWatts(), 200.0f, "charger setpoint delta");
    expectNear(request.meterDeltaWatts(), 200.0f, "charger meter delta");
    expectNear(forecast.gridMinWatts, 0.0f, "charger increase min");
    expectNear(forecast.gridMaxWatts, 200.0f, "charger increase max");
}

void testQueuedChargerAbsorbsStorageResidualBeforeSolarBackoff()
{
    Model model({
        makeActuator("charger", ActuatorKind::Charger, 58.0f),
        makeActuator("solar", ActuatorKind::Solar, 562.0f),
    });

    model.queueRequest(
            "charger",
            RequestDomain::Storage,
            390.0f,
            -332.0f);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = -350.0f;
    snapshot.meterSampleMillis = 1500;

    auto const storageForecast = model.forecast(snapshot, ForecastDomain::Storage);
    expectNear(storageForecast.gridMinWatts, -18.0f, "charger covers storage residual min");
    expectNear(storageForecast.gridMaxWatts, -18.0f, "charger covers storage residual max");
    expectAction(decideCorrection(-18.0f, storageForecast).action, CorrectionAction::Hold,
            "storage holds after pending charger increase");

    auto const globalForecast = model.forecast(snapshot, ForecastDomain::Global);
    expectAction(decideSolarTargetCorrection(-218.0f, globalForecast).action,
            CorrectionAction::MoreEffectiveOutput,
            "global solar correction must not back off after storage charger action");
}

void testQueuedSolarReductionPreventsDuplicateSolarBackoff()
{
    Model model({ makeActuator("solar", ActuatorKind::Solar, 562.0f) });
    model.queueRequest(
            "solar",
            RequestDomain::Global,
            430.0f,
            -132.0f);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = -350.0f;
    snapshot.meterSampleMillis = 1500;

    auto const globalForecast = model.forecast(snapshot, ForecastDomain::Global);
    expectNear(globalForecast.gridMinWatts, -218.0f, "queued solar reduction min");
    expectNear(globalForecast.gridMaxWatts, -218.0f, "queued solar reduction max");
    expectAction(decideSolarTargetCorrection(-218.0f, globalForecast).action,
            CorrectionAction::Hold,
            "queued solar reduction prevents duplicate backoff");
}

void testPendingChargerReductionPreventsSolarProbeIncrease()
{
    Model model({
        makeActuator("charger", ActuatorKind::Charger, 348.0f),
        makeActuator("solar", ActuatorKind::Solar, 1239.0f),
    });

    model.queueRequest(
            "charger",
            RequestDomain::Storage,
            164.0f,
            184.0f);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = -282.0f;
    snapshot.meterSampleMillis = 1500;

    auto const globalForecast = model.forecast(snapshot, ForecastDomain::Global);
    expectNear(globalForecast.virtualMeterWatts, -282.0f, "charger reduction probe edge");
    expectNear(globalForecast.gridMinWatts, -466.0f, "charger reduction forecast min");
    expectNear(globalForecast.gridMaxWatts, -466.0f, "charger reduction forecast max");

    auto const decision = decideSolarTargetCorrection(-365.0f, globalForecast);
    expectAction(decision.action,
            CorrectionAction::LessEffectiveOutput,
            "pending charger reduction prevents opposite solar probe increase");
    expectNear(decision.residualEffectiveWatts,
            -50.5f,
            "pending charger reduction solar backoff residual");
}

void testSameStepStorageBackoffMustNotReplaceSolarTargetDecision()
{
    Model model({
        makeActuator("bat", ActuatorKind::Battery, 1800.0f),
        makeActuator("solar", ActuatorKind::Solar, 3339.0f),
    });

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = -2089.0f;
    snapshot.meterSampleMillis = 1500;

    auto const sampleForecast = model.forecast(snapshot, ForecastDomain::Global);
    auto const sampleDecision = decideSolarTargetCorrection(-532.0f, sampleForecast);
    expectAction(sampleDecision.action,
            CorrectionAction::LessEffectiveOutput,
            "sample solar target needs backoff");
    expectNear(sampleDecision.residualEffectiveWatts,
            -1557.0f,
            "sample solar target residual");

    model.queueRequest(
            "bat",
            RequestDomain::Storage,
            43.0f,
            -1757.0f);

    auto const afterStorageForecast = model.forecast(snapshot, ForecastDomain::Global);
    auto const afterStorageDecision = decideSolarTargetCorrection(-532.0f, afterStorageForecast);
    expectAction(afterStorageDecision.action,
            CorrectionAction::MoreEffectiveOutput,
            "same-step battery backoff would hide solar backoff");
    expectNear(afterStorageDecision.residualEffectiveWatts,
            200.0f,
            "same-step battery backoff replacement residual");

    auto const remainingForecast = forecastWithPlannedMeterDelta(
            sampleForecast,
            1757.0f);
    auto const remaining = decideSolarTargetCorrection(-532.0f, remainingForecast);
    expectAction(remaining.action,
            CorrectionAction::MoreEffectiveOutput,
            "same-step storage backoff can over-cover solar target residual");
    expectNear(remaining.residualEffectiveWatts,
            200.0f,
            "same-step storage backoff over-cover residual");
}

void testSameStepStoragePlanIsSubtractedFromSolarTargetCorrection()
{
    ControlForecast forecast;
    forecast.valid = true;
    forecast.domain = ForecastDomain::Global;
    forecast.virtualMeterWatts = -3182.0f;
    forecast.gridMinWatts = -3182.0f;
    forecast.gridNominalWatts = -3182.0f;
    forecast.gridMaxWatts = -3182.0f;

    auto const shiftedForecast = forecastWithPlannedMeterDelta(
            forecast,
            1518.0f);
    auto const remaining = decideSolarTargetCorrection(-548.0f, shiftedForecast);
    expectAction(remaining.action,
            CorrectionAction::LessEffectiveOutput,
            "storage and charger plan leaves smaller solar backoff");
    expectNear(remaining.residualEffectiveWatts,
            -1116.0f,
            "storage and charger plan residual");
    expectNear(remaining.nearestCorridorEdgeWatts,
            -1664.0f,
            "storage and charger plan shifts edge");
}

void testSameStepStoragePlanCanRequireOppositeSolarTargetCorrection()
{
    ControlForecast forecast;
    forecast.valid = true;
    forecast.domain = ForecastDomain::Global;
    forecast.virtualMeterWatts = -548.0f;
    forecast.gridMinWatts = -548.0f;
    forecast.gridNominalWatts = -548.0f;
    forecast.gridMaxWatts = -548.0f;

    auto const afterMeterRise = decideSolarTargetCorrection(
            -548.0f,
            forecastWithPlannedMeterDelta(forecast,
                    518.0f));
    expectAction(afterMeterRise.action,
            CorrectionAction::MoreEffectiveOutput,
            "planned storage meter rise needs solar compensation");
    expectNear(afterMeterRise.residualEffectiveWatts,
            518.0f,
            "planned storage meter rise residual");

    auto const afterMeterDrop = decideSolarTargetCorrection(
            -548.0f,
            forecastWithPlannedMeterDelta(forecast,
                    -300.0f));
    expectAction(afterMeterDrop.action,
            CorrectionAction::LessEffectiveOutput,
            "planned storage meter drop needs solar backoff");
    expectNear(afterMeterDrop.residualEffectiveWatts,
            -300.0f,
            "planned storage meter drop residual");
}

void testSameStepBatteryIncreaseReducesSolarProbeResidual()
{
    Model model({
        makeActuator("bat", ActuatorKind::Battery, 680.0f),
        makeActuator("solar", ActuatorKind::Solar, 2322.0f),
    });

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = -195.0f;
    snapshot.meterSampleMillis = 1500;

    auto const beforeStorageForecast = model.forecast(snapshot, ForecastDomain::Global);
    auto const unadjusted = decideSolarTargetCorrection(-609.0f, beforeStorageForecast);
    expectAction(unadjusted.action,
            CorrectionAction::MoreEffectiveOutput,
            "sample solar target needs more output");
    expectNear(unadjusted.residualEffectiveWatts,
            414.0f,
            "sample solar target residual before storage");

    model.queueRequest(
            "bat",
            RequestDomain::Storage,
            794.0f,
            114.0f);

    auto const afterStorageForecast = model.forecast(snapshot, ForecastDomain::Global);
    expectNear(afterStorageForecast.virtualMeterWatts,
            -195.0f,
            "ledger forecast keeps physical no-effect edge");
    expectNear(afterStorageForecast.gridMinWatts,
            -309.0f,
            "ledger forecast includes battery meter effect");

    auto const storageAdjustedForecast = forecastWithPlannedMeterDelta(
            beforeStorageForecast,
            -114.0f);
    auto const adjusted = decideSolarTargetCorrection(-609.0f, storageAdjustedForecast);
    expectAction(adjusted.action,
            CorrectionAction::MoreEffectiveOutput,
            "same-step battery output leaves only the export-offset residual");
    expectNear(adjusted.residualEffectiveWatts,
            300.0f,
            "same-step battery output reduces solar probe residual");
}

void testSameStepStoragePlanShiftsFullSolarTargetForecast()
{
    ControlForecast forecast;
    forecast.valid = true;
    forecast.domain = ForecastDomain::Global;
    forecast.virtualMeterWatts = -548.0f;
    forecast.gridMinWatts = -600.0f;
    forecast.gridNominalWatts = -550.0f;
    forecast.gridMaxWatts = -500.0f;

    auto const shifted = forecastWithPlannedMeterDelta(forecast, 110.0f);
    expectNear(shifted.virtualMeterWatts,
            -438.0f,
            "planned meter delta shifts virtual meter");
    expectNear(shifted.gridMinWatts,
            -490.0f,
            "planned meter delta shifts lower edge");
    expectNear(shifted.gridNominalWatts,
            -440.0f,
            "planned meter delta shifts nominal edge");
    expectNear(shifted.gridMaxWatts,
            -390.0f,
            "planned meter delta shifts upper edge");
}

void testQueuedBatteryStandbyAndGlobalSolarBackoffShareGlobalForecast()
{
    Model model({
        makeActuator("bat", ActuatorKind::Battery, 1000.0f),
        makeActuator("solar", ActuatorKind::Solar, 1600.0f),
    });

    model.queueRequest(
            "bat",
            RequestDomain::Storage,
            0.0f,
            -1000.0f);
    model.queueRequest(
            "solar",
            RequestDomain::Global,
            800.0f,
            -800.0f);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = -1800.0f;
    snapshot.meterSampleMillis = 1500;

    auto const storageForecast = model.forecast(snapshot, ForecastDomain::Storage);
    expectNear(storageForecast.gridMinWatts, 0.0f, "battery plus solar backoff storage min");
    expectNear(storageForecast.gridMaxWatts, 0.0f, "battery plus solar backoff storage max");

    auto const globalForecast = model.forecast(snapshot, ForecastDomain::Global);
    expectNear(globalForecast.gridMinWatts, 0.0f, "battery plus solar backoff global min");
    expectNear(globalForecast.gridMaxWatts, 0.0f, "battery plus solar backoff global max");
    expectAction(decideSolarTargetCorrection(0.0f, globalForecast).action,
            CorrectionAction::Hold,
            "queued battery standby and solar backoff reconcile global target");
}

void testMinimumPowerAndSelfConsumptionArePartOfEffectiveDelta()
{
    ActuatorCapabilities chargerCapabilities;
    chargerCapabilities.minActiveSetpointWatts = 300.0f;
    chargerCapabilities.maxSetpointWatts = 1200.0f;
    chargerCapabilities.selfConsumptionWatts = 50.0f;
    auto charger = makeActuator("charger", ActuatorKind::Charger, 0.0f, chargerCapabilities);

    auto const legalChargerTarget = charger.legalizeSetpoint(100.0f);
    auto const chargerEffectiveDelta =
        charger.effectiveOutputForSetpoint(legalChargerTarget)
        - charger.effectiveOutputForSetpoint(charger.safeSetpointWatts);
    expectNear(legalChargerTarget, 300.0f, "charger minimum setpoint");
    expectNear(chargerEffectiveDelta, -350.0f, "charger effective delta with self-consumption");

    ActuatorCapabilities batteryCapabilities;
    batteryCapabilities.minActiveSetpointWatts = 150.0f;
    auto battery = makeActuator("bat", ActuatorKind::Battery, 0.0f, batteryCapabilities);
    auto const legalBatteryTarget = battery.legalizeSetpoint(80.0f);
    auto const batteryEffectiveDelta =
        battery.effectiveOutputForSetpoint(legalBatteryTarget)
        - battery.effectiveOutputForSetpoint(battery.safeSetpointWatts);
    expectNear(legalBatteryTarget, 150.0f, "battery minimum setpoint");
    expectNear(batteryEffectiveDelta, 150.0f, "battery minimum effective delta");
}

void testParallelIndependentActuatorsAreCombinedBySummingEffects()
{
    Model model({
        makeActuator("bat", ActuatorKind::Battery, 0.0f),
        makeActuator("charger", ActuatorKind::Charger, 300.0f),
    });
    model.appendPhysicalRequest(makeRequest("bat", RequestDomain::Storage, 0.0f, 500.0f, 500.0f));
    model.appendPhysicalRequest(makeRequest("charger", RequestDomain::Storage, 300.0f, 0.0f, 300.0f));

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 800.0f;
    snapshot.meterSampleMillis = 1500;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    expectNear(forecast.gridMinWatts, 0.0f, "parallel actuators min");
    expectNear(forecast.gridMaxWatts, 800.0f, "parallel actuators max");
}

void testDynamicTargetLearningIsAllowedWhileStoragePending()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 0.0f) });
    model.appendPhysicalRequest(makeRequest("bat", RequestDomain::Storage, 0.0f, 500.0f, 500.0f));

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = 500.0f;
    snapshot.meterSampleMillis = 1500;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    expectTrue(dynamicTargetLearningAllowed(forecast), "dynamic target learning allowed");
}

void testGridChargerAvailablePowerLimitedRequiresStorageBackoff()
{
    expectTrue(!gridChargerAvailablePowerLimitedForFlexibleLoad(
            CorrectionAction::Hold,
            500,
            900),
            "charger headroom while storage is stable is not an available-power limit");

    expectTrue(!gridChargerAvailablePowerLimitedForFlexibleLoad(
            CorrectionAction::MoreEffectiveOutput,
            500,
            900),
            "charger headroom during storage output increase is not an available-power limit");

    expectTrue(gridChargerAvailablePowerLimitedForFlexibleLoad(
            CorrectionAction::LessEffectiveOutput,
            500,
            900),
            "charger headroom while storage needs less effective output is an available-power limit");

    expectTrue(!gridChargerAvailablePowerLimitedForFlexibleLoad(
            CorrectionAction::LessEffectiveOutput,
            895,
            900),
            "charger headroom below threshold is not an available-power limit");

    expectTrue(gridChargerAvailablePowerLimitedForFlexibleLoad(
            CorrectionAction::LessEffectiveOutput,
            890,
            900),
            "charger headroom at threshold is an available-power limit");

    expectTrue(!gridChargerAvailablePowerLimitedForFlexibleLoad(
            CorrectionAction::LessEffectiveOutput,
            900,
            900),
            "charger at its external cap is not available-power limited");
}

void testHysteresisExpandsNoCommandBand()
{
    auto const forecast = modelFreeForecast(100.0f, 200.0f);

    expectAction(decideCorrection(90.0f, forecast, 10.0f).action, CorrectionAction::Hold,
            "hysteresis lower edge");
    auto const decision = decideCorrection(89.0f, forecast, 10.0f);
    expectAction(decision.action, CorrectionAction::MoreEffectiveOutput,
            "hysteresis just outside lower edge");
    expectNear(decision.residualEffectiveWatts, 5.5f, "hysteresis residual");
}

void testSmallCorrectionDampingHalvesOnlyBelowThreshold()
{
    auto const forecast = modelFreeForecast(100.0f, 300.0f);

    auto const belowSmall = decideCorrection(81.0f, forecast);
    expectAction(belowSmall.action, CorrectionAction::MoreEffectiveOutput,
            "small target below corridor requests more output");
    expectNear(belowSmall.residualEffectiveWatts,
            9.5f,
            "small positive residual is damped");

    auto const aboveSmall = decideCorrection(319.0f, forecast);
    expectAction(aboveSmall.action, CorrectionAction::LessEffectiveOutput,
            "small target above corridor requests less output");
    expectNear(aboveSmall.residualEffectiveWatts,
            -9.5f,
            "small negative residual is damped");

    auto const belowThreshold = decideCorrection(-50.0f, forecast);
    expectNear(belowThreshold.residualEffectiveWatts,
            150.0f,
            "positive residual at threshold is unchanged");

    auto const aboveThreshold = decideCorrection(450.0f, forecast);
    expectNear(aboveThreshold.residualEffectiveWatts,
            -150.0f,
            "negative residual at threshold is unchanged");

    auto const configuredLegacyThreshold =
        decideCorrection(40.0f, forecast, 0.0f, 20.0f);
    expectNear(configuredLegacyThreshold.residualEffectiveWatts,
            60.0f,
            "configured smaller damping threshold is honored");
}

void testSmallSolarProbeCorrectionIsDamped()
{
    auto forecast = modelFreeForecast(-223.0f, -12.0f);
    forecast.virtualMeterWatts = -12.0f;

    expectAction(decideCorrection(-31.0f, forecast).action, CorrectionAction::Hold,
            "standard corridor decision holds");

    auto const decision = decideSolarTargetCorrection(-31.0f, forecast);
    expectAction(decision.action, CorrectionAction::MoreEffectiveOutput,
            "solar probe edge still requests more output");
    expectNear(decision.residualEffectiveWatts,
            9.5f,
            "small solar probe residual is damped");
}

void testCorrectionResidualUsesNearestCorridorEdge()
{
    auto const forecast = modelFreeForecast(100.0f, 300.0f);

    auto const below = decideCorrection(40.0f, forecast);
    expectAction(below.action, CorrectionAction::MoreEffectiveOutput,
            "target below corridor requests more output");
    expectNear(below.residualEffectiveWatts, 30.0f, "below residual uses lower edge");
    expectNear(below.nearestCorridorEdgeWatts, 100.0f, "below nearest edge");

    auto const above = decideCorrection(360.0f, forecast);
    expectAction(above.action, CorrectionAction::LessEffectiveOutput,
            "target above corridor requests less output");
    expectNear(above.residualEffectiveWatts, -30.0f, "above residual uses upper edge");
    expectNear(above.nearestCorridorEdgeWatts, 300.0f, "above nearest edge");
}

void testOutsideCorridorUncertaintyRatio()
{
    auto const forecast = modelFreeForecast(100.0f, 500.0f);

    auto const uncertainty = corridorUncertaintyWatts(forecast);
    auto const deviation = distanceOutsideCorridor(700.0f, forecast);
    expectNear(uncertainty, 400.0f, "corridor uncertainty");
    expectNear(deviation, 200.0f, "outside distance");
    expectNear(outsideCorridorUncertaintyRatio(deviation, uncertainty), 0.5f,
            "outside uncertainty ratio");
    expectNear(distanceOutsideCorridor(250.0f, forecast), 0.0f,
            "inside corridor distance");

    auto const collapsed = modelFreeForecast(100.0f, 100.0f);
    expectTrue(std::isinf(outsideCorridorUncertaintyRatio(
                    distanceOutsideCorridor(150.0f, collapsed),
                    corridorUncertaintyWatts(collapsed))),
            "collapsed corridor with deviation has infinite urgency");
}

void testTargetChangeDistanceUsesLargestTargetDrift()
{
    expectNear(targetChangeDistanceWatts(0.0f, -300.0f, 0.0f, -300.0f), 0.0f,
            "unchanged targets have no drift");
    expectNear(targetChangeDistanceWatts(-20.0f, -340.0f, 0.0f, -300.0f), 40.0f,
            "global target drift dominates");
    expectNear(targetChangeDistanceWatts(-70.0f, -310.0f, 0.0f, -300.0f), 70.0f,
            "storage target drift dominates");
}

void testTargetBandErrorUsesOptimisticForecastEdge()
{
    expectNear(targetBandErrorWatts(0.0f, -100.0f, 25.0f),
            25.0f,
            "measured value above storage target counts from storage edge");
    expectNear(targetBandErrorWatts(0.0f, -100.0f, -140.0f),
            40.0f,
            "measured value below global target counts from global edge");
    expectNear(targetBandErrorWatts(0.0f, -100.0f, modelFreeForecast(-80.0f, -20.0f)),
            0.0f,
            "inside target band has no error");
    expectNear(targetBandErrorWatts(0.0f, -100.0f, modelFreeForecast(-5.0f, 50.0f)),
            0.0f,
            "corridor overlapping storage target uses optimistic high edge");
    expectNear(targetBandErrorWatts(0.0f, -100.0f, modelFreeForecast(10.0f, 50.0f)),
            10.0f,
            "high target-band error uses lower forecast edge");
    expectNear(targetBandErrorWatts(0.0f, -100.0f, modelFreeForecast(-150.0f, -90.0f)),
            0.0f,
            "corridor overlapping global target uses optimistic low edge");
    expectNear(targetBandErrorWatts(0.0f, -100.0f, modelFreeForecast(-150.0f, -120.0f)),
            20.0f,
            "low target-band error uses upper forecast edge");
}

void testTargetBandErrorUsesMeasuredGridOnlyForCollapsedForecast()
{
    expectNear(targetBandErrorWatts(
                    0.0f,
                    -100.0f,
                    25.0f,
                    modelFreeForecast(-20.0f, -20.0f)),
            25.0f,
            "collapsed forecast uses measured grid above storage target");
    expectNear(targetBandErrorWatts(
                    0.0f,
                    -100.0f,
                    25.0f,
                    modelFreeForecast(-5.0f, 50.0f)),
            0.0f,
            "non-zero corridor keeps optimistic storage edge");
    expectNear(targetBandErrorWatts(
                    0.0f,
                    -100.0f,
                    -140.0f,
                    modelFreeForecast(-150.0f, -90.0f)),
            0.0f,
            "non-zero corridor keeps optimistic global edge");
}

void testTargetBandErrorMustUseCurrentForecastNotPlannerEstimate()
{
    Model model({ makeActuator("charger", ActuatorKind::Charger, 0.0f) });
    model.appendPhysicalRequest(
            makeRequest("charger", RequestDomain::Storage, 0.0f, 300.0f, -300.0f));

    MeterSnapshot baseSnapshot;
    baseSnapshot.physicalGridWatts = -100.0f;
    baseSnapshot.meterSampleMillis = 900;

    MeterSnapshot currentSnapshot;
    currentSnapshot.physicalGridWatts = 200.0f;
    currentSnapshot.meterSampleMillis = 2000;

    auto const plannerEstimate =
        model.currentEstimate(
                baseSnapshot,
                ForecastDomain::Global,
                currentSnapshot.meterSampleMillis);
    auto const currentForecast = model.forecast(currentSnapshot, ForecastDomain::Global);

    expectNear(
            targetBandErrorWatts(
                    0.0f,
                    -300.0f,
                    currentSnapshot.physicalGridWatts,
                    plannerEstimate),
            0.0f,
            "elapsed planner estimate can hide a high target-band error");
    expectNear(
            targetBandErrorWatts(
                    0.0f,
                    -300.0f,
                    currentSnapshot.physicalGridWatts,
                    currentForecast),
            200.0f,
            "current future forecast exposes the high target-band error");
}

void testSolarTargetCorrectionUsesNoEffectEdgeForIncreases()
{
    auto forecast = modelFreeForecast(-223.0f, -12.0f);
    forecast.virtualMeterWatts = -12.0f;

    expectAction(decideCorrection(-215.0f, forecast).action, CorrectionAction::Hold,
            "standard corridor decision holds");

    auto const decision = decideSolarTargetCorrection(-215.0f, forecast);
    expectAction(decision.action, CorrectionAction::MoreEffectiveOutput,
            "solar target increase uses no-effect edge");
    expectNear(decision.residualEffectiveWatts, 203.0f, "solar target residual");
}

void testSolarTargetCorrectionWaitsForPendingLedgerCorridor()
{
    ControlForecast forecast;
    forecast.valid = true;
    forecast.domain = ForecastDomain::Global;
    forecast.virtualMeterWatts = 233.0f;
    forecast.gridMinWatts = -1602.0f;
    forecast.gridNominalWatts = -634.0f;
    forecast.gridMaxWatts = 233.0f;
    forecast.pendingCount = 3;

    expectAction(decideCorrection(-723.0f, forecast).action,
            CorrectionAction::Hold,
            "standard corridor decision holds while pending storage may still land");

    auto const decision = decideSolarTargetCorrection(-723.0f, forecast);
    expectAction(decision.action,
            CorrectionAction::Hold,
            "solar target increase waits for pending ledger corridor");
}

void testSolarTargetCorrectionDoesNotProbeAgainstPendingReductionFullEffectEdge()
{
    ControlForecast forecast;
    forecast.valid = true;
    forecast.domain = ForecastDomain::Global;
    forecast.virtualMeterWatts = -1531.0f;
    forecast.gridMinWatts = -1531.0f;
    forecast.gridNominalWatts = -390.0f;
    forecast.gridMaxWatts = 751.0f;
    forecast.pendingCount = 4;

    expectAction(decideCorrection(-571.0f, forecast).action,
            CorrectionAction::Hold,
            "standard corridor decision holds while pending solar reduction may still land");

    auto const decision = decideSolarTargetCorrection(-571.0f, forecast);
    expectAction(decision.action,
            CorrectionAction::Hold,
            "solar target increase must not use pending reduction full-effect edge");
}

void testExpectedMeterValueRequiresCollapsedForecast()
{
    auto const uncertain = modelFreeForecast(-175.0f, -4.0f);
    expectTrue(!deterministicExpectedMeterValue(uncertain),
            "uncertain corridor has no single expected value");

    auto const deterministic = modelFreeForecast(-61.0f, -61.0f);
    auto const expected = deterministicExpectedMeterValue(deterministic);
    expectTrue(expected.has_value(), "collapsed corridor has expected value");
    expectNear(*expected, -61.0f, "collapsed expected value");
}

void testTargetOrderingRejectsOrClampsInvalidGlobalTarget()
{
    expectNear(globalTargetFromExportOffset(0.0f, 300.0f), -300.0f, "global export offset target");

    bool threw = false;
    try {
        validateTargetOrdering(0.0f, 100.0f);
    } catch (std::invalid_argument const&) {
        threw = true;
    }
    expectTrue(threw, "invalid target ordering must throw");
    expectNear(validateTargetOrdering(0.0f, 100.0f, true), 0.0f,
            "invalid target ordering clamp");
}

void testSafetyOverrideIsStillLedgerRequest()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 500.0f) });
    auto request = makeRequest("bat", RequestDomain::Storage, 500.0f, 0.0f, -500.0f);
    request.cause = RequestCause::Safety;
    request.latestEffectMillis = 6000;
    auto const& stored = model.appendPhysicalRequest(request);

    MeterSnapshot snapshot;
    snapshot.physicalGridWatts = -500.0f;
    snapshot.meterSampleMillis = 1500;
    auto const forecast = model.forecast(snapshot, ForecastDomain::Storage);

    expectTrue(stored.cause == RequestCause::Safety, "safety cause retained");
    expectTrue(forecast.pendingCount == 1, "safety request is pending physical effect");
}

void testPowerStateTransitionBlocksUntilLatestEffect()
{
    Model model({ makeActuator("bat", ActuatorKind::Battery, 500.0f) });
    auto standby = makeRequest("bat", RequestDomain::Storage, 500.0f, 0.0f, -500.0f);
    standby.transitionKind = TransitionKind::Standby;
    standby.state = RequestState::Queued;
    standby.sentMillis = std::nullopt;
    standby.latestEffectMillis = std::nullopt;
    model.appendPhysicalRequest(standby);

    expectTrue(model.hasActivePowerStateTransition("bat"),
            "queued standby transition blocks further commands");

    auto* sent = model.markLatestQueuedRequestSent(
            "bat",
            0.0f,
            TransitionKind::Standby,
            2000,
            5000);
    expectTrue(sent != nullptr, "standby transition is marked sent");
    expectTrue(model.hasActivePowerStateTransition("bat"),
            "sent standby transition still blocks further commands");

    model.advanceLedger(4999);
    expectTrue(model.hasActivePowerStateTransition("bat"),
            "standby blocks until latest effect time");

    model.advanceLedger(5000);
    expectTrue(!model.hasActivePowerStateTransition("bat"),
            "settled standby releases command barrier");

    auto setpoint = makeRequest("bat", RequestDomain::Storage, 0.0f, 300.0f, 300.0f);
    setpoint.transitionKind = TransitionKind::Setpoint;
    model.appendPhysicalRequest(setpoint);
    expectTrue(!model.hasActivePowerStateTransition("bat"),
            "ordinary setpoint request does not create a power-state barrier");
}

void testBatteryStandbyAssumptionAllowsGridChargerBeforeConfirmation()
{
    Model model({
        makeActuator("bat", ActuatorKind::Battery, 500.0f),
        makeActuator("charger", ActuatorKind::Charger, 0.0f),
    });
    auto batteryOff = makeRequest("bat", RequestDomain::Storage, 500.0f, 0.0f, -500.0f);
    batteryOff.transitionKind = TransitionKind::Standby;
    batteryOff.latestEffectMillis = 4000;
    model.appendPhysicalRequest(batteryOff);

    expectTrue(model.actuatorMayBePhysicallyActive("bat"),
            "default activity remains conservative while standby is pending");
    expectTrue(!model.actuatorMayBePhysicallyActive("bat", 0.5f, true),
            "battery standby assumption treats pending-off as inactive");
    expectTrue(!model.anyActuatorMayBePhysicallyActive(ActuatorKind::Battery, 0.5f, true),
            "battery kind does not block charger after standby dispatch");
    expectTrue(!model.storageOutputAndChargerMayOverlap(),
            "battery pending-off alone is not a charger overlap");

    auto batteryOn = makeRequest("bat", RequestDomain::Storage, 0.0f, 300.0f, 300.0f);
    batteryOn.transitionKind = TransitionKind::Startup;
    batteryOn.latestEffectMillis = 5000;
    model.appendPhysicalRequest(batteryOn);
    expectTrue(model.anyActuatorMayBePhysicallyActive(ActuatorKind::Battery, 0.5f, true),
            "later battery startup overrides the standby assumption");

    model.advanceLedger(4000);
    expectTrue(model.actuatorMayBePhysicallyActive("bat", 0.5f, true),
            "startup remains physically possible after pending-off settles");
}

void testStorageOutputAndChargerOverlapIsDetectable()
{
    Model model({
        makeActuator("bat", ActuatorKind::Battery, 500.0f),
        makeActuator("charger", ActuatorKind::Charger, 0.0f),
    });
    auto chargerOn = makeRequest("charger", RequestDomain::Storage, 0.0f, 300.0f, -300.0f);
    chargerOn.sentMillis = 1000;
    chargerOn.latestEffectMillis = 4000;
    model.appendPhysicalRequest(chargerOn);

    expectTrue(model.actuatorMayBePhysicallyActive("charger"),
            "charger pending-on is physically possible");
    expectTrue(model.storageOutputAndChargerMayOverlap(),
            "battery output and charger pending-on overlap is detectable");
}

} // namespace

int main()
{
    std::vector<std::pair<std::string, std::function<void()>>> tests = {
        { "storage forecast uses physical grid", testStorageForecastUsesPhysicalGrid },
        { "actuator requested setpoint defaults to safe setpoint", testActuatorRequestedSetpointDefaultsToSafeSetpoint },
        { "battery pending command visible early is not double counted", testBatteryPendingCommandVisibleEarlyIsNotDoubleCounted },
        { "battery pending command not yet visible keeps corridor open", testBatteryPendingCommandNotYetVisibleKeepsCorridorOpen },
        { "current estimate tracks visible pending deltas since base sample", testCurrentEstimateTracksVisiblePendingDeltasSinceBaseSample },
        { "current estimate does not double count base visible delta", testCurrentEstimateDoesNotDoubleCountBaseVisibleDelta },
        { "hard not-before time blocks visible branch", testHardNotBeforeTimeBlocksVisibleBranch },
        { "soft timing does not close conservative corridor", testSoftTimingDoesNotCloseConservativeCorridor },
        { "ordered chain does not create impossible sum", testOrderedChainDoesNotCreateImpossibleSum },
        { "ambiguous earlier request keeps later delta relative to command chain", testAmbiguousEarlierRequestKeepsLaterDeltaRelativeToCommandChain },
        { "later request with earlier latest does not settle alone", testLaterRequestWithEarlierLatestDoesNotSettleAlone },
        { "failed queued request does not suppress retry", testFailedQueuedRequestDoesNotSuppressRetry },
        { "queued requests are not superseded before send", testQueuedRequestsAreNotSupersededBeforeSend },
        { "RF-dispatched queued request is not superseded before completion", testDispatchedQueuedRequestIsNotSupersededBeforeCompletion },
        { "unsent queued request can be discarded", testUnsentQueuedRequestCanBeDiscarded },
        { "RF-dispatched queued request can be discarded after inverter reset", testRfDispatchedQueuedRequestCanBeDiscardedAfterInverterReset },
        { "queued request enters forecast before RF dispatch", testQueuedRequestEntersForecastBeforeRfDispatch },
        { "stale queued request is discarded", testStaleQueuedRequestIsDiscarded },
        { "mark latest queued request sent uses dispatch time for latency", testMarkLatestQueuedRequestSentUsesDispatchTimeForLatency },
        { "latest effect advances safe setpoint and closes pending corridor", testLatestEffectAdvancesSafeSetpointAndClosesPendingCorridor },
        { "pruning drops settled requests and keeps active ones", testPruningDropsSettledRequestsAndKeepsActiveOnes },
        { "ambiguous send failure remains in corridor", testAmbiguousSendFailureRemainsInCorridor },
        { "solar capacity-limited increase without headroom does not block storage", testSolarCapacityLimitedIncreaseWithoutHeadroomDoesNotBlockStorage },
        { "deterministic earlier request is not optional because later request is uncertain", testDeterministicEarlierRequestIsNotOptionalBecauseLaterRequestIsUncertain },
        { "solar reduction is deterministic even with capacity-limited kind", testSolarReductionIsDeterministicEvenWithCapacityLimitedKind },
        { "charger startup uses longer hard not-before window", testChargerStartupUsesLongerHardNotBeforeWindow },
        { "grid charger standby transitions use longer effect window", testGridChargerStandbyTransitionsUseLongerEffectWindow },
        { "solar startup from standby keeps capacity uncertainty until timeout", testSolarStartupFromStandbyKeepsCapacityUncertaintyUntilTimeout },
        { "global-domain solar contributes to storage forecast", testGlobalSolarContributesToStorageForecast },
        { "pending global-domain solar increase is detectable", testPendingGlobalSolarIncreaseIsDetectable },
        { "solar capacity-limited timeout does not advance safe setpoint", testSolarCapacityLimitedTimeoutDoesNotAdvanceSafeSetpoint },
        { "charger increase raises meter and reduces effective output", testChargerIncreaseRaisesMeterAndReducesEffectiveOutput },
        { "queued charger absorbs storage residual before solar backoff", testQueuedChargerAbsorbsStorageResidualBeforeSolarBackoff },
        { "queued solar reduction prevents duplicate solar backoff", testQueuedSolarReductionPreventsDuplicateSolarBackoff },
        { "pending charger reduction prevents solar probe increase", testPendingChargerReductionPreventsSolarProbeIncrease },
        { "same-step storage backoff must not replace solar target decision", testSameStepStorageBackoffMustNotReplaceSolarTargetDecision },
        { "same-step storage plan is subtracted from solar target correction", testSameStepStoragePlanIsSubtractedFromSolarTargetCorrection },
        { "same-step storage plan can require opposite solar target correction", testSameStepStoragePlanCanRequireOppositeSolarTargetCorrection },
        { "same-step battery increase reduces solar probe residual", testSameStepBatteryIncreaseReducesSolarProbeResidual },
        { "same-step storage plan shifts full solar target forecast", testSameStepStoragePlanShiftsFullSolarTargetForecast },
        { "queued battery standby and global solar backoff share global forecast", testQueuedBatteryStandbyAndGlobalSolarBackoffShareGlobalForecast },
        { "minimum power and self-consumption are part of effective delta", testMinimumPowerAndSelfConsumptionArePartOfEffectiveDelta },
        { "parallel independent actuators are combined by summing effects", testParallelIndependentActuatorsAreCombinedBySummingEffects },
        { "dynamic target learning is allowed while storage pending", testDynamicTargetLearningIsAllowedWhileStoragePending },
        { "grid charger available-power limit requires storage backoff", testGridChargerAvailablePowerLimitedRequiresStorageBackoff },
        { "hysteresis expands no-command band", testHysteresisExpandsNoCommandBand },
        { "small correction damping halves only below threshold", testSmallCorrectionDampingHalvesOnlyBelowThreshold },
        { "small solar probe correction is damped", testSmallSolarProbeCorrectionIsDamped },
        { "correction residual uses nearest corridor edge", testCorrectionResidualUsesNearestCorridorEdge },
        { "outside corridor uncertainty ratio", testOutsideCorridorUncertaintyRatio },
        { "target change distance uses largest target drift", testTargetChangeDistanceUsesLargestTargetDrift },
        { "target band error uses optimistic forecast edge", testTargetBandErrorUsesOptimisticForecastEdge },
        { "target band error uses measured grid only for collapsed forecast", testTargetBandErrorUsesMeasuredGridOnlyForCollapsedForecast },
        { "target band error must use current forecast not planner estimate", testTargetBandErrorMustUseCurrentForecastNotPlannerEstimate },
        { "solar target correction uses no-effect edge for increases", testSolarTargetCorrectionUsesNoEffectEdgeForIncreases },
        { "solar target correction waits for pending ledger corridor", testSolarTargetCorrectionWaitsForPendingLedgerCorridor },
        { "solar target correction does not probe against pending reduction full-effect edge", testSolarTargetCorrectionDoesNotProbeAgainstPendingReductionFullEffectEdge },
        { "expected meter value requires collapsed forecast", testExpectedMeterValueRequiresCollapsedForecast },
        { "target ordering rejects or clamps invalid global target", testTargetOrderingRejectsOrClampsInvalidGlobalTarget },
        { "safety override is still ledger request", testSafetyOverrideIsStillLedgerRequest },
        { "power-state transition blocks until latest effect", testPowerStateTransitionBlocksUntilLatestEffect },
        { "battery standby assumption allows grid charger before confirmation", testBatteryStandbyAssumptionAllowsGridChargerBeforeConfirmation },
        { "storage output and charger overlap is detectable", testStorageOutputAndChargerOverlapIsDetectable },
    };

    size_t passed = 0;
    for (auto const& test : tests) {
        try {
            test.second();
            ++passed;
        } catch (std::exception const& exc) {
            std::cerr << "[FAIL] " << test.first << ": " << exc.what() << std::endl;
            return 1;
        }
    }

    std::cout << "Predictive control tests passed: " << passed << "/" << tests.size() << std::endl;
    return 0;
}
