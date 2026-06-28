#!/usr/bin/env python3
"""Executable reference model for the DPL predictive control algorithm.

This module is intentionally independent from the firmware. It models the
control specification in docs/PowerLimiterPredictiveControl.md so scenarios can
be tested with synthetic meter samples and command ledgers before the logic is
ported to C++.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
from itertools import product
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


PrefixState = Tuple[int, float, Tuple[float, ...]]


class ActuatorKind(Enum):
    BATTERY = "battery"
    CHARGER = "charger"
    SOLAR = "solar"
    SMART_BUFFER = "smart_buffer"


class RequestDomain(Enum):
    STORAGE = "storage"
    GRID_ONLY = "grid_only"
    MIXED = "mixed"


class ForecastDomain(Enum):
    STORAGE = "storage"
    GLOBAL = "global"


class RequestCause(Enum):
    NORMAL = "normal"
    SAFETY = "safety"
    MANUAL = "manual"
    RECOVERY = "recovery"


class RequestState(Enum):
    QUEUED = "queued"
    SUPERSEDED_BEFORE_SEND = "superseded_before_send"
    SENT = "sent"
    ACCEPTED = "accepted"
    REJECTED = "rejected"
    FAILED_NO_EFFECT = "failed_no_effect"
    FAILED_AMBIGUOUS = "failed_ambiguous"
    SETTLED_BY_METER = "settled_by_meter"
    SETTLED_BY_TELEMETRY = "settled_by_telemetry"
    SETTLED_BY_TIMEOUT = "settled_by_timeout"
    SETTLED_SUPERSEDED = "settled_superseded"
    SETTLED_DISCARDED = "settled_discarded"


class EffectKind(Enum):
    DETERMINISTIC = "deterministic"
    SOLAR_CAPACITY_LIMITED = "solar_capacity_limited"
    AMBIGUOUS = "ambiguous"


class TransitionKind(Enum):
    SETPOINT = "setpoint"
    STARTUP = "startup"
    STANDBY = "standby"


class CorrectionAction(Enum):
    HOLD = "hold"
    MORE_EFFECTIVE_OUTPUT = "more_effective_output"
    LESS_EFFECTIVE_OUTPUT = "less_effective_output"


PHYSICAL_PENDING_STATES = {
    RequestState.SENT,
    RequestState.ACCEPTED,
    RequestState.FAILED_AMBIGUOUS,
}

ACTIVE_FORECAST_STATES = {
    RequestState.QUEUED,
    RequestState.SENT,
    RequestState.ACCEPTED,
    RequestState.FAILED_AMBIGUOUS,
}


@dataclass
class ActuatorCapabilities:
    min_active_setpoint_watts: float = 0.0
    max_setpoint_watts: Optional[float] = None
    self_consumption_watts: float = 0.0

    def legalize_setpoint(self, desired_setpoint_watts: float) -> float:
        if desired_setpoint_watts <= 0:
            return 0.0

        legal = max(desired_setpoint_watts, self.min_active_setpoint_watts)
        if self.max_setpoint_watts is not None:
            legal = min(legal, self.max_setpoint_watts)
        return legal


@dataclass
class ActuatorState:
    actuator_key: str
    kind: ActuatorKind
    safe_setpoint_watts: float
    requested_setpoint_watts: Optional[float] = None
    measured_output_watts: Optional[float] = None
    capabilities: ActuatorCapabilities = field(default_factory=ActuatorCapabilities)

    def __post_init__(self) -> None:
        self.safe_setpoint_watts = self.capabilities.legalize_setpoint(
            self.safe_setpoint_watts
        )
        if self.requested_setpoint_watts is None:
            self.requested_setpoint_watts = self.safe_setpoint_watts
        else:
            self.requested_setpoint_watts = self.capabilities.legalize_setpoint(
                self.requested_setpoint_watts
            )

    def legalize_setpoint(self, desired_setpoint_watts: float) -> float:
        return self.capabilities.legalize_setpoint(desired_setpoint_watts)

    def effective_output_for_setpoint(self, setpoint_watts: float) -> float:
        legal_setpoint = self.legalize_setpoint(setpoint_watts)
        if legal_setpoint <= 0:
            return 0.0

        if self.kind == ActuatorKind.CHARGER:
            return -(legal_setpoint + self.capabilities.self_consumption_watts)

        return legal_setpoint


@dataclass
class PendingControlRequest:
    seq: int
    actuator_key: str
    domain: RequestDomain
    base_setpoint_watts: float
    target_setpoint_watts: float
    effective_delta_watts: float
    storage_effective_delta_watts: float
    global_effective_delta_watts: float
    cause: RequestCause = RequestCause.NORMAL
    state: RequestState = RequestState.QUEUED
    effect_kind: EffectKind = EffectKind.DETERMINISTIC
    transition_kind: TransitionKind = TransitionKind.SETPOINT
    created_millis: int = 0
    sent_millis: Optional[int] = None
    ack_millis: Optional[int] = None
    not_before_effect_millis: Optional[int] = None
    earliest_expected_millis: Optional[int] = None
    typical_effect_millis: Optional[int] = None
    latest_effect_millis: Optional[int] = None
    capacity_full_output_possible: bool = True

    @property
    def setpoint_delta_watts(self) -> float:
        return self.target_setpoint_watts - self.base_setpoint_watts

    @property
    def meter_delta_watts(self) -> float:
        return -self.effective_delta_watts

    def storage_meter_delta_watts(self, scale: float = 1.0) -> float:
        return -self.storage_effective_delta_watts * scale

    def global_meter_delta_watts(self, scale: float = 1.0) -> float:
        return -self.global_effective_delta_watts * scale

    def is_physical_pending(self) -> bool:
        return self.state in PHYSICAL_PENDING_STATES

    def is_active_for_forecast(self) -> bool:
        return self.state in ACTIVE_FORECAST_STATES


@dataclass(frozen=True)
class MeterSnapshot:
    physical_grid_watts: float
    meter_sample_millis: int
    proven_solar_only_effective_watts: float = 0.0


@dataclass(frozen=True)
class ControlForecast:
    valid: bool
    domain: ForecastDomain
    meter_sample_millis: int
    virtual_meter_watts: float
    grid_min_watts: float
    grid_nominal_watts: float
    grid_max_watts: float
    pending_count: int


@dataclass(frozen=True)
class CorrectionDecision:
    action: CorrectionAction
    residual_effective_watts: float
    reason: str
    nearest_corridor_edge_watts: float


@dataclass(frozen=True)
class SolarAllocation:
    storage_effective_watts: float
    solar_only_effective_watts: float
    storage_shortfall_watts: float
    solar_only_shortfall_watts: float


class PredictiveControlModel:
    """Small reference implementation of the predictive-control core."""

    def __init__(self, actuators: Iterable[ActuatorState] = ()) -> None:
        self.actuators: Dict[str, ActuatorState] = {
            actuator.actuator_key: actuator for actuator in actuators
        }
        self.ledger: List[PendingControlRequest] = []
        self._next_seq = 1

    def add_actuator(self, actuator: ActuatorState) -> None:
        self.actuators[actuator.actuator_key] = actuator

    def queue_request(
        self,
        actuator_key: str,
        domain: RequestDomain,
        target_setpoint_watts: float,
        effective_delta_watts: float,
        storage_effective_delta_watts: float,
        global_effective_delta_watts: float,
        *,
        cause: RequestCause = RequestCause.NORMAL,
        effect_kind: EffectKind = EffectKind.DETERMINISTIC,
        transition_kind: TransitionKind = TransitionKind.SETPOINT,
        created_millis: int = 0,
    ) -> PendingControlRequest:
        actuator = self.actuators[actuator_key]
        for pending in self.ledger:
            if (
                pending.actuator_key == actuator_key
                and pending.state == RequestState.QUEUED
            ):
                pending.state = RequestState.SUPERSEDED_BEFORE_SEND

        request = PendingControlRequest(
            seq=self._next_seq,
            actuator_key=actuator_key,
            domain=domain,
            base_setpoint_watts=actuator.requested_setpoint_watts,
            target_setpoint_watts=target_setpoint_watts,
            effective_delta_watts=effective_delta_watts,
            storage_effective_delta_watts=storage_effective_delta_watts,
            global_effective_delta_watts=global_effective_delta_watts,
            cause=cause,
            state=RequestState.QUEUED,
            effect_kind=effect_kind,
            transition_kind=transition_kind,
            created_millis=created_millis,
        )
        self._next_seq += 1
        self.ledger.append(request)
        actuator.requested_setpoint_watts = target_setpoint_watts
        return request

    def append_physical_request(
        self,
        actuator_key: str,
        domain: RequestDomain,
        base_setpoint_watts: float,
        target_setpoint_watts: float,
        effective_delta_watts: float,
        storage_effective_delta_watts: float,
        global_effective_delta_watts: float,
        *,
        cause: RequestCause = RequestCause.NORMAL,
        state: RequestState = RequestState.ACCEPTED,
        effect_kind: EffectKind = EffectKind.DETERMINISTIC,
        transition_kind: TransitionKind = TransitionKind.SETPOINT,
        created_millis: int = 0,
        sent_millis: int = 0,
        ack_millis: Optional[int] = None,
        not_before_effect_millis: Optional[int] = None,
        earliest_expected_millis: Optional[int] = None,
        typical_effect_millis: Optional[int] = None,
        latest_effect_millis: Optional[int] = None,
        capacity_full_output_possible: bool = True,
    ) -> PendingControlRequest:
        request = PendingControlRequest(
            seq=self._next_seq,
            actuator_key=actuator_key,
            domain=domain,
            base_setpoint_watts=base_setpoint_watts,
            target_setpoint_watts=target_setpoint_watts,
            effective_delta_watts=effective_delta_watts,
            storage_effective_delta_watts=storage_effective_delta_watts,
            global_effective_delta_watts=global_effective_delta_watts,
            cause=cause,
            state=state,
            effect_kind=effect_kind,
            transition_kind=transition_kind,
            created_millis=created_millis,
            sent_millis=sent_millis,
            ack_millis=ack_millis,
            not_before_effect_millis=not_before_effect_millis,
            earliest_expected_millis=earliest_expected_millis,
            typical_effect_millis=typical_effect_millis,
            latest_effect_millis=latest_effect_millis,
            capacity_full_output_possible=capacity_full_output_possible,
        )
        self._next_seq += 1
        self.ledger.append(request)
        self.actuators[actuator_key].requested_setpoint_watts = target_setpoint_watts
        return request

    def mark_queued_request_sent(
        self,
        seq: int,
        sent_millis: int,
        *,
        state: RequestState = RequestState.SENT,
        ack_millis: Optional[int] = None,
        not_before_effect_millis: Optional[int] = None,
        earliest_expected_millis: Optional[int] = None,
        typical_effect_millis: Optional[int] = None,
        latest_effect_millis: Optional[int] = None,
    ) -> PendingControlRequest:
        if state not in {RequestState.SENT, RequestState.ACCEPTED}:
            raise ValueError("sent queued request must become sent or accepted")

        request = self._request_by_seq(seq)
        if request.state != RequestState.QUEUED:
            raise ValueError("only queued requests can be marked sent")

        request.state = state
        request.sent_millis = sent_millis
        request.ack_millis = ack_millis
        request.not_before_effect_millis = not_before_effect_millis
        request.earliest_expected_millis = earliest_expected_millis
        request.typical_effect_millis = typical_effect_millis
        request.latest_effect_millis = latest_effect_millis
        return request

    def classify_failure(self, seq: int, state: RequestState) -> None:
        if state not in {RequestState.FAILED_NO_EFFECT, RequestState.FAILED_AMBIGUOUS}:
            raise ValueError("failure state must be failed-no-effect or ambiguous")

        request = self._request_by_seq(seq)
        request.state = state
        self._recompute_requested_setpoint(request.actuator_key)

    def advance_ledger(self, meter_sample_millis: int) -> None:
        for actuator_key, chain in self._active_chains().items():
            actuator = self.actuators[actuator_key]
            for request in chain:
                if (
                    request.latest_effect_millis is None
                    or meter_sample_millis < request.latest_effect_millis
                ):
                    break

                if request.state == RequestState.FAILED_AMBIGUOUS:
                    break

                request.state = RequestState.SETTLED_BY_TIMEOUT
                actuator.safe_setpoint_watts = request.target_setpoint_watts

            self._recompute_requested_setpoint(actuator_key)

    def prune_inactive_requests(self, retained_inactive_requests: int = 0) -> None:
        if retained_inactive_requests < 0:
            raise ValueError("retained_inactive_requests must not be negative")

        retained = 0
        pruned: List[PendingControlRequest] = []
        for request in reversed(self.ledger):
            if request.is_active_for_forecast():
                pruned.append(request)
                continue
            if retained < retained_inactive_requests:
                retained += 1
                pruned.append(request)

        self.ledger = list(reversed(pruned))

    def forecast(
        self,
        snapshot: MeterSnapshot,
        domain: ForecastDomain,
    ) -> ControlForecast:
        virtual_meter = self._virtual_meter(snapshot, domain)
        future_values = [virtual_meter]
        pending_count = 0

        for chain in self._active_chains().values():
            domain_chain = [
                request
                for request in chain
                if self._request_is_relevant_to_domain(request, domain)
            ]
            if not domain_chain:
                continue

            pending_count += len(domain_chain)
            pairs = self._actuator_meter_delta_pairs(
                domain_chain, domain, snapshot.meter_sample_millis
            )
            adjustments = [final - visible for visible, final in pairs]
            future_values = [
                value + adjustment
                for value, adjustment in product(future_values, adjustments)
            ]

        return ControlForecast(
            valid=True,
            domain=domain,
            meter_sample_millis=snapshot.meter_sample_millis,
            virtual_meter_watts=virtual_meter,
            grid_min_watts=min(future_values),
            grid_nominal_watts=sum(future_values) / len(future_values),
            grid_max_watts=max(future_values),
            pending_count=pending_count,
        )

    def possible_meter_deltas_for_actuator(
        self,
        actuator_key: str,
        domain: ForecastDomain,
        meter_sample_millis: int,
    ) -> List[float]:
        chain = self._active_chains().get(actuator_key, [])
        pairs = self._actuator_meter_delta_pairs(chain, domain, meter_sample_millis)
        return sorted({visible for visible, _final in pairs})

    def _request_by_seq(self, seq: int) -> PendingControlRequest:
        for request in self.ledger:
            if request.seq == seq:
                return request
        raise KeyError(seq)

    def _virtual_meter(
        self,
        snapshot: MeterSnapshot,
        domain: ForecastDomain,
    ) -> float:
        if domain == ForecastDomain.STORAGE:
            return (
                snapshot.physical_grid_watts
                + snapshot.proven_solar_only_effective_watts
            )
        return snapshot.physical_grid_watts

    def _active_chains(self) -> Dict[str, List[PendingControlRequest]]:
        chains: Dict[str, List[PendingControlRequest]] = {}
        for request in self.ledger:
            if not request.is_active_for_forecast():
                continue
            chains.setdefault(request.actuator_key, []).append(request)

        for chain in chains.values():
            chain.sort(key=lambda request: request.seq)
        return chains

    def _actuator_meter_delta_pairs(
        self,
        chain: Sequence[PendingControlRequest],
        domain: ForecastDomain,
        meter_sample_millis: int,
    ) -> List[Tuple[float, float]]:
        if not chain:
            return [(0.0, 0.0)]

        visible_domain = (
            ForecastDomain.GLOBAL if domain == ForecastDomain.STORAGE else domain
        )
        visible_deltas = self._possible_visible_prefix_states(
            chain, visible_domain, meter_sample_millis
        )
        final_deltas = self._possible_final_prefix_states(chain, domain)
        return [
            (visible_delta, final_delta)
            for visible_prefix, visible_delta, visible_scales in visible_deltas
            for final_prefix, final_delta, final_scales in final_deltas
            if self._final_state_is_consistent_with_visible_state(
                visible_prefix,
                visible_scales,
                final_prefix,
                final_scales,
            )
        ]

    def _possible_visible_prefix_states(
        self,
        chain: Sequence[PendingControlRequest],
        domain: ForecastDomain,
        meter_sample_millis: int,
    ) -> List[PrefixState]:
        max_prefix = 0
        for request in chain:
            if not self._could_be_visible(request, meter_sample_millis):
                break
            max_prefix += 1

        min_prefix = 0
        for index, request in enumerate(chain, start=1):
            if not self._must_be_visible(request, meter_sample_millis):
                break
            min_prefix = index

        return self._prefix_delta_options(chain, domain, min_prefix, max_prefix)

    def _possible_final_prefix_states(
        self,
        chain: Sequence[PendingControlRequest],
        domain: ForecastDomain,
    ) -> List[PrefixState]:
        min_prefix = len(chain)
        for index, request in enumerate(chain):
            if self._has_uncertain_final_effect(request):
                min_prefix = index
                break

        return self._prefix_delta_options(chain, domain, min_prefix, len(chain))

    def _has_uncertain_final_effect(self, request: PendingControlRequest) -> bool:
        if request.state == RequestState.FAILED_AMBIGUOUS:
            return True
        if request.effect_kind == EffectKind.AMBIGUOUS:
            return True
        return (
            request.effect_kind == EffectKind.SOLAR_CAPACITY_LIMITED
            and request.effective_delta_watts > 0
        )

    def _prefix_delta_options(
        self,
        chain: Sequence[PendingControlRequest],
        domain: ForecastDomain,
        min_prefix: int,
        max_prefix: int,
    ) -> List[PrefixState]:
        prefix_options: List[List[Tuple[float, Tuple[float, ...]]]] = [[(0.0, ())]]
        current_options = [(0.0, ())]

        for request in chain:
            next_options: List[Tuple[float, Tuple[float, ...]]] = []
            for current_delta, current_scales in current_options:
                for scale in self._effect_scales(request):
                    next_options.append(
                        (
                            current_delta + self._meter_delta(request, domain, scale),
                            current_scales + (scale,),
                        )
                    )
            current_options = sorted(set(next_options))
            prefix_options.append(current_options)

        result: List[PrefixState] = []
        for prefix in range(min_prefix, max_prefix + 1):
            result.extend(
                (prefix, delta, scales) for delta, scales in prefix_options[prefix]
            )
        return sorted(set(result))

    def _final_state_is_consistent_with_visible_state(
        self,
        visible_prefix: int,
        visible_scales: Tuple[float, ...],
        final_prefix: int,
        final_scales: Tuple[float, ...],
    ) -> bool:
        if final_prefix < visible_prefix:
            return False

        for index, visible_scale in enumerate(visible_scales):
            if index >= len(final_scales):
                return False
            if final_scales[index] < visible_scale:
                return False

        return True

    def _meter_delta(
        self,
        request: PendingControlRequest,
        domain: ForecastDomain,
        scale: float,
    ) -> float:
        if domain == ForecastDomain.STORAGE:
            return request.storage_meter_delta_watts(scale)
        return request.global_meter_delta_watts(scale)

    def _request_is_relevant_to_domain(
        self,
        request: PendingControlRequest,
        domain: ForecastDomain,
    ) -> bool:
        if domain == ForecastDomain.STORAGE:
            return request.global_effective_delta_watts != 0.0
        return request.global_effective_delta_watts != 0.0

    def _effect_scales(self, request: PendingControlRequest) -> List[float]:
        if request.effect_kind != EffectKind.SOLAR_CAPACITY_LIMITED:
            return [1.0]

        if request.effective_delta_watts <= 0:
            return [1.0]

        if request.capacity_full_output_possible:
            return [0.0, 1.0]
        return [0.0]

    def _could_be_visible(
        self,
        request: PendingControlRequest,
        meter_sample_millis: int,
    ) -> bool:
        if request.sent_millis is None:
            return False
        if meter_sample_millis < request.sent_millis:
            return False
        if (
            request.not_before_effect_millis is not None
            and meter_sample_millis < request.not_before_effect_millis
        ):
            return False
        return True

    def _must_be_visible(
        self,
        request: PendingControlRequest,
        meter_sample_millis: int,
    ) -> bool:
        if request.latest_effect_millis is None:
            return False
        if meter_sample_millis < request.latest_effect_millis:
            return False
        if request.state == RequestState.FAILED_AMBIGUOUS:
            return False
        if request.effect_kind in {EffectKind.SOLAR_CAPACITY_LIMITED, EffectKind.AMBIGUOUS}:
            return False
        return True

    def _recompute_requested_setpoint(self, actuator_key: str) -> None:
        actuator = self.actuators[actuator_key]
        requested = actuator.safe_setpoint_watts

        for request in sorted(self.ledger, key=lambda item: item.seq):
            if request.actuator_key != actuator_key:
                continue
            if request.state in {
                RequestState.QUEUED,
                RequestState.SENT,
                RequestState.ACCEPTED,
                RequestState.FAILED_AMBIGUOUS,
            }:
                requested = request.target_setpoint_watts

        actuator.requested_setpoint_watts = requested


def decide_correction(
    target_watts: float,
    forecast: ControlForecast,
    hysteresis_watts: float = 0.0,
) -> CorrectionDecision:
    if (
        target_watts >= forecast.grid_min_watts - hysteresis_watts
        and target_watts <= forecast.grid_max_watts + hysteresis_watts
    ):
        return CorrectionDecision(
            action=CorrectionAction.HOLD,
            residual_effective_watts=0.0,
            reason="inside_corridor",
            nearest_corridor_edge_watts=target_watts,
        )

    if target_watts < forecast.grid_min_watts - hysteresis_watts:
        return CorrectionDecision(
            action=CorrectionAction.MORE_EFFECTIVE_OUTPUT,
            residual_effective_watts=forecast.grid_min_watts - target_watts,
            reason="target_below_corridor",
            nearest_corridor_edge_watts=forecast.grid_min_watts,
        )

    return CorrectionDecision(
        action=CorrectionAction.LESS_EFFECTIVE_OUTPUT,
        residual_effective_watts=forecast.grid_max_watts - target_watts,
        reason="target_above_corridor",
        nearest_corridor_edge_watts=forecast.grid_max_watts,
    )


def decide_solar_only_correction(
    target_watts: float,
    forecast: ControlForecast,
    hysteresis_watts: float = 0.0,
) -> CorrectionDecision:
    if forecast.valid and target_watts < forecast.grid_max_watts - hysteresis_watts:
        return CorrectionDecision(
            action=CorrectionAction.MORE_EFFECTIVE_OUTPUT,
            residual_effective_watts=forecast.grid_max_watts - target_watts,
            reason="target_below_solar_probe_edge",
            nearest_corridor_edge_watts=forecast.grid_max_watts,
        )

    return decide_correction(target_watts, forecast, hysteresis_watts)


def allocate_mixed_solar(
    storage_part_watts: float,
    solar_only_part_watts: float,
    possible_solar_effective_watts: float,
) -> SolarAllocation:
    storage_effective = min(storage_part_watts, possible_solar_effective_watts)
    remaining = max(0.0, possible_solar_effective_watts - storage_effective)
    solar_only_effective = min(solar_only_part_watts, remaining)

    return SolarAllocation(
        storage_effective_watts=storage_effective,
        solar_only_effective_watts=solar_only_effective,
        storage_shortfall_watts=max(0.0, storage_part_watts - storage_effective),
        solar_only_shortfall_watts=max(
            0.0, solar_only_part_watts - solar_only_effective
        ),
    )


def dynamic_target_learning_allowed(storage_forecast: ControlForecast) -> bool:
    return storage_forecast.valid


def deterministic_expected_meter_value(
    forecast: ControlForecast,
    tolerance_watts: float = 1.0,
) -> Optional[float]:
    if not forecast.valid:
        return None
    if abs(forecast.grid_max_watts - forecast.grid_min_watts) > tolerance_watts:
        return None
    return forecast.grid_nominal_watts


def global_target_from_solar_offset(
    storage_target_watts: float,
    solar_only_export_offset_watts: float,
) -> float:
    if solar_only_export_offset_watts < 0:
        raise ValueError("solar-only export offset must be non-negative")
    return storage_target_watts - solar_only_export_offset_watts


def proven_solar_only_effective_output(
    physical_grid_watts: float,
    storage_target_watts: float,
    solar_only_export_offset_watts: float,
    storage_absorption_headroom_watts: float,
    solar_effective_output_watts: float,
) -> float:
    if solar_only_export_offset_watts <= 0 or solar_effective_output_watts <= 0:
        return 0.0

    export_beyond_storage_target = storage_target_watts - physical_grid_watts
    if export_beyond_storage_target <= 0:
        return 0.0

    solar_only_candidate = min(
        export_beyond_storage_target,
        solar_only_export_offset_watts,
        solar_effective_output_watts,
    )
    remaining_after_storage_absorption = (
        solar_only_candidate - max(0.0, storage_absorption_headroom_watts)
    )
    return min(
        max(remaining_after_storage_absorption, 0.0),
        solar_only_export_offset_watts,
    )


def validate_target_ownership(
    storage_target_watts: float,
    global_target_watts: float,
    *,
    clamp: bool = False,
) -> float:
    if global_target_watts <= storage_target_watts:
        return global_target_watts
    if clamp:
        return storage_target_watts
    raise ValueError("global target must be less than or equal to storage target")
