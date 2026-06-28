#!/usr/bin/env python3
"""Generate an HTML timeline report for the DPL reference scenarios."""

from __future__ import annotations

from dataclasses import dataclass
from html import escape
from pathlib import Path
import sys
from typing import Callable, Dict, Iterable, List, Optional, Sequence

from predictive_control import (
    ActuatorCapabilities,
    ActuatorKind,
    ActuatorState,
    CorrectionDecision,
    EffectKind,
    ForecastDomain,
    MeterSnapshot,
    PredictiveControlModel,
    RequestCause,
    RequestDomain,
    RequestState,
    TransitionKind,
    allocate_mixed_solar,
    decide_correction,
    dynamic_target_learning_allowed,
    global_target_from_solar_offset,
    validate_target_ownership,
)


REPORT_PATH = Path(__file__).with_name("dpl_sim_report.html")


@dataclass
class TimelineSample:
    t_ms: int
    physical_grid_w: Optional[float] = None
    storage_target_w: Optional[float] = 0.0
    global_target_w: Optional[float] = 0.0
    storage_min_w: Optional[float] = None
    storage_nominal_w: Optional[float] = None
    storage_max_w: Optional[float] = None
    global_min_w: Optional[float] = None
    global_nominal_w: Optional[float] = None
    global_max_w: Optional[float] = None
    battery_safe_w: Optional[float] = None
    battery_requested_w: Optional[float] = None
    charger_safe_w: Optional[float] = None
    charger_requested_w: Optional[float] = None
    solar_safe_w: Optional[float] = None
    solar_requested_w: Optional[float] = None
    storage_decision: str = ""
    note: str = ""


@dataclass
class Event:
    t_ms: int
    label: str


@dataclass
class Scenario:
    title: str
    summary: str
    samples: List[TimelineSample]
    events: List[Event]
    assertions: List[str]


def actor_value(
    model: PredictiveControlModel,
    key: str,
    attr: str,
) -> Optional[float]:
    actor = model.actuators.get(key)
    if actor is None:
        return None
    return getattr(actor, attr)


def record(
    model: PredictiveControlModel,
    t_ms: int,
    physical_grid_w: float,
    *,
    storage_target_w: float = 0.0,
    global_target_w: float = 0.0,
    proven_solar_only_effective_w: float = 0.0,
    note: str = "",
) -> TimelineSample:
    snapshot = MeterSnapshot(
        physical_grid_watts=physical_grid_w,
        meter_sample_millis=t_ms,
        proven_solar_only_effective_watts=proven_solar_only_effective_w,
    )
    storage = model.forecast(snapshot, ForecastDomain.STORAGE)
    global_ = model.forecast(snapshot, ForecastDomain.GLOBAL)
    decision = decide_correction(storage_target_w, storage)

    return TimelineSample(
        t_ms=t_ms,
        physical_grid_w=physical_grid_w,
        storage_target_w=storage_target_w,
        global_target_w=global_target_w,
        storage_min_w=storage.grid_min_watts,
        storage_nominal_w=storage.grid_nominal_watts,
        storage_max_w=storage.grid_max_watts,
        global_min_w=global_.grid_min_watts,
        global_nominal_w=global_.grid_nominal_watts,
        global_max_w=global_.grid_max_watts,
        battery_safe_w=actor_value(model, "bat", "safe_setpoint_watts"),
        battery_requested_w=actor_value(model, "bat", "requested_setpoint_watts"),
        charger_safe_w=actor_value(model, "charger", "safe_setpoint_watts"),
        charger_requested_w=actor_value(model, "charger", "requested_setpoint_watts"),
        solar_safe_w=actor_value(model, "solar", "safe_setpoint_watts"),
        solar_requested_w=actor_value(model, "solar", "requested_setpoint_watts"),
        storage_decision=decision.action.value,
        note=note,
    )


def simple_scenario(
    title: str,
    summary: str,
    samples: Sequence[TimelineSample],
    assertions: Iterable[str],
    events: Iterable[Event] = (),
) -> Scenario:
    return Scenario(
        title=title,
        summary=summary,
        samples=list(samples),
        events=list(events),
        assertions=list(assertions),
    )


def scenario_solar_only_offset() -> Scenario:
    model = PredictiveControlModel()
    samples = [
        record(
            model,
            0,
            -300,
            storage_target_w=0,
            global_target_w=-300,
            proven_solar_only_effective_w=300,
            note="Solar-only export is proven and removed from storage meter.",
        )
    ]
    return simple_scenario(
        "Solar-only offset does not cause storage action",
        "Physical meter exports, but storage virtual meter is at target.",
        samples,
        [
            "Storage forecast is centered at 0 W.",
            "Storage decision is HOLD; battery and charger do not compensate solar export.",
        ],
    )


def scenario_battery_pending_visible_early() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0)]
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=0,
        target_setpoint_watts=500,
        effective_delta_watts=500,
        storage_effective_delta_watts=500,
        global_effective_delta_watts=500,
        sent_millis=1000,
        latest_effect_millis=4000,
    )
    samples = [
        record(model, 900, 500, note="Before send; future battery effect is pending."),
        record(model, 1500, 0, note="Meter may already include the battery effect."),
        record(model, 4000, 0, note="After latestEffect the request can settle."),
    ]
    model.advance_ledger(4000)
    samples[-1] = record(model, 4000, 0, note="Settled; pending corridor closed.")
    return simple_scenario(
        "Battery pending command visible early",
        "A 500 W battery command drops the meter before latestEffect; no double-counting.",
        samples,
        [
            "While pending, storage corridor includes the current 0 W meter value.",
            "The controller holds instead of adding another 500 W.",
        ],
        [Event(1000, "battery request 0 -> 500 W")],
    )


def scenario_battery_pending_not_visible() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0)]
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=0,
        target_setpoint_watts=500,
        effective_delta_watts=500,
        storage_effective_delta_watts=500,
        global_effective_delta_watts=500,
        sent_millis=1000,
        latest_effect_millis=4000,
    )
    samples = [
        record(model, 1500, 500, note="Effect not visible yet, but target is in corridor."),
        record(model, 4000, 500, note="At timeout, old request leaves pending state."),
    ]
    model.advance_ledger(4000)
    samples[-1] = record(model, 4000, 500, note="Residual import becomes a new disturbance.")
    return simple_scenario(
        "Battery pending command not visible",
        "The target lies inside the pending corridor until the deterministic timeout.",
        samples,
        [
            "Before latestEffect, the controller waits.",
            "After latestEffect, residual import is visible again and can be corrected.",
        ],
        [Event(1000, "battery request 0 -> 500 W")],
    )


def scenario_not_before_and_soft_timing() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0)]
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=0,
        target_setpoint_watts=500,
        effective_delta_watts=500,
        storage_effective_delta_watts=500,
        global_effective_delta_watts=500,
        sent_millis=1000,
        not_before_effect_millis=2500,
        earliest_expected_millis=3000,
        typical_effect_millis=3500,
        latest_effect_millis=5000,
    )
    samples = [
        record(model, 1500, 500, note="After send but before hard notBefore."),
        record(model, 3000, 500, note="After notBefore; visible and not-visible branches exist."),
    ]
    return simple_scenario(
        "Hard notBefore and soft timing",
        "Hard timing excludes early visibility; soft timing never closes min/max.",
        samples,
        [
            "Before notBefore, there is no already-visible branch.",
            "After notBefore, the conservative corridor is open until latestEffect.",
        ],
        [Event(1000, "battery request sent"), Event(2500, "notBefore")],
    )


def scenario_soft_timing() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0)]
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=0,
        target_setpoint_watts=500,
        effective_delta_watts=500,
        storage_effective_delta_watts=500,
        global_effective_delta_watts=500,
        sent_millis=1000,
        earliest_expected_millis=2500,
        typical_effect_millis=3500,
        latest_effect_millis=5000,
    )
    samples = [
        record(model, 1500, 500, note="Before earliestExpected, but no hard notBefore."),
        record(model, 3500, 500, note="At typical time, min/max corridor is still open."),
    ]
    return simple_scenario(
        "Soft timing does not close corridor",
        "earliestExpected and typical affect nominal reasoning only, not conservative min/max.",
        samples,
        ["Visible and not-visible branches remain possible until latestEffect."],
        [Event(1000, "request sent"), Event(2500, "earliestExpected")],
    )


def scenario_ordered_chain() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=700)]
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=700,
        target_setpoint_watts=300,
        effective_delta_watts=-400,
        storage_effective_delta_watts=-400,
        global_effective_delta_watts=-400,
        sent_millis=1000,
        latest_effect_millis=5000,
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=300,
        target_setpoint_watts=500,
        effective_delta_watts=200,
        storage_effective_delta_watts=200,
        global_effective_delta_watts=200,
        sent_millis=1100,
        latest_effect_millis=3000,
    )
    samples = [
        record(model, 1500, -700, note="Possible states: 700, 300, 500 W."),
        record(model, 3000, -500, note="B has earlier latestEffect but cannot settle alone."),
    ]
    model.advance_ledger(3000)
    samples[-1] = record(
        model,
        3000,
        -500,
        note="Safe state remains 700 W because A is still unsettled.",
    )
    return simple_scenario(
        "Ordered same-actuator setpoint chain",
        "Absolute requests are evaluated as a chain, not independent summed deltas.",
        samples,
        [
            "Reachable battery states are 700 W, 300 W, and 500 W.",
            "The impossible 900 W state never appears.",
        ],
        [Event(1000, "700 -> 300 W"), Event(1100, "300 -> 500 W")],
    )


def scenario_ambiguous_failure_chain() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=700)]
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=700,
        target_setpoint_watts=300,
        effective_delta_watts=-400,
        storage_effective_delta_watts=-400,
        global_effective_delta_watts=-400,
        state=RequestState.FAILED_AMBIGUOUS,
        sent_millis=1000,
        latest_effect_millis=4000,
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=300,
        target_setpoint_watts=500,
        effective_delta_watts=200,
        storage_effective_delta_watts=200,
        global_effective_delta_watts=200,
        sent_millis=1100,
        latest_effect_millis=4100,
    )
    samples = [
        record(model, 1500, -500, note="Earlier ambiguous request remains in chain.")
    ]
    return simple_scenario(
        "Ambiguous earlier failure with later absolute command",
        "The later command is not interpreted as a delta against the old safe state.",
        samples,
        [
            "Ambiguous failure is not discarded.",
            "No impossible negative or 900 W battery state is generated.",
        ],
        [Event(1000, "A ambiguous"), Event(1100, "B accepted")],
    )


def scenario_later_request_earlier_latest() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=700)]
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=700,
        target_setpoint_watts=300,
        effective_delta_watts=-400,
        storage_effective_delta_watts=-400,
        global_effective_delta_watts=-400,
        sent_millis=1000,
        latest_effect_millis=5000,
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=300,
        target_setpoint_watts=500,
        effective_delta_watts=200,
        storage_effective_delta_watts=200,
        global_effective_delta_watts=200,
        sent_millis=1100,
        latest_effect_millis=3000,
    )
    samples = [record(model, 2500, -500, note="B timeout is approaching before A.")]
    model.advance_ledger(3000)
    samples.append(
        record(model, 3000, -500, note="B does not settle alone; safe remains 700 W.")
    )
    return simple_scenario(
        "Later request with earlier latestEffect",
        "A later absolute command cannot settle by itself if the earlier chain prefix is unsettled.",
        samples,
        ["The safe setpoint remains 700 W until the chain can be settled consistently."],
        [Event(1000, "A: 700 -> 300 W"), Event(1100, "B: 300 -> 500 W")],
    )


def scenario_failed_queued_retry() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0)]
    )
    request = model.queue_request(
        "bat",
        RequestDomain.STORAGE,
        target_setpoint_watts=1000,
        effective_delta_watts=1000,
        storage_effective_delta_watts=1000,
        global_effective_delta_watts=1000,
    )
    samples = [record(model, 0, 1000, note="Queued request suppresses duplicates.")]
    model.classify_failure(request.seq, RequestState.FAILED_NO_EFFECT)
    samples.append(record(model, 100, 1000, note="Failure resets requested target to safe."))
    return simple_scenario(
        "Failed queued request does not suppress retry",
        "No-effect failure is removed from requested target calculation.",
        samples,
        ["After failure, requested battery setpoint returns to 0 W."],
        [Event(0, "queue 1000 W"), Event(100, "failed no effect")],
    )


def scenario_queued_supersession() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0)]
    )
    first = model.queue_request(
        "bat",
        RequestDomain.STORAGE,
        target_setpoint_watts=500,
        effective_delta_watts=500,
        storage_effective_delta_watts=500,
        global_effective_delta_watts=500,
    )
    samples = [record(model, 0, 800, note="First queued target is 500 W.")]
    second = model.queue_request(
        "bat",
        RequestDomain.STORAGE,
        target_setpoint_watts=800,
        effective_delta_watts=800,
        storage_effective_delta_watts=800,
        global_effective_delta_watts=800,
    )
    samples.append(record(model, 100, 800, note="Second queued request supersedes first."))
    return simple_scenario(
        "Queued supersession before send",
        "Queued-only requests may be coalesced because they are not physical effects.",
        samples,
        [
            f"First state: {first.state.value}.",
            f"Second state: {second.state.value}.",
        ],
        [Event(0, "queue 500 W"), Event(100, "queue 800 W")],
    )


def scenario_latest_effect_advances_safe_setpoint() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0)]
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=0,
        target_setpoint_watts=500,
        effective_delta_watts=500,
        storage_effective_delta_watts=500,
        global_effective_delta_watts=500,
        sent_millis=1000,
        latest_effect_millis=4000,
    )
    samples = [record(model, 1500, 500, note="Request is still pending.")]
    model.advance_ledger(4000)
    samples.append(
        record(
            model,
            4000,
            500,
            note="Request settled; remaining import is a new disturbance.",
        )
    )
    return simple_scenario(
        "Deterministic settlement at latestEffect",
        "A deterministic request leaves the pending set at latestEffect.",
        samples,
        ["safeSetpoint advances to 500 W; the residual grid error is no longer hidden."],
        [Event(1000, "battery +500 W"), Event(4000, "latestEffect")],
    )


def scenario_ambiguous_send_failure() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0)]
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=0,
        target_setpoint_watts=500,
        effective_delta_watts=500,
        storage_effective_delta_watts=500,
        global_effective_delta_watts=500,
        state=RequestState.FAILED_AMBIGUOUS,
        sent_millis=1000,
        latest_effect_millis=4000,
    )
    samples = [record(model, 1500, 500, note="Command may or may not have arrived.")]
    return simple_scenario(
        "Ambiguous send failure remains uncertain",
        "Ambiguous failure stays inside the forecast corridor.",
        samples,
        ["Pending count remains nonzero; effect is not silently discarded."],
        [Event(1000, "send timeout, effect possible")],
    )


def scenario_solar_no_headroom() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=300)]
    )
    model.append_physical_request(
        "solar",
        RequestDomain.STORAGE,
        base_setpoint_watts=300,
        target_setpoint_watts=800,
        effective_delta_watts=500,
        storage_effective_delta_watts=500,
        global_effective_delta_watts=500,
        effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
        sent_millis=1000,
        latest_effect_millis=4000,
        capacity_full_output_possible=False,
    )
    samples = [record(model, 1500, 500, note="Telemetry says no solar headroom.")]
    return simple_scenario(
        "Solar increase without headroom",
        "Full-output branch is removed when fresh telemetry proves no headroom.",
        samples,
        ["Storage correction is not blocked by an impossible solar increase."],
        [Event(1000, "raise solar limit 300 -> 800 W")],
    )


def scenario_solar_reduction() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=800)]
    )
    model.append_physical_request(
        "solar",
        RequestDomain.STORAGE,
        base_setpoint_watts=800,
        target_setpoint_watts=300,
        effective_delta_watts=-500,
        storage_effective_delta_watts=-500,
        global_effective_delta_watts=-500,
        effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
        sent_millis=1000,
        latest_effect_millis=4000,
    )
    samples = [record(model, 1500, -500, note="Reduction is deterministic cap behavior.")]
    return simple_scenario(
        "Solar reduction is deterministic",
        "Lowering a limit does not get a no-irradiance no-effect branch.",
        samples,
        ["Forecast treats the cap as deterministic pending effect."],
        [Event(1000, "solar limit 800 -> 300 W")],
    )


def scenario_charger_startup() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("charger", ActuatorKind.CHARGER, safe_setpoint_watts=0)]
    )
    model.append_physical_request(
        "charger",
        RequestDomain.STORAGE,
        base_setpoint_watts=0,
        target_setpoint_watts=1200,
        effective_delta_watts=-1200,
        storage_effective_delta_watts=-1200,
        global_effective_delta_watts=-1200,
        transition_kind=TransitionKind.STARTUP,
        sent_millis=1000,
        not_before_effect_millis=8000,
        latest_effect_millis=15000,
    )
    samples = [
        record(model, 5000, 0, note="Before startup can affect meter."),
        record(model, 9000, 1200, note="Startup window open; charger may be visible."),
    ]
    return simple_scenario(
        "Grid charger startup latency",
        "Starting the charger uses a long timing profile.",
        samples,
        ["The charger effect cannot be marked visible before notBeforeEffectMillis."],
        [Event(1000, "start charger"), Event(8000, "startup notBefore")],
    )


def scenario_solar_startup() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=0)]
    )
    model.append_physical_request(
        "solar",
        RequestDomain.STORAGE,
        base_setpoint_watts=0,
        target_setpoint_watts=500,
        effective_delta_watts=500,
        storage_effective_delta_watts=500,
        global_effective_delta_watts=500,
        effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
        transition_kind=TransitionKind.STARTUP,
        sent_millis=1000,
        not_before_effect_millis=8000,
        latest_effect_millis=15000,
        capacity_full_output_possible=True,
    )
    samples = [
        record(model, 5000, 500, note="Standby wake-up not yet expected."),
        record(model, 9000, 500, note="Solar may wake, but irradiance remains uncertain."),
    ]
    model.advance_ledger(15000)
    samples.append(record(model, 15000, 500, note="Startup probe expired without effect."))
    return simple_scenario(
        "Solar startup from standby",
        "Solar wake-up has startup latency and capacity uncertainty.",
        samples,
        ["After the startup timeout, storage can correct residual import."],
        [Event(1000, "wake solar"), Event(8000, "startup notBefore")],
    )


def scenario_solar_probe_timeout() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=300)]
    )
    model.append_physical_request(
        "solar",
        RequestDomain.STORAGE,
        base_setpoint_watts=300,
        target_setpoint_watts=800,
        effective_delta_watts=500,
        storage_effective_delta_watts=500,
        global_effective_delta_watts=500,
        effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
        sent_millis=1000,
        latest_effect_millis=2000,
        capacity_full_output_possible=True,
    )
    samples = [record(model, 1500, 500, note="Short solar probe still pending.")]
    model.advance_ledger(2000)
    samples.append(record(model, 2000, 500, note="Probe expired; residual import remains."))
    return simple_scenario(
        "Solar probe expires without effect",
        "Storage waits only for the configured short probe window.",
        samples,
        ["Before timeout HOLD is allowed; after timeout storage residual is actionable."],
        [Event(1000, "solar probe +500 W"), Event(2000, "probe latestEffect")],
    )


def scenario_grid_only_solar_pending_storage_forecast() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=300)]
    )
    model.append_physical_request(
        "solar",
        RequestDomain.GRID_ONLY,
        base_setpoint_watts=300,
        target_setpoint_watts=600,
        effective_delta_watts=300,
        storage_effective_delta_watts=0,
        global_effective_delta_watts=300,
        effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
        sent_millis=1000,
        latest_effect_millis=4000,
    )
    samples = [
        record(
            model,
            1500,
            0,
            storage_target_w=0,
            global_target_w=-300,
            note="Storage has no storage-owned delta but tracks physical uncertainty.",
        )
    ]
    return simple_scenario(
        "Grid-only solar pending in storage forecast",
        "Pure solar-only pending does not become storage output, but it may be visible in the raw meter.",
        samples,
        [
            "Storage forecast keeps enough uncertainty to avoid reacting to unproven solar-only export.",
            "Global forecast contains the solar-only branch.",
        ],
        [Event(1000, "grid-only solar +300 W")],
    )


def scenario_grid_only_solar_visible_unproven() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=300)]
    )
    model.append_physical_request(
        "solar",
        RequestDomain.GRID_ONLY,
        base_setpoint_watts=300,
        target_setpoint_watts=600,
        effective_delta_watts=300,
        storage_effective_delta_watts=0,
        global_effective_delta_watts=300,
        effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
        sent_millis=1000,
        latest_effect_millis=4000,
    )
    samples = [
        record(
            model,
            1500,
            -300,
            storage_target_w=0,
            global_target_w=-300,
            note="Physical export may be unproven solar-only effect.",
        )
    ]
    return simple_scenario(
        "Visible but unproven grid-only solar",
        "Storage forecast includes the possibility that pending solar-only export is already visible.",
        samples,
        ["Storage target remains inside corridor; no battery/charger backoff."],
        [Event(1000, "solar-only request +300 W")],
    )


def scenario_mixed_solar() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=300)]
    )
    model.append_physical_request(
        "solar",
        RequestDomain.MIXED,
        base_setpoint_watts=300,
        target_setpoint_watts=800,
        effective_delta_watts=500,
        storage_effective_delta_watts=300,
        global_effective_delta_watts=500,
        sent_millis=1000,
        latest_effect_millis=4000,
    )
    allocation = allocate_mixed_solar(300, 200, 400)
    samples = [record(model, 1500, 500, note="Storage sees 300 W, global sees 500 W.")]
    return simple_scenario(
        "Mixed solar request",
        "Storage and global domains use different parts of the same solar command.",
        samples,
        [
            "With 400 W plausible solar: 300 W storage, 100 W solar-only.",
            f"Solar-only shortfall is {allocation.solar_only_shortfall_watts:.0f} W.",
        ],
        [Event(1000, "mixed solar +500 W")],
    )


def scenario_mixed_solar_allocation() -> Scenario:
    allocation = allocate_mixed_solar(300, 200, 400)
    model = PredictiveControlModel()
    samples = [
        record(
            model,
            0,
            -400,
            storage_target_w=0,
            global_target_w=-500,
            proven_solar_only_effective_w=allocation.solar_only_effective_watts,
            note="400 W actual solar is allocated storage-first.",
        )
    ]
    return simple_scenario(
        "Mixed solar allocation storage-first",
        "Actual or plausible mixed solar output is assigned to storage before solar-only export.",
        samples,
        [
            f"Storage contribution: {allocation.storage_effective_watts:.0f} W.",
            f"Solar-only contribution: {allocation.solar_only_effective_watts:.0f} W.",
            f"Solar-only shortfall: {allocation.solar_only_shortfall_watts:.0f} W.",
        ],
    )


def scenario_charger_sign() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("charger", ActuatorKind.CHARGER, safe_setpoint_watts=300)]
    )
    request = model.append_physical_request(
        "charger",
        RequestDomain.STORAGE,
        base_setpoint_watts=300,
        target_setpoint_watts=500,
        effective_delta_watts=-200,
        storage_effective_delta_watts=-200,
        global_effective_delta_watts=-200,
        sent_millis=1000,
        latest_effect_millis=4000,
    )
    samples = [record(model, 1500, 0, note="Increasing charger input raises meter.")]
    return simple_scenario(
        "Grid charger sign convention",
        "Charger setpoint increase reduces effective output.",
        samples,
        [
            f"setpointDelta = {request.setpoint_delta_watts:.0f} W.",
            f"effectiveDelta = {request.effective_delta_watts:.0f} W.",
            f"meterDelta = {request.meter_delta_watts:.0f} W.",
        ],
        [Event(1000, "charger 300 -> 500 W")],
    )


def scenario_inverter_minimum() -> Scenario:
    battery = ActuatorState(
        "bat",
        ActuatorKind.BATTERY,
        safe_setpoint_watts=0,
        capabilities=ActuatorCapabilities(min_active_setpoint_watts=150),
    )
    model = PredictiveControlModel([battery])
    legal_battery = battery.legalize_setpoint(80)
    battery_delta = battery.effective_output_for_setpoint(legal_battery)
    samples = [record(model, 0, 0, note="Small positive inverter request is coerced.")]
    return simple_scenario(
        "Inverter minimum active power",
        "Positive inverter setpoints below the stable minimum are not emitted as-is.",
        samples,
        [
            f"Battery 80 W desired -> {legal_battery:.0f} W legal.",
            f"Effective output delta becomes {battery_delta:.0f} W.",
        ],
    )


def scenario_capabilities() -> Scenario:
    charger = ActuatorState(
        "charger",
        ActuatorKind.CHARGER,
        safe_setpoint_watts=0,
        capabilities=ActuatorCapabilities(
            min_active_setpoint_watts=300,
            max_setpoint_watts=1200,
            self_consumption_watts=50,
        ),
    )
    battery = ActuatorState(
        "bat",
        ActuatorKind.BATTERY,
        safe_setpoint_watts=0,
        capabilities=ActuatorCapabilities(min_active_setpoint_watts=150),
    )
    model = PredictiveControlModel([charger, battery])
    legal_charger = charger.legalize_setpoint(100)
    charger_delta = charger.effective_output_for_setpoint(legal_charger)
    legal_battery = battery.legalize_setpoint(80)
    battery_delta = battery.effective_output_for_setpoint(legal_battery)
    samples = [record(model, 0, 0, note="Capability coercion before ledger append.")]
    return simple_scenario(
        "Actuator minimums and charger self-consumption",
        "Desired corrections are converted to legal physical setpoints before deltas are recorded.",
        samples,
        [
            f"Charger 100 W desired -> {legal_charger:.0f} W legal.",
            f"Charger effective delta includes self-consumption: {charger_delta:.0f} W.",
            f"Battery 80 W desired -> {legal_battery:.0f} W legal, {battery_delta:.0f} W effective.",
        ],
    )


def scenario_parallel_actuators() -> Scenario:
    model = PredictiveControlModel(
        [
            ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0),
            ActuatorState("charger", ActuatorKind.CHARGER, safe_setpoint_watts=300),
        ]
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=0,
        target_setpoint_watts=500,
        effective_delta_watts=500,
        storage_effective_delta_watts=500,
        global_effective_delta_watts=500,
        sent_millis=1000,
        latest_effect_millis=4000,
    )
    model.append_physical_request(
        "charger",
        RequestDomain.STORAGE,
        base_setpoint_watts=300,
        target_setpoint_watts=0,
        effective_delta_watts=300,
        storage_effective_delta_watts=300,
        global_effective_delta_watts=300,
        sent_millis=1000,
        latest_effect_millis=4000,
    )
    samples = [record(model, 1500, 800, note="Independent actuator branches combine.")]
    return simple_scenario(
        "Parallel independent actuators",
        "Battery and charger pending effects are combined across actuators.",
        samples,
        ["Possible future meter range is 0 W to 800 W."],
        [Event(1000, "battery +500 W, charger input -300 W")],
    )


def scenario_dynamic_learning() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0)]
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=0,
        target_setpoint_watts=500,
        effective_delta_watts=500,
        storage_effective_delta_watts=500,
        global_effective_delta_watts=500,
        sent_millis=1000,
        latest_effect_millis=4000,
    )
    sample = record(model, 1500, 500, note="Storage pending blocks target learning.")
    allowed = dynamic_target_learning_allowed(
        model.forecast(
            MeterSnapshot(500, 1500),
            ForecastDomain.STORAGE,
        )
    )
    return simple_scenario(
        "Dynamic battery target freeze",
        "Raw meter samples are not learned while storage-relevant requests are pending.",
        [sample],
        [f"dynamic target learning allowed: {allowed}."],
        [Event(1000, "battery pending")],
    )


def scenario_hysteresis() -> Scenario:
    model = PredictiveControlModel()
    sample = record(model, 0, 150, storage_target_w=90, note="Target sits on lower hysteresis edge.")
    decision_edge = decide_correction(
        90,
        model.forecast(MeterSnapshot(150, 0), ForecastDomain.STORAGE),
        hysteresis_watts=60,
    )
    decision_outside = decide_correction(
        89,
        model.forecast(MeterSnapshot(150, 0), ForecastDomain.STORAGE),
        hysteresis_watts=60,
    )
    return simple_scenario(
        "Hysteresis boundaries",
        "The no-command band extends the corridor by the configured hysteresis.",
        [sample],
        [
            f"At edge: {decision_edge.action.value}.",
            f"One watt outside: {decision_outside.action.value}.",
        ],
    )


def scenario_target_ownership() -> Scenario:
    model = PredictiveControlModel()
    global_target = global_target_from_solar_offset(0, 300)
    clamped = validate_target_ownership(0, 100, clamp=True)
    sample = record(model, 0, 0, storage_target_w=0, global_target_w=global_target)
    return simple_scenario(
        "Target ownership validation",
        "The global target must not be above the storage target.",
        [sample],
        [
            f"0 W storage with 300 W solar-only offset -> global target {global_target:.0f} W.",
            f"Invalid +100 W global target clamps to {clamped:.0f} W when clamp mode is used.",
        ],
    )


def scenario_safety_override() -> Scenario:
    model = PredictiveControlModel(
        [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=500)]
    )
    model.append_physical_request(
        "bat",
        RequestDomain.STORAGE,
        base_setpoint_watts=500,
        target_setpoint_watts=0,
        effective_delta_watts=-500,
        storage_effective_delta_watts=-500,
        global_effective_delta_watts=-500,
        cause=RequestCause.SAFETY,
        transition_kind=TransitionKind.STANDBY,
        sent_millis=1000,
        latest_effect_millis=6000,
    )
    samples = [record(model, 1500, -500, note="Safety standby is still physical pending state.")]
    return simple_scenario(
        "Safety override ledger entry",
        "Safety commands still enter the ledger so normal control sees pending effects.",
        samples,
        ["Cause is SAFETY; transition is STANDBY."],
        [Event(1000, "battery standby safety command")],
    )


def build_scenarios() -> List[Scenario]:
    return [
        scenario_solar_only_offset(),
        scenario_battery_pending_visible_early(),
        scenario_battery_pending_not_visible(),
        scenario_not_before_and_soft_timing(),
        scenario_soft_timing(),
        scenario_ordered_chain(),
        scenario_ambiguous_failure_chain(),
        scenario_later_request_earlier_latest(),
        scenario_failed_queued_retry(),
        scenario_queued_supersession(),
        scenario_latest_effect_advances_safe_setpoint(),
        scenario_ambiguous_send_failure(),
        scenario_solar_no_headroom(),
        scenario_solar_reduction(),
        scenario_charger_startup(),
        scenario_solar_startup(),
        scenario_solar_probe_timeout(),
        scenario_grid_only_solar_pending_storage_forecast(),
        scenario_grid_only_solar_visible_unproven(),
        scenario_mixed_solar(),
        scenario_mixed_solar_allocation(),
        scenario_charger_sign(),
        scenario_inverter_minimum(),
        scenario_capabilities(),
        scenario_parallel_actuators(),
        scenario_dynamic_learning(),
        scenario_hysteresis(),
        scenario_target_ownership(),
        scenario_safety_override(),
    ]


SERIES: Dict[str, Dict[str, object]] = {
    "physical_grid_w": {"label": "Grid real", "color": "#d93a4a", "dash": ""},
    "storage_target_w": {"label": "Storage target", "color": "#6f42c1", "dash": "5 4"},
    "global_target_w": {"label": "Global target", "color": "#6c757d", "dash": "5 4"},
    "storage_min_w": {"label": "Storage corridor min", "color": "#198754", "dash": ""},
    "storage_max_w": {"label": "Storage corridor max", "color": "#198754", "dash": "4 4"},
    "global_min_w": {"label": "Global corridor min", "color": "#0dcaf0", "dash": ""},
    "global_max_w": {"label": "Global corridor max", "color": "#0dcaf0", "dash": "4 4"},
    "battery_requested_w": {"label": "Battery requested", "color": "#fd7e14", "dash": ""},
    "charger_requested_w": {"label": "Charger requested", "color": "#20c997", "dash": ""},
    "solar_requested_w": {"label": "Solar requested", "color": "#ffc107", "dash": ""},
}


def all_numeric_values(samples: Sequence[TimelineSample]) -> List[float]:
    values: List[float] = []
    for sample in samples:
        for key in SERIES:
            value = getattr(sample, key)
            if value is not None:
                values.append(float(value))
    if not values:
        return [0.0]
    return values


def render_svg(scenario: Scenario) -> str:
    width = 1120
    height = 360
    left = 70
    right = 20
    top = 24
    bottom = 58
    plot_w = width - left - right
    plot_h = height - top - bottom

    samples = scenario.samples
    min_t = min(sample.t_ms for sample in samples)
    max_t = max(sample.t_ms for sample in samples)
    if min_t == max_t:
        min_t -= 1000
        max_t += 1000

    values = all_numeric_values(samples)
    min_v = min(values)
    max_v = max(values)
    if min_v == max_v:
        min_v -= 100
        max_v += 100
    padding = max(50.0, (max_v - min_v) * 0.12)
    min_v -= padding
    max_v += padding

    def x_for(t_ms: int) -> float:
        return left + ((t_ms - min_t) / (max_t - min_t)) * plot_w

    def y_for(value: float) -> float:
        return top + ((max_v - value) / (max_v - min_v)) * plot_h

    def points_for(key: str) -> List[str]:
        points: List[str] = []
        for sample in samples:
            value = getattr(sample, key)
            if value is not None:
                points.append(f"{x_for(sample.t_ms):.1f},{y_for(float(value)):.1f}")
        return points

    lines: List[str] = [
        f'<svg class="chart" viewBox="0 0 {width} {height}" role="img">',
        f'<rect x="0" y="0" width="{width}" height="{height}" fill="#fff"/>',
    ]

    for i in range(5):
        value = min_v + (max_v - min_v) * i / 4
        y = y_for(value)
        lines.append(
            f'<line x1="{left}" y1="{y:.1f}" x2="{width - right}" y2="{y:.1f}" '
            'stroke="#e5e7eb" stroke-width="1"/>'
        )
        lines.append(
            f'<text x="{left - 8}" y="{y + 4:.1f}" text-anchor="end" '
            f'class="axis">{value:.0f} W</text>'
        )

    zero_y = y_for(0.0)
    if top <= zero_y <= top + plot_h:
        lines.append(
            f'<line x1="{left}" y1="{zero_y:.1f}" x2="{width - right}" '
            'y2="{zero_y:.1f}" stroke="#9ca3af" stroke-width="1.5"/>'
        )

    for key, style in SERIES.items():
        points = points_for(key)
        if not points:
            continue
        dash = style["dash"]
        dash_attr = f' stroke-dasharray="{dash}"' if dash else ""
        if len(points) == 1:
            x, y = points[0].split(",")
            lines.append(
                f'<circle cx="{x}" cy="{y}" r="4" fill="{style["color"]}">'
                f'<title>{escape(str(style["label"]))}</title></circle>'
            )
        else:
            lines.append(
                f'<polyline fill="none" stroke="{style["color"]}" stroke-width="2.2"'
                f'{dash_attr} points="{" ".join(points)}">'
                f'<title>{escape(str(style["label"]))}</title></polyline>'
            )

    for event in scenario.events:
        x = x_for(event.t_ms)
        lines.append(
            f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{top + plot_h}" '
            'stroke="#111827" stroke-width="1" stroke-dasharray="3 3"/>'
        )
        lines.append(
            f'<text x="{x + 4:.1f}" y="{top + 14}" class="event">'
            f'{escape(event.label)}</text>'
        )

    for sample in samples:
        x = x_for(sample.t_ms)
        lines.append(
            f'<text x="{x:.1f}" y="{height - 20}" text-anchor="middle" class="axis">'
            f'{sample.t_ms / 1000:.1f}s</text>'
        )

    lines.append("</svg>")
    return "\n".join(lines)


def render_sample_table(samples: Sequence[TimelineSample]) -> str:
    rows = []
    for sample in samples:
        rows.append(
            "<tr>"
            f"<td>{sample.t_ms / 1000:.1f}s</td>"
            f"<td>{format_w(sample.physical_grid_w)}</td>"
            f"<td>{format_range(sample.storage_min_w, sample.storage_max_w)}</td>"
            f"<td>{format_range(sample.global_min_w, sample.global_max_w)}</td>"
            f"<td>{format_w(sample.battery_requested_w)}</td>"
            f"<td>{format_w(sample.charger_requested_w)}</td>"
            f"<td>{format_w(sample.solar_requested_w)}</td>"
            f"<td>{escape(sample.storage_decision)}</td>"
            f"<td>{escape(sample.note)}</td>"
            "</tr>"
        )
    return (
        '<table><thead><tr><th>Time</th><th>Grid real</th><th>Storage forecast</th>'
        '<th>Global forecast</th><th>Battery req</th><th>Charger req</th>'
        '<th>Solar req</th><th>Storage decision</th><th>Note</th></tr></thead>'
        f'<tbody>{"".join(rows)}</tbody></table>'
    )


def format_w(value: Optional[float]) -> str:
    if value is None:
        return "-"
    return f"{value:.0f} W"


def format_range(min_w: Optional[float], max_w: Optional[float]) -> str:
    if min_w is None or max_w is None:
        return "-"
    if min_w == max_w:
        return f"{min_w:.0f} W"
    return f"{min_w:.0f} .. {max_w:.0f} W"


def render_legend() -> str:
    items = []
    for style in SERIES.values():
        dash = " dashed" if style["dash"] else ""
        items.append(
            f'<span class="legend-item"><span class="swatch{dash}" '
            f'style="--c:{style["color"]}"></span>{escape(str(style["label"]))}</span>'
        )
    return f'<div class="legend">{"".join(items)}</div>'


def render_scenario(scenario: Scenario, index: int) -> str:
    assertions = "".join(f"<li>{escape(item)}</li>" for item in scenario.assertions)
    return (
        f'<section class="scenario"><h2>{index}. {escape(scenario.title)}</h2>'
        f'<p>{escape(scenario.summary)}</p>'
        f"{render_svg(scenario)}"
        f"{render_sample_table(scenario.samples)}"
        f"<ul>{assertions}</ul>"
        "</section>"
    )


def render_report(scenarios: Sequence[Scenario]) -> str:
    sections = "\n".join(
        render_scenario(scenario, index)
        for index, scenario in enumerate(scenarios, start=1)
    )
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>DPL Predictive Control Scenario Report</title>
<style>
body {{
  color: #1f2933;
  font: 14px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  margin: 0;
  background: #f6f7f9;
}}
main {{
  max-width: 1180px;
  margin: 0 auto;
  padding: 28px 20px 48px;
}}
h1 {{
  font-size: 28px;
  margin: 0 0 8px;
}}
h2 {{
  font-size: 20px;
  margin: 0 0 6px;
}}
p {{
  margin: 0 0 14px;
}}
.scenario {{
  background: #fff;
  border: 1px solid #d9dde3;
  border-radius: 6px;
  margin: 18px 0;
  padding: 18px;
}}
.chart {{
  width: 100%;
  height: auto;
  border: 1px solid #e5e7eb;
  border-radius: 4px;
  background: #fff;
}}
.axis {{
  fill: #667085;
  font-size: 12px;
}}
.event {{
  fill: #111827;
  font-size: 12px;
}}
.legend {{
  display: flex;
  flex-wrap: wrap;
  gap: 10px 16px;
  margin: 18px 0;
}}
.legend-item {{
  align-items: center;
  display: inline-flex;
  gap: 6px;
  white-space: nowrap;
}}
.swatch {{
  border-top: 3px solid var(--c);
  display: inline-block;
  width: 24px;
}}
.swatch.dashed {{
  border-top-style: dashed;
}}
table {{
  border-collapse: collapse;
  font-size: 12px;
  margin-top: 12px;
  width: 100%;
}}
th, td {{
  border-bottom: 1px solid #e5e7eb;
  padding: 6px 8px;
  text-align: left;
  vertical-align: top;
}}
th {{
  color: #344054;
  background: #f8fafc;
}}
ul {{
  margin: 12px 0 0 20px;
  padding: 0;
}}
</style>
</head>
<body>
<main>
<h1>DPL Predictive Control Scenario Report</h1>
<p>Generated from the Python reference model in <code>tools/dpl_sim</code>.
The charts show synthetic timeline samples, not measurements from hardware.</p>
{render_legend()}
{sections}
</main>
</body>
</html>
"""


def main(argv: Sequence[str]) -> int:
    output_path = Path(argv[1]) if len(argv) > 1 else REPORT_PATH
    output_path.parent.mkdir(parents=True, exist_ok=True)
    scenarios = build_scenarios()
    output_path.write_text(render_report(scenarios), encoding="utf-8")
    print(f"Wrote {output_path}")
    print(f"Scenarios: {len(scenarios)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
