#include "RestartHelper.h"
#include "PowerLimiterInverter.h"
#include "PowerLimiterBatteryInverter.h"
#include "PowerLimiterSolarInverter.h"
#include "PowerLimiterSmartBufferInverter.h"
#include "SunPosition.h"
#include <esp_log.h>
#include <LogHelper.h>
#include <algorithm>
#include <cmath>
#include <limits>

#undef TAG
static const char* TAG = "dynamicPowerLimiter";
#define SUBTAG _logPrefix

static bool millisAtOrAfter(uint32_t timestamp, uint32_t reference)
{
    auto constexpr halfOfAllMillis = std::numeric_limits<uint32_t>::max() / 2;
    return (timestamp - reference) < halfOfAllMillis;
}

std::unique_ptr<PowerLimiterInverter> PowerLimiterInverter::create(PowerLimiterInverterConfig const& config)
{
    std::unique_ptr<PowerLimiterInverter> upInverter;

    switch (config.PowerSource) {
        case PowerLimiterInverterConfig::InverterPowerSource::Battery:
            upInverter = std::make_unique<PowerLimiterBatteryInverter>(config);
            break;
        case PowerLimiterInverterConfig::InverterPowerSource::Solar:
            upInverter = std::make_unique<PowerLimiterSolarInverter>(config);
            break;
        case PowerLimiterInverterConfig::InverterPowerSource::SmartBuffer:
            upInverter = std::make_unique<PowerLimiterSmartBufferInverter>(config);
            break;
    }

    if (nullptr == upInverter->_spInverter) { return nullptr; }

    return std::move(upInverter);
}

PowerLimiterInverter::PowerLimiterInverter(PowerLimiterInverterConfig const& config)
    : _config(config)
{
    _spInverter = Hoymiles.getInverterBySerial(config.Serial);
    if (!_spInverter) { return; }

    snprintf(_serialStr, sizeof(_serialStr), "%0x%08x",
            static_cast<uint32_t>((config.Serial >> 32) & 0xFFFFFFFF),
            static_cast<uint32_t>(config.Serial & 0xFFFFFFFF));

    snprintf(_logPrefix, sizeof(_logPrefix), "Inverter %s", _serialStr);
}

PowerLimiterInverter::Eligibility PowerLimiterInverter::getEligibility() const
{
    // at dawn, solar-powered inverters switch to standby, but are still
    // reachable. during this time, we shall not use them. we assume that
    // it is already "night" when the inverter switches to standby, so this
    // check makes sense.
    if (isSolarPowered() && !SunPosition.isDayPeriod()) { return Eligibility::Nighttime; }

    if (!isReachable()) { return Eligibility::Unreachable; }

    if (!isSendingCommandsEnabled()) { return Eligibility::SendingCommandsDisabled; }

    // the model-dependent maximum AC power output is only known after the
    // first DevInfoSimpleCommand succeeded. we desperately need this info, so
    // the inverter is not eligible until this value is known.
    if (getInverterMaxPowerWatts() == 0) { return Eligibility::MaxOutputUnknown; }

    if (_oNextUpdateAttemptMillis && !millisAtOrAfter(millis(), *_oNextUpdateAttemptMillis)) {
        return Eligibility::CommandBackoff;
    }

    // after startup, the limit effective at the inverter is not known. the
    // respective message to request this info is only sent after a significant
    // backoff (~5 minutes, see upstream FAQ). this is to avoid error messages
    // to appear in the inverter's event log.
    if (getCurrentLimitWatts() == 0) { return Eligibility::CurrentLimitUnknown; }

    // inverters not connected to the grid are not eligible, as they cannot
    // produce power.
    if (getGridVoltage() < 100.0) { return Eligibility::GridDisconnected; }

    return Eligibility::Eligible;
}

bool PowerLimiterInverter::isEligible() const
{
    return getEligibility() == Eligibility::Eligible;
}

bool PowerLimiterInverter::update(bool allowNewDispatch)
{
    auto reset = [this]() -> bool {
        _oTargetPowerState = std::nullopt;
        _oTargetPowerLimitWatts = std::nullopt;
        _forcePowerStateCommand = false;
        _allowParallelStartupCommands = false;
        _oUpdateStartMillis = std::nullopt;
        _oInFlightPowerLimitWatts = std::nullopt;
        _oInFlightPowerLimitExpectedOutputAcWatts = std::nullopt;
        _oInFlightPowerState = std::nullopt;
        _oInFlightPowerStateExpectedOutputAcWatts = std::nullopt;
        return false;
    };

    if (!_oTargetPowerState.has_value()
            && CMD_PENDING == _spInverter->PowerCommand()->getLastPowerCommandSuccess()) {
        return true;
    }

    switch (getEligibility()) {
        case Eligibility::Eligible:
            break;

        case Eligibility::CurrentLimitUnknown:
            // we actually can and must do something about this: set the configured
            // lower power limit. the inverter becomes eligible shortly and
            // inverters whose current limit is not fetched for some reason (see
            // #1427) are "woken up".
            if (!_oTargetPowerLimitWatts.has_value()
                    && hasRestoredLimitPendingFeedback()) {
                break;
            }
            if (!_oTargetPowerLimitWatts.has_value()) {
                DTU_LOGD("bootstrapping by setting lower power limit");
                _oTargetPowerLimitWatts = _config.LowerPowerLimit;
            }
            break;

        case Eligibility::CommandBackoff:
            return reset();
            break;

        default:
            return reset();
            break;
    }

    // do not reset _updateTimeouts below if no state change requested
    if (!_oTargetPowerState.has_value() && !_oTargetPowerLimitWatts.has_value()) {
        return reset();
    }

    if (!_oUpdateStartMillis.has_value()) {
        _oUpdateStartMillis = millis();
    }

    auto updateFailure = [this,&reset]() -> bool {
        ++_updateTimeouts;
        _oNextUpdateAttemptMillis = millis() + _failedUpdateRetryBackoffMillis;

        // NOTE that these thresholds are not correlated to a specific time, since
        // this counts timeouts and failures, not absolute time. after any timeout or
        // failure, an update cycle ends. a new timeout or failure can only happen
        // after starting a new update cycle, which in turn is only started if the
        // DPL did calculate a new limit, which in turn does not happen while the
        // inverter is unreachable, no matter how long (a whole night) that might be.
        if (_updateTimeouts >= 20) {
            DTU_LOGE("restarting system since inverter is unresponsive");
            RestartHelper.triggerRestart("power_limiter_inverter_unresponsive");
        }
        else if (_updateTimeouts >= 10) {
            DTU_LOGW("issuing restart command after update timed out or failed %d times",
                    _updateTimeouts);
            _spInverter->suppressNextRequestHistory();
            _spInverter->sendRestartControlRequest();
        }

        return reset();
    };

    if ((millis() - *_oUpdateStartMillis) > 30 * 1000) {
        DTU_LOGW("timeout (%d in succession), state transition pending: %s, limit pending: %s",
                _updateTimeouts,
                (_oTargetPowerState.has_value()?"yes":"no"),
                (_oTargetPowerLimitWatts.has_value()?"yes":"no"));

        return updateFailure();
    }

    auto constexpr halfOfAllMillis = std::numeric_limits<uint32_t>::max() / 2;

    auto startupFromAssumedStandby = [this]() -> bool {
        return _oTargetPowerState
            && *_oTargetPowerState
            && isUsingAssumedOutput()
            && getCurrentOutputAcWatts() == 0;
    };

    auto switchPowerState = [this, allowNewDispatch, &startupFromAssumedStandby](bool transitionOn) -> bool {
        // no power state transition requested at all
        if (!_oTargetPowerState.has_value()) { return false; }

        // the transition that may be started is not the one which is requested
        if (transitionOn != *_oTargetPowerState) { return false; }

        // wait for pending power command(s) to complete
        auto lastPowerCommandState = _spInverter->PowerCommand()->getLastPowerCommandSuccess();
        if (CMD_PENDING == lastPowerCommandState) {
            return true;
        }

        auto lastPowerCommandMillis = _spInverter->PowerCommand()->getLastUpdateCommand();
        if (lastPowerCommandMillis > 0
                && millisAtOrAfter(lastPowerCommandMillis, *_oUpdateStartMillis)
                && _oInFlightPowerState.has_value()) {
            auto const acknowledgedState =
                _oInFlightPowerState.value_or(*_oTargetPowerState);
            auto const acknowledgedExpectedOutput =
                _oInFlightPowerStateExpectedOutputAcWatts.value_or(_expectedOutputAcWatts);
            if (CMD_OK == lastPowerCommandState) {
                recordSuccessfulTargetCommand(
                        lastPowerCommandMillis,
                        acknowledgedExpectedOutput,
                        false);
                recordTargetDispatchEvent({
                    TargetDispatchKind::PowerState,
                    acknowledgedState ? acknowledgedExpectedOutput : static_cast<uint16_t>(0),
                    acknowledgedExpectedOutput,
                    lastPowerCommandMillis,
                    acknowledgedState,
                    true,
                });
            }

            _oInFlightPowerState = std::nullopt;
            _oInFlightPowerStateExpectedOutputAcWatts = std::nullopt;

            if (CMD_OK == lastPowerCommandState
                    && _oTargetPowerState
                    && *_oTargetPowerState == acknowledgedState) {
                _oTargetPowerState = std::nullopt;
            }

            if (CMD_OK == lastPowerCommandState) {
                if (!_oTargetPowerState) { return false; }
                _oUpdateStartMillis = millis();
                if (transitionOn != *_oTargetPowerState) { return false; }
            }
        }

        bool const batteryStopStillHasEffectiveOutput =
            isBatteryPowered()
            && !*_oTargetPowerState
            && (getCurrentOutputAcWatts() > 0
                || getExpectedOutputAcWatts() > 0
                || isUsingAssumedOutput());
        bool const forcePowerStateCommand =
            _forcePowerStateCommand
            && _oTargetPowerState
            && transitionOn == *_oTargetPowerState;
        bool const shouldSendPowerState =
            isProducing() != *_oTargetPowerState
            || batteryStopStillHasEffectiveOutput
            || forcePowerStateCommand
            || (transitionOn && startupFromAssumedStandby());

        if (shouldSendPowerState) {
            if (!allowNewDispatch) { return true; }

            DTU_LOGI("%s inverter...", ((*_oTargetPowerState)?"Starting":"Stopping"));
            _spInverter->suppressNextRequestHistory();
            if (!_spInverter->sendPowerControlRequestCompleteOnTx(*_oTargetPowerState)) {
                DTU_LOGW("failed to queue power-state command for urgent target");
                return true;
            }

            _oInFlightPowerState = *_oTargetPowerState;
            _oInFlightPowerStateExpectedOutputAcWatts = _expectedOutputAcWatts;
            recordTargetDispatchEvent({
                TargetDispatchKind::PowerState,
                *_oTargetPowerState ? _expectedOutputAcWatts : static_cast<uint16_t>(0),
                _expectedOutputAcWatts,
                0,
                *_oTargetPowerState,
                false,
            });
            return true;
        }

        _oTargetPowerState = std::nullopt; // target power state reached
        return false;
    };

    // we use a lambda function here to be able to use return statements,
    // which allows to avoid if-else-indentions and improves code readability
    auto updateLimit = [this, &updateFailure, allowNewDispatch]() -> bool {
        // no limit update requested at all
        if (!_oTargetPowerLimitWatts.has_value()) { return false; }

        // wait for pending limit command(s) to complete
        auto lastLimitCommandState = _spInverter->SystemConfigPara()->getLastLimitCommandSuccess();
        if (CMD_PENDING == lastLimitCommandState) {
            return true;
        }

        // if no limit command is pending, the SystemConfigPara does report the
        // current limit, as the answer by the inverter to a limit command is
        // the canonical source that updates the known current limit.
        auto currentRelativeLimit = _spInverter->SystemConfigPara()->getLimitPercent();

        // we assume having exclusive control over the inverter. if the last
        // limit command completed and if it was sent after we started the last
        // update cycle, we should assume *our* requested limit was set.
        uint32_t lastLimitCommandMillis = _spInverter->SystemConfigPara()->getLastUpdateCommand();
        if (lastLimitCommandMillis > 0
                && (lastLimitCommandMillis - *_oUpdateStartMillis) < halfOfAllMillis
                && _oInFlightPowerLimitWatts.has_value()) {
            auto const acknowledgedLimit =
                _oInFlightPowerLimitWatts.value_or(*_oTargetPowerLimitWatts);
            auto const acknowledgedExpectedOutput =
                _oInFlightPowerLimitExpectedOutputAcWatts.value_or(_expectedOutputAcWatts);
            float const acknowledgedRelativeLimit =
                static_cast<float>(acknowledgedLimit * 100) / getInverterMaxPowerWatts();

            DTU_LOGD("limit update %s, actual limit is %.1f %% (%.0f W "
                    "respectively), effective %d ms after update started, "
                    "requested were %.1f %%",
                    (CMD_OK == lastLimitCommandState)?"succeeded":"FAILED",
                    currentRelativeLimit,
                    (currentRelativeLimit * getInverterMaxPowerWatts() / 100),
                    (lastLimitCommandMillis - *_oUpdateStartMillis),
                    acknowledgedRelativeLimit);

            auto deviation = std::abs(acknowledgedRelativeLimit - currentRelativeLimit);
            if (CMD_OK == lastLimitCommandState && deviation > 2.0) {
                DTU_LOGW("expected limit of %.1f %% and actual limit of "
                        "%.1f %% mismatch by more than 2 %%, is the DPL in exclusive "
                        "control over the inverter?",
                        acknowledgedRelativeLimit, currentRelativeLimit);
            }

            if (CMD_OK == lastLimitCommandState) {
                _oRestoredLimitMillis = std::nullopt;
                recordSuccessfulTargetCommand(
                        lastLimitCommandMillis,
                        acknowledgedExpectedOutput,
                        false);
                recordTargetDispatchEvent({
                    TargetDispatchKind::PowerLimit,
                    acknowledgedExpectedOutput,
                    acknowledgedExpectedOutput,
                    lastLimitCommandMillis,
                    false,
                    true,
                });
            }

            _oInFlightPowerLimitWatts = std::nullopt;
            _oInFlightPowerLimitExpectedOutputAcWatts = std::nullopt;

            if (_oTargetPowerLimitWatts
                    && *_oTargetPowerLimitWatts == acknowledgedLimit) {
                _oTargetPowerLimitWatts = std::nullopt;
            }

            if (CMD_OK != lastLimitCommandState) {
                // we don't retry a failed limit command, since it might as well
                // be outdated by now. the DPL will calculate a new limit for
                // the inverter and we will send that later instead.
                return updateFailure();
            }

            if (!_oTargetPowerLimitWatts) { return false; }

            _oUpdateStartMillis = millis();
        }

        float newRelativeLimit = static_cast<float>(*_oTargetPowerLimitWatts * 100) / getInverterMaxPowerWatts();

        if (!allowNewDispatch) { return true; }

        DTU_LOGI("sending limit of %.1f %% (%.0f W respectively), max output is %d W",
                newRelativeLimit, (newRelativeLimit * getInverterMaxPowerWatts() / 100),
                getInverterMaxPowerWatts());

        _spInverter->suppressNextRequestHistory();
        if (!_spInverter->sendActivePowerControlRequest(newRelativeLimit,
                    PowerLimitControlType::RelativNonPersistent)) {
            DTU_LOGW("failed to queue limit command for %.1f %%", newRelativeLimit);
            return true;
        }

        _oInFlightPowerLimitWatts = *_oTargetPowerLimitWatts;
        _oInFlightPowerLimitExpectedOutputAcWatts = _expectedOutputAcWatts;
        recordTargetDispatchEvent({
            TargetDispatchKind::PowerLimit,
            _expectedOutputAcWatts,
            _expectedOutputAcWatts,
            0,
            false,
            false,
        });

        return true;
    };

    // disable power production as soon as possible.
    // setting the power limit is less important once the inverter is off.
    if (switchPowerState(false)) { return true; }

    bool const assumedStandbyStartup = startupFromAssumedStandby();
    bool const parallelStartupAllowed =
        assumedStandbyStartup
        || (_allowParallelStartupCommands
                && _oTargetPowerState
                && *_oTargetPowerState
                && !isProducing());
    bool const limitPending = updateLimit();
    if (limitPending && !parallelStartupAllowed) { return true; }

    // Enable power production only after setting the desired limit. After a
    // standby command, stale inverter stats may still report production; do
    // not let those stale stats satisfy a follow-up startup target.
    if (switchPowerState(true)) { return true; }

    if (limitPending) { return true; }

    _updateTimeouts = 0;

    return reset();
}

bool PowerLimiterInverter::retire(bool allowNewDispatch)
{
    if (!_retired) { standby(); }
    _retired = true;
    return update(allowNewDispatch);
}

void PowerLimiterInverter::reassertTargetPowerState()
{
    if (!_oTargetPowerState) { return; }
    _forcePowerStateCommand = true;
}

std::vector<PowerLimiterInverter::TargetDispatchEvent>
PowerLimiterInverter::consumeTargetDispatchEvents()
{
    auto events = _targetDispatchEvents;
    _targetDispatchEvents.clear();
    return events;
}

std::optional<uint32_t> PowerLimiterInverter::getLatestStatsMillis() const
{
    uint32_t now = millis();

    // concerns both power limits and start/stop/restart commands and is
    // only updated if a respective response was received from the inverter
    auto lastUpdateCmdAge = std::min(
            now - _spInverter->SystemConfigPara()->getLastUpdateCommand(),
            now - _spInverter->PowerCommand()->getLastUpdateCommand()
    );

    // we use _oStatsMillis to persist a stats update timestamp, as we are
    // looking for the single oldest inverter stats which is still younger than
    // the last update command. we shall not just return the actual youngest
    // stats timestamp if newer stats arrived while no update command was sent
    // in the meantime.
    if (_oStatsMillis && lastUpdateCmdAge < (now - *_oStatsMillis)) {
        _oStatsMillis.reset();
    }

    if (!_oStatsMillis) {
        auto lastStatsMillis = _spInverter->Statistics()->getLastUpdate();
        auto lastStatsAge = now - lastStatsMillis;
        if (lastStatsAge > lastUpdateCmdAge) {
            return std::nullopt;
        }

        _oStatsMillis = lastStatsMillis;
    }

    return _oStatsMillis;
}

uint32_t PowerLimiterInverter::getCurrentStatsMillis() const
{
    return _spInverter->Statistics()->getLastUpdate();
}

std::optional<uint32_t> PowerLimiterInverter::getOutputReferenceMillis() const
{
    if (isUsingAssumedOutput()) {
        return _oAssumedOutputValidAfterMillis;
    }

    if (isUsingRestoredState()) {
        return _oRestoredStateMillis;
    }

    auto lastStatsMillis = _spInverter->Statistics()->getLastUpdate();
    if (lastStatsMillis == 0) { return std::nullopt; }

    return lastStatsMillis;
}

uint16_t PowerLimiterInverter::getInverterMaxPowerWatts() const
{
    auto const maxPower = _spInverter->getMaxPower();
    if (maxPower > 0) { return maxPower; }
    if (isUsingRestoredState()) { return _restoredRuntimeState.MaxPowerWatts; }
    return 0;
}

uint16_t PowerLimiterInverter::getConfiguredMaxPowerWatts() const
{
    return std::min(getInverterMaxPowerWatts(), _config.UpperPowerLimit);
}

uint16_t PowerLimiterInverter::getCurrentOutputAcWatts() const
{
    if (isUsingAssumedOutput()) { return *_oAssumedOutputAcWatts; }
    if (isUsingRestoredState()) { return _restoredRuntimeState.OutputAcWatts; }
    return getMeasuredOutputAcWatts();
}

std::optional<float> PowerLimiterInverter::getTemperatureCelsius() const
{
    auto stats = _spInverter->Statistics();
    if (!stats->hasChannelFieldValue(TYPE_INV, CH0, FLD_T)) {
        return std::nullopt;
    }

    float temperature = stats->getChannelFieldValue(TYPE_INV, CH0, FLD_T);
    if (!std::isfinite(temperature) || temperature < -40.0f || temperature > 140.0f) {
        return std::nullopt;
    }

    return temperature;
}

uint16_t PowerLimiterInverter::getMeasuredOutputAcWatts() const
{
    return _spInverter->Statistics()->getChannelFieldValue(TYPE_AC, CH0, FLD_PAC);
}

uint16_t PowerLimiterInverter::getExpectedOutputAcWatts() const
{
    if (!hasPendingTarget()) {
        if (isUsingRestoredState()) {
            return _restoredRuntimeState.ExpectedOutputAcWatts;
        }

        // the inverter's output will not change due to commands being sent
        return getCurrentOutputAcWatts();
    }

    return _expectedOutputAcWatts;
}

bool PowerLimiterInverter::hasStatsAtOrAfter(uint32_t timestamp) const
{
    auto lastStatsMillis = _spInverter->Statistics()->getLastUpdate();
    return lastStatsMillis > 0 && millisAtOrAfter(lastStatsMillis, timestamp);
}

bool PowerLimiterInverter::isUsingAssumedOutput() const
{
    if (!_oAssumedOutputAcWatts) { return false; }
    if (_keepAssumedOutput) { return true; }
    if (!_oAssumedOutputValidAfterMillis) { return false; }
    return !hasStatsAtOrAfter(*_oAssumedOutputValidAfterMillis);
}

bool PowerLimiterInverter::hasLimitFeedbackAtOrAfter(uint32_t timestamp) const
{
    auto const lastLimitFeedbackMillis =
        _spInverter->SystemConfigPara()->getLastUpdateRequest();
    return lastLimitFeedbackMillis > 0
        && millisAtOrAfter(lastLimitFeedbackMillis, timestamp);
}

bool PowerLimiterInverter::hasRestoredLimitPendingFeedback() const
{
    if (!_oRestoredLimitMillis) { return false; }
    return !hasLimitFeedbackAtOrAfter(*_oRestoredLimitMillis);
}

bool PowerLimiterInverter::isUsingRestoredLimit() const
{
    return hasRestoredLimitPendingFeedback();
}

bool PowerLimiterInverter::isUsingRestoredState() const
{
    if (!_oRestoredStateMillis) { return false; }
    if ((millis() - *_oRestoredStateMillis) > RestoredStateAssumptionMillis) { return false; }
    return !hasStatsAtOrAfter(*_oRestoredStateMillis);
}

bool PowerLimiterInverter::hasFreshRuntimeStateForPersistence() const
{
    if (isUsingRestoredState()) { return false; }
    if (hasRestoredLimitPendingFeedback()) { return false; }
    if (getInverterMaxPowerWatts() == 0) { return false; }
    if (getCurrentLimitWatts() == 0) { return false; }
    return getCurrentStatsMillis() > 0 || isUsingAssumedOutput();
}

PowerLimiterInverter::RuntimeState PowerLimiterInverter::getRuntimeStateForPersistence() const
{
    RuntimeState state;
    state.Serial = getSerial();
    state.OutputAcWatts = getCurrentOutputAcWatts();
    state.ExpectedOutputAcWatts = getExpectedOutputAcWatts();
    state.PowerLimitWatts = getExpectedLimitWatts();
    state.MaxPowerWatts = getInverterMaxPowerWatts();
    state.GridVoltage = getGridVoltage();
    if (!std::isfinite(state.GridVoltage) || state.GridVoltage < 0.0f) {
        state.GridVoltage = 0.0f;
    }
    state.Reachable = isReachable();
    state.Producing = isProducing() || state.OutputAcWatts > 0 || state.ExpectedOutputAcWatts > 0;
    return state;
}

void PowerLimiterInverter::restoreRuntimeState(
        RuntimeState const& state,
        uint32_t restoreMillis)
{
    if (state.Serial != getSerial()) { return; }
    if (state.MaxPowerWatts == 0 || state.PowerLimitWatts == 0) { return; }

    _restoredRuntimeState = state;
    _oRestoredStateMillis = restoreMillis;
    _oRestoredLimitMillis = restoreMillis;
    _restoredRuntimeState.PowerLimitWatts = clampLimitToConfiguredBounds(
            std::min(
                    _restoredRuntimeState.PowerLimitWatts,
                    _restoredRuntimeState.MaxPowerWatts));
    _restoredRuntimeState.OutputAcWatts = std::min(
            _restoredRuntimeState.OutputAcWatts,
            _restoredRuntimeState.MaxPowerWatts);
    _restoredRuntimeState.ExpectedOutputAcWatts = std::min(
            _restoredRuntimeState.ExpectedOutputAcWatts,
            _restoredRuntimeState.MaxPowerWatts);
    if (_restoredRuntimeState.OutputAcWatts > 0
            || _restoredRuntimeState.ExpectedOutputAcWatts > 0) {
        _restoredRuntimeState.Producing = true;
    }

    // Restore is only a startup assumption. Do not mark it as a target, or
    // update() would enqueue a fresh RF limit command for every inverter.
    _expectedOutputAcWatts = _restoredRuntimeState.ExpectedOutputAcWatts;

    DTU_LOGI("restored startup target: output %u W, expected %u W, limit %u W",
            _restoredRuntimeState.OutputAcWatts,
            _restoredRuntimeState.ExpectedOutputAcWatts,
            _restoredRuntimeState.PowerLimitWatts);
}

void PowerLimiterInverter::recordSuccessfulTargetCommand(
        uint32_t commandMillis,
        uint16_t assumedOutputAcWatts,
        bool keepAssumedOutput)
{
    _oNextUpdateAttemptMillis = std::nullopt;
    _oRestoredStateMillis = std::nullopt;
    _oAssumedOutputAcWatts = assumedOutputAcWatts;
    _keepAssumedOutput = keepAssumedOutput;
    _oAssumedOutputValidAfterMillis = commandMillis
        + (keepAssumedOutput ? 0 : TargetEffectAssumptionMillis);
}

void PowerLimiterInverter::recordTargetDispatchEvent(TargetDispatchEvent event)
{
    _targetDispatchEvents.push_back(event);
}

void PowerLimiterInverter::setMaxOutput(bool fastStart)
{
    auto const maxOutput = getConfiguredMaxPowerWatts();
    _allowParallelStartupCommands = fastStart;
    _oTargetPowerState = true;
    setAcOutput(maxOutput);
}

uint32_t PowerLimiterInverter::getRadioQueueSize() const
{
    if (!_spInverter) { return 0; }
    auto const radio = _spInverter->getRadio();
    if (radio == nullptr) { return 0; }
    return radio->getQueueSize();
}

bool PowerLimiterInverter::restart()
{
    _spInverter->suppressNextRequestHistory();
    return _spInverter->sendRestartControlRequest();
}

float PowerLimiterInverter::getGridVoltage() const
{
    if (isUsingRestoredState() && _restoredRuntimeState.GridVoltage >= 100.0f) {
        return _restoredRuntimeState.GridVoltage;
    }
    return _spInverter->Statistics()->getChannelFieldValue(TYPE_AC, CH0, FLD_UAC);
}

float PowerLimiterInverter::getDcVoltage(uint8_t input)
{
    return _spInverter->Statistics()->getChannelFieldValue(TYPE_DC,
            static_cast<ChannelNum_t>(input), FLD_UDC);
}

uint16_t PowerLimiterInverter::getCurrentLimitWatts() const
{
    auto currentLimitPercent = _spInverter->SystemConfigPara()->getLimitPercent();
    auto const currentLimit = static_cast<uint16_t>(
            currentLimitPercent * getInverterMaxPowerWatts() / 100);
    if (currentLimit > 0) { return currentLimit; }
    if (_oLastTargetPowerLimitWatts) {
        return *_oLastTargetPowerLimitWatts;
    }
    if (isUsingRestoredLimit()) { return _restoredRuntimeState.PowerLimitWatts; }
    return 0;
}

uint16_t PowerLimiterInverter::clampLimitToConfiguredBounds(uint16_t power) const
{
    auto upper = getConfiguredMaxPowerWatts();
    power = std::min(power, upper);

    if (isProducing()) {
        power = std::max(power, _config.LowerPowerLimit);
    }

    return power;
}

uint16_t PowerLimiterInverter::getLimitResolutionWatts() const
{
    return std::max<uint16_t>(1, (getInverterMaxPowerWatts() + 999) / 1000);
}

bool PowerLimiterInverter::applyConfiguredLimitBounds()
{
    if (!isEligible() || hasPendingTarget()) { return false; }

    auto const currentLimit = getCurrentLimitWatts();
    auto const lowerLimit = isProducing() ? _config.LowerPowerLimit : 0;
    auto const upperLimit = getConfiguredMaxPowerWatts();
    auto const resolution = getLimitResolutionWatts();
    if (static_cast<uint32_t>(currentLimit) + resolution >= lowerLimit
            && currentLimit <= static_cast<uint32_t>(upperLimit) + resolution) {
        return false;
    }

    auto const targetLimit = clampLimitToConfiguredBounds(currentLimit);

    DTU_LOGI("current limit %u W is outside configured range %u..%u W, refreshing to %u W",
            currentLimit, lowerLimit, upperLimit, targetLimit);

    auto expectedOutput = getCurrentOutputAcWatts();
    expectedOutput = std::min(expectedOutput, getConfiguredMaxPowerWatts());
    if (isProducing()) {
        expectedOutput = std::max(expectedOutput, _config.LowerPowerLimit);
    }

    setExpectedOutputAcWatts(expectedOutput);
    setTargetPowerLimitWatts(targetLimit);
    return true;
}

bool PowerLimiterInverter::refreshStaleLimit()
{
    if (!isEligible() || hasPendingTarget() || !isProducing()) { return false; }
    if (hasRestoredLimitPendingFeedback()) { return false; }

    auto now = millis();
    auto lastLimitCommandMillis = _spInverter->SystemConfigPara()->getLastUpdateCommand();
    if (lastLimitCommandMillis > 0 && (now - lastLimitCommandMillis) < _limitRefreshIntervalMillis) {
        return false;
    }

    auto targetLimit = _oLastTargetPowerLimitWatts.value_or(getCurrentLimitWatts());
    targetLimit = clampLimitToConfiguredBounds(targetLimit);
    if (targetLimit == 0) { return false; }

    DTU_LOGD("periodically refreshing limit %u W", targetLimit);

    setExpectedOutputAcWatts(getCurrentOutputAcWatts());
    setTargetPowerLimitWatts(targetLimit);
    return true;
}

void PowerLimiterInverter::debug() const
{
    if (!DTU_LOG_IS_VERBOSE) { return; }

    String eligibility("disqualified");
    switch (getEligibility()) {
        case Eligibility::Nighttime:
            eligibility += " (nighttime)";
            break;
        case Eligibility::Unreachable:
            eligibility += " (unreachable)";
            break;
        case Eligibility::SendingCommandsDisabled:
            eligibility += " (sending commands disabled)";
            break;
        case Eligibility::MaxOutputUnknown:
            eligibility += " (max output unknown)";
            break;
        case Eligibility::CurrentLimitUnknown:
            eligibility += " (current limit unknown)";
            break;
        case Eligibility::CommandBackoff:
            eligibility += " (command backoff)";
            break;
        case Eligibility::GridDisconnected:
            eligibility += " (grid disconnected)";
            break;
        case Eligibility::Eligible:
            eligibility = "eligible";
            break;
    }

    DTU_LOGV("State Details");
    DTU_LOGV("    %s-powered, %s %d W, output %s power meter reading",
        (isSmartBufferPowered()?"smart-buffer":(isSolarPowered()?"solar":"battery")),
        (isProducing()?"producing":"standing by at"), getCurrentOutputAcWatts(),
        (isBehindPowerMeter()?"included in":"excluded from")
    );
    DTU_LOGV("    lower/current/upper limit: %d/%d/%d W, output capability: %d W",
        _config.LowerPowerLimit, getCurrentLimitWatts(), _config.UpperPowerLimit,
        getInverterMaxPowerWatts()
    );
    DTU_LOGV("    sending commands %s, %s, %s",
        (isSendingCommandsEnabled()?"enabled":"disabled"),
        (isReachable()?"reachable":"offline"), eligibility.c_str()
    );
    DTU_LOGV("    max reduction production/standby: %d/%d W, max increase: %d W",
        getMaxReductionWatts(false), getMaxReductionWatts(true), getMaxIncreaseWatts()
    );
    DTU_LOGV("    target limit/output/state: %i W (%s)/%d W/%s, %d update timeouts",
        (_oTargetPowerLimitWatts.has_value()?*_oTargetPowerLimitWatts:-1),
        (_oTargetPowerLimitWatts.has_value()?"update":"unchanged"),
        getExpectedOutputAcWatts(),
        (_oTargetPowerState.has_value()?(*_oTargetPowerState?"production":"standby"):"unchanged"),
        getUpdateTimeouts()
    );

    char mpptDebug[160] = {0}; // AI says this is sufficient for up to 6 MPPTs
    size_t offset = snprintf(mpptDebug, sizeof(mpptDebug), "    MPPTs AC power/DC voltage:");

    auto pStats = _spInverter->Statistics();
    float inverterEfficiencyFactor = pStats->getChannelFieldValue(TYPE_INV, CH0, FLD_EFF) / 100;
    std::vector<MpptNum_t> dcMppts = _spInverter->getMppts();

    for (auto& m : dcMppts) {
        float mpptPowerAC = 0.0;
        float mpptVoltageDC = 0.0;
        std::vector<ChannelNum_t> mpptChnls = _spInverter->getChannelsDCByMppt(m);

        for (auto& c : mpptChnls) {
            mpptPowerAC += pStats->getChannelFieldValue(TYPE_DC, c, FLD_PDC) * inverterEfficiencyFactor;
            mpptVoltageDC = std::max(mpptVoltageDC, pStats->getChannelFieldValue(TYPE_DC, c, FLD_UDC));
        }

        offset += snprintf(mpptDebug + offset, sizeof(mpptDebug) - offset,
                " %c: %.0f W/%.1f V", mpptName(m), mpptPowerAC, mpptVoltageDC);
    }

    DTU_LOGV("%s", mpptDebug);
}

char PowerLimiterInverter::mpptName(MpptNum_t mppt)
{
    switch (mppt) {
        case MpptNum_t::MPPT_A:
            return 'a';

        case MpptNum_t::MPPT_B:
            return 'b';

        case MpptNum_t::MPPT_C:
            return 'c';

        case MpptNum_t::MPPT_D:
            return 'd';

        default:
            return '?';
    }
}
