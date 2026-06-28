#!/usr/bin/env python3
"""Generate a 10-minute closed-loop household regulation report."""

from __future__ import annotations

from dataclasses import dataclass
from html import escape
from pathlib import Path
import math
import random
import sys
from typing import Callable, Dict, List, Optional, Sequence

from predictive_control import (
    ActuatorCapabilities,
    ActuatorKind,
    ActuatorState,
    CorrectionAction,
    EffectKind,
    ForecastDomain,
    MeterSnapshot,
    PredictiveControlModel,
    RequestDomain,
    TransitionKind,
    decide_correction,
    proven_solar_only_effective_output,
)


REPORT_PATH = Path(__file__).with_name("household_regulation_report.html")

STORAGE_TARGET_W = 0.0
GLOBAL_TARGET_W = -150.0
MAX_SOLAR_LIMIT_W = 1200.0
MAX_BATTERY_OUTPUT_W = 800.0
MIN_BATTERY_OUTPUT_W = 150.0
INITIAL_CHARGER_SETPOINT_W = 500.0
MIN_CHARGER_INPUT_W = 250.0
MAX_CHARGER_INPUT_W = 900.0
CHARGER_SELF_CONSUMPTION_W = 40.0
CONTROL_INTERVAL_MS = 2000
SIM_DURATION_MS = 10 * 60 * 1000
DT_MS = 1000
ASSERTION_TOLERANCE_W = 5.0


@dataclass
class ScheduledCommand:
    actuator_key: str
    target_watts: float
    apply_millis: int
    label: str


@dataclass
class Event:
    t_ms: int
    label: str


@dataclass
class Row:
    t_ms: int
    load_w: float
    solar_capacity_w: float
    solar_limit_w: float
    solar_output_w: float
    battery_setpoint_w: float
    battery_requested_w: float
    battery_output_w: float
    charger_setpoint_w: float
    charger_input_w: float
    grid_w: float
    proven_solar_only_w: float
    storage_min_w: float
    storage_max_w: float
    global_min_w: float
    global_max_w: float
    storage_residual_w: float
    global_residual_w: float
    pending_count: int
    storage_decision: str
    global_decision: str
    note: str = ""


class HouseholdSimulation:
    def __init__(self) -> None:
        self.random = random.Random(17)
        self.model = PredictiveControlModel(
            [
                ActuatorState(
                    "solar",
                    ActuatorKind.SOLAR,
                    safe_setpoint_watts=0.0,
                    capabilities=ActuatorCapabilities(max_setpoint_watts=MAX_SOLAR_LIMIT_W),
                ),
                ActuatorState(
                    "bat",
                    ActuatorKind.BATTERY,
                    safe_setpoint_watts=0.0,
                    capabilities=ActuatorCapabilities(
                        min_active_setpoint_watts=MIN_BATTERY_OUTPUT_W,
                        max_setpoint_watts=MAX_BATTERY_OUTPUT_W,
                    ),
                ),
                ActuatorState(
                    "charger",
                    ActuatorKind.CHARGER,
                    safe_setpoint_watts=INITIAL_CHARGER_SETPOINT_W,
                    capabilities=ActuatorCapabilities(
                        min_active_setpoint_watts=MIN_CHARGER_INPUT_W,
                        max_setpoint_watts=MAX_CHARGER_INPUT_W,
                        self_consumption_watts=CHARGER_SELF_CONSUMPTION_W,
                    ),
                ),
            ]
        )
        self.actual_solar_limit_w = 0.0
        self.actual_battery_setpoint_w = 0.0
        self.actual_charger_setpoint_w = INITIAL_CHARGER_SETPOINT_W
        self.scheduled: List[ScheduledCommand] = []
        self.events: List[Event] = []
        self.rows: List[Row] = []
        self.last_control_ms = -CONTROL_INTERVAL_MS
        self.last_solar_probe_ms = -60_000
        self.last_battery_command_ms = -60_000

    def run(self) -> None:
        for t_ms in range(0, SIM_DURATION_MS + DT_MS, DT_MS):
            self._apply_due_commands(t_ms)

            load_w = external_load_w(t_ms)
            solar_capacity_w = solar_capacity_watts(t_ms)
            solar_output_w = min(self.actual_solar_limit_w, solar_capacity_w)
            battery_output_w = self.actual_battery_setpoint_w
            charger_input_w = charger_grid_input_w(self.actual_charger_setpoint_w)
            grid_w = load_w + charger_input_w - solar_output_w - battery_output_w
            charger_headroom_w = max(
                0.0,
                MAX_CHARGER_INPUT_W - self.actual_charger_setpoint_w,
            )
            proven_solar_only_w = proven_solar_only_effective_output(
                physical_grid_watts=grid_w,
                storage_target_watts=STORAGE_TARGET_W,
                solar_only_export_offset_watts=STORAGE_TARGET_W - GLOBAL_TARGET_W,
                storage_absorption_headroom_watts=charger_headroom_w,
                solar_effective_output_watts=solar_output_w,
            )

            snapshot = MeterSnapshot(
                physical_grid_watts=grid_w,
                meter_sample_millis=t_ms,
                proven_solar_only_effective_watts=proven_solar_only_w,
            )
            self.model.advance_ledger(t_ms)
            self.model.prune_inactive_requests()
            storage_forecast = self.model.forecast(snapshot, ForecastDomain.STORAGE)
            global_forecast = self.model.forecast(snapshot, ForecastDomain.GLOBAL)
            storage_decision = decide_correction(
                STORAGE_TARGET_W,
                storage_forecast,
            )
            global_decision = decide_correction(
                GLOBAL_TARGET_W,
                global_forecast,
            )

            note = ""
            if t_ms - self.last_control_ms >= CONTROL_INTERVAL_MS:
                note = self._control(
                    t_ms,
                    load_w,
                    solar_capacity_w,
                    storage_decision,
                    global_decision,
                    storage_forecast.grid_min_watts,
                    storage_forecast.grid_max_watts,
                )
                self.last_control_ms = t_ms

            if t_ms % 5000 == 0 or note:
                self.rows.append(
                    Row(
                        t_ms=t_ms,
                        load_w=load_w,
                        solar_capacity_w=solar_capacity_w,
                        solar_limit_w=self.actual_solar_limit_w,
                        solar_output_w=solar_output_w,
                        battery_setpoint_w=self.actual_battery_setpoint_w,
                        battery_requested_w=(
                            self.model.actuators["bat"].requested_setpoint_watts
                        ),
                        battery_output_w=battery_output_w,
                        charger_setpoint_w=self.actual_charger_setpoint_w,
                        charger_input_w=charger_input_w,
                        grid_w=grid_w,
                        proven_solar_only_w=proven_solar_only_w,
                        storage_min_w=storage_forecast.grid_min_watts,
                        storage_max_w=storage_forecast.grid_max_watts,
                        global_min_w=global_forecast.grid_min_watts,
                        global_max_w=global_forecast.grid_max_watts,
                        storage_residual_w=storage_decision.residual_effective_watts,
                        global_residual_w=global_decision.residual_effective_watts,
                        pending_count=storage_forecast.pending_count,
                        storage_decision=storage_decision.action.value,
                        global_decision=global_decision.action.value,
                        note=note,
                    )
                )

    def _control(
        self,
        t_ms: int,
        load_w: float,
        solar_capacity_w: float,
        storage_decision,
        global_decision,
        storage_min_w: float,
        storage_max_w: float,
    ) -> str:
        if storage_decision.action == CorrectionAction.MORE_EFFECTIVE_OUTPUT:
            residual = max(0.0, storage_decision.residual_effective_watts)
            if self._solar_has_useful_headroom(solar_capacity_w):
                target = min(
                    MAX_SOLAR_LIMIT_W,
                    self.model.actuators["solar"].requested_setpoint_watts + residual,
                )
                if target > self.model.actuators["solar"].requested_setpoint_watts + 1:
                    self._command_solar(
                        t_ms,
                        target,
                        RequestDomain.STORAGE,
                        storage_effective_delta=True,
                        capacity_full_output_possible=True,
                    )
                    return f"solar storage increase to {target:.0f} W"

            current_charger = self.model.actuators["charger"].requested_setpoint_watts
            if current_charger > 0:
                target = reduced_charger_target_w(current_charger, residual)
                if target is not None:
                    self._command_charger(t_ms, target)
                    return f"charger reduce to {target:.0f} W"

            if self._charger_active_or_pending():
                return "wait charger off before battery"

            if (
                self.model.actuators["solar"].requested_setpoint_watts
                < MAX_SOLAR_LIMIT_W - 1
            ):
                self._command_solar(
                    t_ms,
                    MAX_SOLAR_LIMIT_W,
                    RequestDomain.STORAGE,
                    storage_effective_delta=False,
                    capacity_full_output_possible=False,
                )
                return f"solar uncap before battery to {MAX_SOLAR_LIMIT_W:.0f} W"

            target = min(
                MAX_BATTERY_OUTPUT_W,
                self.model.actuators["bat"].requested_setpoint_watts + residual,
            )
            target = legal_battery_setpoint_w(
                self.model.actuators["bat"].requested_setpoint_watts, target
            )
            if target > self.model.actuators["bat"].requested_setpoint_watts + 1:
                self._command_battery(t_ms, target)
                return f"battery increase to {target:.0f} W"

        if storage_decision.action == CorrectionAction.LESS_EFFECTIVE_OUTPUT:
            residual = abs(storage_decision.residual_effective_watts)
            current_bat = self.model.actuators["bat"].requested_setpoint_watts
            if current_bat > 0:
                target = max(0.0, current_bat - residual)
                target = legal_battery_setpoint_w(current_bat, target)
                if target < current_bat - 1:
                    self._command_battery(t_ms, target)
                    return f"battery reduce to {target:.0f} W"

            if self._battery_active_or_pending():
                return "wait battery off before charger"

            current_charger = self.model.actuators["charger"].requested_setpoint_watts
            target = min(MAX_CHARGER_INPUT_W, current_charger + residual)
            if 0 < target < MIN_CHARGER_INPUT_W:
                target = MIN_CHARGER_INPUT_W
            if target > current_charger + 1:
                self._command_charger(t_ms, target)
                return f"charger increase to {target:.0f} W"

            current_solar = self.model.actuators["solar"].requested_setpoint_watts
            if current_solar > 0:
                target = max(0.0, current_solar - residual)
                self._command_solar(
                    t_ms,
                    target,
                    RequestDomain.STORAGE,
                    storage_effective_delta=True,
                    capacity_full_output_possible=True,
                )
                return f"solar storage reduce to {target:.0f} W"

        if (
            storage_min_w <= STORAGE_TARGET_W <= storage_max_w
            and global_decision.action == CorrectionAction.MORE_EFFECTIVE_OUTPUT
            and self._solar_has_useful_headroom(solar_capacity_w)
        ):
            residual = max(0.0, global_decision.residual_effective_watts)
            target = min(
                MAX_SOLAR_LIMIT_W,
                self.model.actuators["solar"].requested_setpoint_watts + residual,
            )
            if target > self.model.actuators["solar"].requested_setpoint_watts + 1:
                self._command_solar(
                    t_ms,
                    target,
                    RequestDomain.GRID_ONLY,
                    storage_effective_delta=False,
                    capacity_full_output_possible=True,
                )
                return f"solar-only export increase to {target:.0f} W"

        if (
            storage_min_w <= STORAGE_TARGET_W <= storage_max_w
            and global_decision.action == CorrectionAction.LESS_EFFECTIVE_OUTPUT
        ):
            residual = abs(global_decision.residual_effective_watts)

            current_bat = self.model.actuators["bat"].requested_setpoint_watts
            if current_bat > 0:
                target = max(0.0, current_bat - residual)
                target = legal_battery_setpoint_w(current_bat, target)
                if target < current_bat - 1:
                    self._command_battery(t_ms, target)
                    return f"battery reduce for solar headroom to {target:.0f} W"

            if self._battery_active_or_pending():
                return "wait battery off before solar-only reduce"

            current_solar = self.model.actuators["solar"].requested_setpoint_watts
            minimum_storage_solar = max(
                0.0,
                load_w
                + charger_grid_input_w(self.actual_charger_setpoint_w)
                - self.actual_battery_setpoint_w,
            )
            target = max(minimum_storage_solar, current_solar - residual)
            target = min(target, current_solar)
            if target < current_solar - 1:
                self._command_solar(
                    t_ms,
                    target,
                    RequestDomain.GRID_ONLY,
                    storage_effective_delta=False,
                    capacity_full_output_possible=True,
                )
                return f"solar-only export reduce to {target:.0f} W"

        return ""

    def _command_battery(self, t_ms: int, target_w: float) -> None:
        actor = self.model.actuators["bat"]
        base = actor.requested_setpoint_watts
        target_w = legal_battery_setpoint_w(base, target_w)
        delta = target_w - base
        startup = self.actual_battery_setpoint_w <= 0 and target_w > 0
        latency = 14_000 if startup else 3_000
        latest = latency + (4_000 if startup else 2_000)
        self.model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=base,
            target_setpoint_watts=target_w,
            effective_delta_watts=delta,
            storage_effective_delta_watts=delta,
            global_effective_delta_watts=delta,
            transition_kind=TransitionKind.STARTUP if startup else TransitionKind.SETPOINT,
            sent_millis=t_ms,
            not_before_effect_millis=t_ms + max(1000, latency - 2000),
            latest_effect_millis=t_ms + latest,
        )
        self.scheduled.append(
            ScheduledCommand("bat", target_w, t_ms + latency, f"battery -> {target_w:.0f} W")
        )
        self.events.append(Event(t_ms, f"cmd battery {base:.0f}->{target_w:.0f} W"))
        self.last_battery_command_ms = t_ms

    def _command_charger(self, t_ms: int, target_w: float) -> None:
        actor = self.model.actuators["charger"]
        base = actor.requested_setpoint_watts
        target_w = legal_charger_setpoint_w(target_w)
        base_effective = charger_effective_output_w(base)
        target_effective = charger_effective_output_w(target_w)
        delta = target_effective - base_effective
        startup = self.actual_charger_setpoint_w <= 0 and target_w > 0
        latency = 10_000 if startup else 3_000
        latest = latency + (5_000 if startup else 2_000)
        self.model.append_physical_request(
            "charger",
            RequestDomain.STORAGE,
            base_setpoint_watts=base,
            target_setpoint_watts=target_w,
            effective_delta_watts=delta,
            storage_effective_delta_watts=delta,
            global_effective_delta_watts=delta,
            transition_kind=TransitionKind.STARTUP if startup else TransitionKind.SETPOINT,
            sent_millis=t_ms,
            not_before_effect_millis=t_ms + max(1000, latency - 2000),
            latest_effect_millis=t_ms + latest,
        )
        self.scheduled.append(
            ScheduledCommand(
                "charger",
                target_w,
                t_ms + latency,
                f"charger -> {target_w:.0f} W",
            )
        )
        self.events.append(Event(t_ms, f"cmd charger {base:.0f}->{target_w:.0f} W"))

    def _command_solar(
        self,
        t_ms: int,
        target_w: float,
        domain: RequestDomain,
        *,
        storage_effective_delta: bool,
        capacity_full_output_possible: bool,
    ) -> None:
        actor = self.model.actuators["solar"]
        base = actor.requested_setpoint_watts
        target_w = actor.legalize_setpoint(target_w)
        delta = target_w - base
        startup = self.actual_solar_limit_w <= 0 and target_w > 0
        latency = 20_000 if startup else 5_000
        latest = latency + (7_000 if startup else 7_000)
        storage_delta = delta if storage_effective_delta else 0.0
        self.model.append_physical_request(
            "solar",
            domain,
            base_setpoint_watts=base,
            target_setpoint_watts=target_w,
            effective_delta_watts=delta,
            storage_effective_delta_watts=storage_delta,
            global_effective_delta_watts=delta,
            effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
            transition_kind=TransitionKind.STARTUP if startup else TransitionKind.SETPOINT,
            sent_millis=t_ms,
            not_before_effect_millis=t_ms + max(1000, latency - 3000),
            latest_effect_millis=t_ms + latest,
            capacity_full_output_possible=capacity_full_output_possible,
        )
        self.scheduled.append(
            ScheduledCommand("solar", target_w, t_ms + latency, f"solar -> {target_w:.0f} W")
        )
        self.events.append(Event(t_ms, f"cmd solar {base:.0f}->{target_w:.0f} W"))
        self.last_solar_probe_ms = t_ms

    def _apply_due_commands(self, t_ms: int) -> None:
        remaining: List[ScheduledCommand] = []
        for command in self.scheduled:
            if command.apply_millis <= t_ms:
                if command.actuator_key == "solar":
                    self.actual_solar_limit_w = command.target_watts
                elif command.actuator_key == "bat":
                    self.actual_battery_setpoint_w = command.target_watts
                elif command.actuator_key == "charger":
                    self.actual_charger_setpoint_w = command.target_watts
                self.events.append(Event(t_ms, f"apply {command.label}"))
            else:
                remaining.append(command)
        self.scheduled = remaining

    def _has_pending(self, actuator_key: str) -> bool:
        return any(
            request.actuator_key == actuator_key and request.is_physical_pending()
            for request in self.model.ledger
        )

    def _battery_active_or_pending(self) -> bool:
        return (
            self.actual_battery_setpoint_w > 0
            or self.model.actuators["bat"].requested_setpoint_watts > 0
            or self._has_pending("bat")
        )

    def _charger_active_or_pending(self) -> bool:
        return (
            self.actual_charger_setpoint_w > 0
            or self.model.actuators["charger"].requested_setpoint_watts > 0
            or self._has_pending("charger")
        )

    def _solar_has_useful_headroom(self, solar_capacity_w: float) -> bool:
        requested = self.model.actuators["solar"].requested_setpoint_watts
        headroom = solar_capacity_w - min(self.actual_solar_limit_w, solar_capacity_w)
        if requested >= MAX_SOLAR_LIMIT_W - 1:
            return False
        return headroom > 50 or self.actual_solar_limit_w <= 0


def external_load_w(t_ms: int) -> float:
    t = t_ms / 1000.0
    load = 420.0
    load += 35.0 * math.sin(t / 21.0)
    load += 20.0 * math.sin(t / 7.0)

    if 65 <= t < 112:
        load += 1450.0  # kettle / cooking spike
    if 150 <= t < 360:
        load += 260.0 + 80.0 * math.sin(t / 9.0)  # washing machine
    if 235 <= t < 315:
        load += 850.0  # appliance burst
    if 420 <= t < 520:
        load += 380.0  # heat pump / HVAC
    if 525 <= t < 560:
        load += 1000.0  # second kitchen event

    # Fridge-like cycling.
    if int(t // 75) % 2 == 1:
        load += 110.0

    return max(120.0, load)


def solar_capacity_watts(t_ms: int) -> float:
    t = t_ms / 1000.0
    capacity = 920.0 + 180.0 * math.sin((t - 90.0) / 140.0)
    capacity += 90.0 * math.sin(t / 31.0)

    for center, width, depth in [
        (125.0, 22.0, 430.0),
        (255.0, 34.0, 600.0),
        (390.0, 28.0, 360.0),
        (515.0, 22.0, 520.0),
    ]:
        capacity -= depth * math.exp(-((t - center) / width) ** 2)

    return min(MAX_SOLAR_LIMIT_W, max(0.0, capacity))


def legal_charger_setpoint_w(desired_w: float) -> float:
    if desired_w <= 0:
        return 0.0
    return min(MAX_CHARGER_INPUT_W, max(MIN_CHARGER_INPUT_W, desired_w))


def legal_battery_setpoint_w(current_w: float, desired_w: float) -> float:
    if desired_w <= 0:
        return 0.0

    if desired_w < MIN_BATTERY_OUTPUT_W:
        return 0.0 if current_w > 0 else 0.0

    return min(MAX_BATTERY_OUTPUT_W, desired_w)


def charger_grid_input_w(setpoint_w: float) -> float:
    if setpoint_w <= 0:
        return 0.0
    return setpoint_w + CHARGER_SELF_CONSUMPTION_W


def charger_effective_output_w(setpoint_w: float) -> float:
    return -charger_grid_input_w(setpoint_w)


def reduced_charger_target_w(current_w: float, residual_effective_w: float) -> Optional[float]:
    target_w = current_w - residual_effective_w
    if target_w >= MIN_CHARGER_INPUT_W:
        return target_w
    if residual_effective_w >= charger_grid_input_w(current_w):
        return 0.0
    return None


SERIES: Dict[str, Dict[str, object]] = {
    "load_w": {"label": "External load", "color": "#7c3aed", "axis": "w"},
    "solar_capacity_w": {"label": "Solar capacity", "color": "#f59e0b", "axis": "w"},
    "solar_output_w": {"label": "Solar output", "color": "#fbbf24", "axis": "w"},
    "battery_output_w": {"label": "Battery inverter", "color": "#f97316", "axis": "w"},
    "charger_input_w": {"label": "Grid charger input", "color": "#14b8a6", "axis": "w"},
    "grid_w": {"label": "Grid real", "color": "#dc2626", "axis": "w"},
    "storage_min_w": {"label": "Storage corridor min", "color": "#16a34a", "axis": "w"},
    "storage_max_w": {"label": "Storage corridor max", "color": "#16a34a", "axis": "w", "dash": "4 4"},
    "global_min_w": {"label": "Global corridor min", "color": "#0ea5e9", "axis": "w"},
    "global_max_w": {"label": "Global corridor max", "color": "#0ea5e9", "axis": "w", "dash": "4 4"},
    "storage_residual_w": {"label": "Storage residual", "color": "#166534", "axis": "w"},
    "global_residual_w": {"label": "Global residual", "color": "#0369a1", "axis": "w"},
    "solar_limit_w": {"label": "Solar limit", "color": "#92400e", "axis": "w", "dash": "6 4"},
    "battery_setpoint_w": {"label": "Battery target", "color": "#c2410c", "axis": "w", "dash": "6 4"},
    "charger_setpoint_w": {"label": "Grid charger setpoint", "color": "#0f766e", "axis": "w", "dash": "6 4"},
}


def render_chart(
    rows: Sequence[Row],
    events: Sequence[Event],
    keys: Sequence[str],
    title: str,
    height: int = 430,
    include_targets: bool = True,
    forced_min_value: Optional[float] = None,
) -> str:
    width = 1180
    left = 72
    right = 18
    top = 38
    bottom = 54
    plot_w = width - left - right
    plot_h = height - top - bottom
    min_t = min(row.t_ms for row in rows)
    max_t = max(row.t_ms for row in rows)

    values: List[float] = []
    for row in rows:
        for key in keys:
            values.append(float(getattr(row, key)))
    if include_targets:
        values.extend([STORAGE_TARGET_W, GLOBAL_TARGET_W])
    min_v = min(values)
    max_v = max(values)
    if forced_min_value is not None:
        min_v = forced_min_value
    if min_v == max_v:
        min_v -= 100
        max_v += 100
    padding = max(80.0, (max_v - min_v) * 0.10)
    min_v -= padding
    max_v += padding

    def x_for(t_ms: int) -> float:
        return left + ((t_ms - min_t) / (max_t - min_t)) * plot_w

    def y_for(value: float) -> float:
        return top + ((max_v - value) / (max_v - min_v)) * plot_h

    out = [
        f'<svg class="chart" viewBox="0 0 {width} {height}" role="img">',
        f'<rect x="0" y="0" width="{width}" height="{height}" fill="#fff"/>',
        f'<text x="{left}" y="24" class="chart-title">{escape(title)}</text>',
    ]

    for i in range(6):
        value = min_v + (max_v - min_v) * i / 5
        y = y_for(value)
        out.append(
            f'<line x1="{left}" y1="{y:.1f}" x2="{width - right}" y2="{y:.1f}" '
            'stroke="#e5e7eb"/>'
        )
        out.append(
            f'<text x="{left - 8}" y="{y + 4:.1f}" text-anchor="end" class="axis">'
            f'{value:.0f} W</text>'
        )

    if include_targets:
        for target, label, color in [
            (STORAGE_TARGET_W, "storage target", "#6d28d9"),
            (GLOBAL_TARGET_W, "global target", "#6b7280"),
        ]:
            y = y_for(target)
            out.append(
                f'<line x1="{left}" y1="{y:.1f}" x2="{width - right}" y2="{y:.1f}" '
                f'stroke="{color}" stroke-width="1.4" stroke-dasharray="5 4"/>'
            )
            out.append(
                f'<text x="{width - right - 4}" y="{y - 4:.1f}" text-anchor="end" '
                f'class="axis">{label}</text>'
            )

    for key in keys:
        style = SERIES[key]
        points = [
            f"{x_for(row.t_ms):.1f},{y_for(float(getattr(row, key))):.1f}"
            for row in rows
        ]
        dash = style.get("dash", "")
        dash_attr = f' stroke-dasharray="{dash}"' if dash else ""
        out.append(
            f'<polyline fill="none" stroke="{style["color"]}" stroke-width="2.1"'
            f'{dash_attr} points="{" ".join(points)}">'
            f'<title>{escape(str(style["label"]))}</title></polyline>'
        )

    for event in events:
        if event.t_ms < min_t or event.t_ms > max_t:
            continue
        x = x_for(event.t_ms)
        out.append(
            f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{top + plot_h}" '
            'stroke="#111827" stroke-width="0.8" stroke-dasharray="3 4"/>'
        )

    for minute in range(0, 11):
        t_ms = minute * 60_000
        x = x_for(t_ms)
        out.append(
            f'<text x="{x:.1f}" y="{height - 20}" text-anchor="middle" class="axis">'
            f'{minute}m</text>'
        )

    out.append("</svg>")
    return "\n".join(out)


def render_legend(keys: Sequence[str]) -> str:
    items = []
    for key in keys:
        style = SERIES[key]
        dash = " dashed" if style.get("dash") else ""
        items.append(
            f'<span class="legend-item"><span class="swatch{dash}" '
            f'style="--c:{style["color"]}"></span>{escape(str(style["label"]))}</span>'
        )
    return f'<div class="legend">{"".join(items)}</div>'


def render_table(rows: Sequence[Row]) -> str:
    selected = [row for row in rows if row.note or row.t_ms % 30_000 == 0]
    trs = []
    for row in selected:
        trs.append(
            "<tr>"
            f"<td>{row.t_ms / 1000:.0f}s</td>"
            f"<td>{row.load_w:.0f}</td>"
            f"<td>{row.solar_capacity_w:.0f}</td>"
            f"<td>{row.solar_output_w:.0f}</td>"
            f"<td>{row.battery_output_w:.0f}</td>"
            f"<td>{row.charger_input_w:.0f}</td>"
            f"<td>{row.grid_w:.0f}</td>"
            f"<td>{row.storage_min_w:.0f} .. {row.storage_max_w:.0f}</td>"
            f"<td>{row.storage_residual_w:.0f}</td>"
            f"<td>{row.global_residual_w:.0f}</td>"
            f"<td>{row.pending_count}</td>"
            f"<td>{escape(row.storage_decision)}</td>"
            f"<td>{escape(row.global_decision)}</td>"
            f"<td>{escape(row.note)}</td>"
            "</tr>"
        )
    return (
        "<table><thead><tr><th>t</th><th>Load</th><th>Solar cap</th>"
        "<th>Solar out</th><th>Battery out</th><th>Charger in</th><th>Grid</th>"
        "<th>Storage corridor</th><th>Storage residual</th><th>Global residual</th>"
        "<th>Pending</th><th>Storage</th><th>Global</th><th>Note</th>"
        f"</tr></thead><tbody>{''.join(trs)}</tbody></table>"
    )


def render_event_table(events: Sequence[Event]) -> str:
    trs = []
    for event in events:
        trs.append(
            f"<tr><td>{event.t_ms / 1000:.0f}s</td><td>{escape(event.label)}</td></tr>"
        )
    return f"<table><thead><tr><th>t</th><th>Event</th></tr></thead><tbody>{''.join(trs)}</tbody></table>"


def render_invariant_summary(rows: Sequence[Row]) -> str:
    charger_inputs = [row.charger_input_w for row in rows]
    overlap_rows = [
        row for row in rows if row.battery_output_w > 0.5 and row.charger_input_w > 0.5
    ]
    solar_throttled_with_battery_rows = [
        row
        for row in rows
        if row.battery_output_w > 0.5 and row.solar_capacity_w > row.solar_output_w + 1.0
    ]
    negative_physical_rows = [
        row
        for row in rows
        if min(
            row.solar_capacity_w,
            row.solar_limit_w,
            row.solar_output_w,
            row.battery_setpoint_w,
            row.battery_output_w,
            row.charger_setpoint_w,
            row.charger_input_w,
        )
        < -0.001
    ]
    solar_limit_violations = [
        row
        for row in rows
        if row.solar_output_w > row.solar_capacity_w + 1.0
        or row.solar_output_w > row.solar_limit_w + 1.0
    ]
    battery_export_without_reduction = [
        row
        for row in rows
        if row.battery_output_w > 0.5
        and row.t_ms % CONTROL_INTERVAL_MS == 0
        and row.storage_max_w < STORAGE_TARGET_W - ASSERTION_TOLERANCE_W
        and row.battery_requested_w >= row.battery_output_w - 0.5
        and not row.note.startswith("battery reduce")
    ]
    battery_physical_export_without_solar_only = [
        row
        for row in rows
        if row.battery_output_w > 0.5
        and row.t_ms % CONTROL_INTERVAL_MS == 0
        and row.grid_w < STORAGE_TARGET_W - ASSERTION_TOLERANCE_W
        and row.proven_solar_only_w <= 1.0
        and row.battery_requested_w >= row.battery_output_w - 0.5
        and not row.note.startswith("battery reduce")
    ]
    min_power_violations = [
        row
        for row in rows
        if (0.5 < row.battery_setpoint_w < MIN_BATTERY_OUTPUT_W - 0.5)
        or (0.5 < row.charger_setpoint_w < MIN_CHARGER_INPUT_W - 0.5)
    ]
    max_grid_balance_error = max(
        abs(
            row.grid_w
            - (
                row.load_w
                + row.charger_input_w
                - row.solar_output_w
                - row.battery_output_w
            )
        )
        for row in rows
    )
    wait_rows = [row for row in rows if "wait" in row.note]
    max_pending = max(row.pending_count for row in rows)

    return (
        "<table><thead><tr><th>Check</th><th>Value</th></tr></thead><tbody>"
        f"<tr><td>Grid charger physical input range</td>"
        f"<td>{min(charger_inputs):.0f} W .. {max(charger_inputs):.0f} W</td></tr>"
        f"<tr><td>Battery and charger physically active together</td>"
        f"<td>{len(overlap_rows)} samples</td></tr>"
        f"<tr><td>Solar curtailed while battery inverter is active</td>"
        f"<td>{len(solar_throttled_with_battery_rows)} samples</td></tr>"
        f"<tr><td>Negative physical actuator values</td>"
        f"<td>{len(negative_physical_rows)} samples</td></tr>"
        f"<tr><td>Solar output above limit or capacity</td>"
        f"<td>{len(solar_limit_violations)} samples</td></tr>"
        f"<tr><td>Battery below storage target at control tick without reduction</td>"
        f"<td>{len(battery_export_without_reduction)} samples</td></tr>"
        f"<tr><td>Battery physical export at control tick without reduction</td>"
        f"<td>{len(battery_physical_export_without_solar_only)} samples</td></tr>"
        f"<tr><td>Active setpoint below minimum power</td>"
        f"<td>{len(min_power_violations)} samples</td></tr>"
        f"<tr><td>Maximum grid balance equation error</td>"
        f"<td>{max_grid_balance_error:.3f} W</td></tr>"
        f"<tr><td>Mutual-exclusion wait rows</td>"
        f"<td>{len(wait_rows)} samples</td></tr>"
        f"<tr><td>Maximum pending request count</td><td>{max_pending}</td></tr>"
        "</tbody></table>"
    )


def render_report(sim: HouseholdSimulation) -> str:
    overview_keys = [
        "grid_w",
        "storage_min_w",
        "storage_max_w",
        "global_min_w",
        "global_max_w",
    ]
    physical_keys = [
        "load_w",
        "solar_capacity_w",
        "solar_limit_w",
        "solar_output_w",
        "battery_setpoint_w",
        "battery_output_w",
        "charger_setpoint_w",
        "charger_input_w",
    ]
    residual_keys = [
        "storage_residual_w",
        "global_residual_w",
        "grid_w",
    ]
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>DPL Household Regulation Simulation</title>
<style>
body {{
  background: #f6f7f9;
  color: #1f2933;
  font: 14px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  margin: 0;
}}
main {{
  max-width: 1220px;
  margin: 0 auto;
  padding: 28px 20px 48px;
}}
h1 {{
  font-size: 28px;
  margin: 0 0 8px;
}}
h2 {{
  font-size: 20px;
  margin: 24px 0 8px;
}}
.panel {{
  background: #fff;
  border: 1px solid #d9dde3;
  border-radius: 6px;
  margin: 18px 0;
  padding: 18px;
}}
.chart {{
  background: #fff;
  border: 1px solid #e5e7eb;
  border-radius: 4px;
  width: 100%;
}}
.axis {{
  fill: #667085;
  font-size: 12px;
}}
.chart-title {{
  fill: #111827;
  font-size: 16px;
  font-weight: 600;
}}
.legend {{
  display: flex;
  flex-wrap: wrap;
  gap: 10px 16px;
  margin: 14px 0;
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
  background: #f8fafc;
}}
code {{
  background: #eef2f7;
  border-radius: 3px;
  padding: 1px 4px;
}}
</style>
</head>
<body>
<main>
<h1>DPL Household Regulation Simulation</h1>
<p>Closed-loop synthetic 10-minute run. Assumptions: battery is full and
available for AC output, one solar inverter, one battery inverter, one grid
charger. Storage target is <code>0 W</code>; global target is
<code>-150 W</code> and may only be reached by solar. Target comparisons use the
exact predictive corridor; the report invariants use a <code>5 W</code> numeric
tolerance. The controller may issue consecutive absolute
setpoint updates while earlier requests are still in flight. The grid charger
starts at <code>500 W</code> to represent a battery that wants to keep charging
to 100% whenever the storage target permits it.</p>

<p>Control priority follows the specification: for more storage effective
output the simulation tries storage-relevant solar first, then reduces grid
charger input, then increases battery output. For less storage effective output
it reduces battery output first, then increases grid charger input, then reduces
storage-relevant solar. The order is also a mutual-exclusion rule for storage:
battery output and grid charger input are not allowed to be physically active
at the same time. Solar-only global offset is regulated only by solar. The grid
charger is plotted as positive physical AC input; its internal effective-output
delta has the opposite sign by convention and is not a physical input value.</p>

<div class="panel">
<h2>Invariant checks</h2>
{render_invariant_summary(sim.rows)}
</div>

<div class="panel">
{render_legend(overview_keys)}
{render_chart(sim.rows, sim.events, overview_keys, "Signed grid and forecast corridors")}
</div>

<div class="panel">
{render_legend(physical_keys)}
{render_chart(sim.rows, sim.events, physical_keys, "Physical powers and setpoints (non-negative)", include_targets=False, forced_min_value=0.0)}
</div>

<div class="panel">
{render_legend(residual_keys)}
{render_chart(sim.rows, sim.events, residual_keys, "Control residuals")}
</div>

<div class="panel">
<h2>Sample table</h2>
{render_table(sim.rows)}
</div>

<div class="panel">
<h2>Command and apply events</h2>
{render_event_table(sim.events)}
</div>
</main>
</body>
</html>
"""


def main(argv: Sequence[str]) -> int:
    output_path = Path(argv[1]) if len(argv) > 1 else REPORT_PATH
    simulation = HouseholdSimulation()
    simulation.run()
    output_path.write_text(render_report(simulation), encoding="utf-8")
    print(f"Wrote {output_path}")
    print(f"Samples: {len(simulation.rows)}")
    print(f"Events: {len(simulation.events)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
