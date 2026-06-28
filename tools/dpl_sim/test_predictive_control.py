#!/usr/bin/env python3
"""Tests for the DPL predictive-control reference model."""

import unittest

from predictive_control import (
    ActuatorCapabilities,
    ActuatorKind,
    ActuatorState,
    CorrectionAction,
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
    decide_solar_only_correction,
    deterministic_expected_meter_value,
    dynamic_target_learning_allowed,
    global_target_from_solar_offset,
    proven_solar_only_effective_output,
    validate_target_ownership,
)


class PredictiveControlModelTest(unittest.TestCase):
    def test_solar_only_offset_does_not_cause_storage_action(self) -> None:
        model = PredictiveControlModel()
        snapshot = MeterSnapshot(
            physical_grid_watts=-300.0,
            meter_sample_millis=10_000,
            proven_solar_only_effective_watts=300.0,
        )

        storage_forecast = model.forecast(snapshot, ForecastDomain.STORAGE)
        global_forecast = model.forecast(snapshot, ForecastDomain.GLOBAL)

        self.assertEqual(storage_forecast.virtual_meter_watts, 0.0)
        self.assertEqual(global_forecast.virtual_meter_watts, -300.0)
        self.assertEqual(
            decide_correction(0.0, storage_forecast).action,
            CorrectionAction.HOLD,
        )
        self.assertEqual(
            decide_correction(-300.0, global_forecast).action,
            CorrectionAction.HOLD,
        )

    def test_battery_pending_command_visible_early_is_not_double_counted(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0.0)]
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=0.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )

        snapshot = MeterSnapshot(physical_grid_watts=0.0, meter_sample_millis=1_500)
        forecast = model.forecast(snapshot, ForecastDomain.STORAGE)

        self.assertEqual(forecast.grid_min_watts, -500.0)
        self.assertEqual(forecast.grid_max_watts, 0.0)
        self.assertEqual(decide_correction(0.0, forecast).action, CorrectionAction.HOLD)

    def test_battery_pending_command_not_yet_visible_keeps_corridor_open(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0.0)]
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=0.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )

        snapshot = MeterSnapshot(physical_grid_watts=500.0, meter_sample_millis=1_500)
        forecast = model.forecast(snapshot, ForecastDomain.STORAGE)

        self.assertEqual(forecast.grid_min_watts, 0.0)
        self.assertEqual(forecast.grid_max_watts, 500.0)
        self.assertEqual(decide_correction(0.0, forecast).action, CorrectionAction.HOLD)

    def test_not_before_effect_time_blocks_visible_branch(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0.0)]
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=0.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
            sent_millis=1_000,
            not_before_effect_millis=2_000,
            latest_effect_millis=4_000,
        )

        snapshot = MeterSnapshot(physical_grid_watts=500.0, meter_sample_millis=1_500)
        forecast = model.forecast(snapshot, ForecastDomain.STORAGE)

        self.assertEqual(forecast.grid_min_watts, 0.0)
        self.assertEqual(forecast.grid_max_watts, 0.0)

    def test_soft_timing_does_not_close_conservative_corridor(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0.0)]
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=0.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
            sent_millis=1_000,
            earliest_expected_millis=2_000,
            typical_effect_millis=3_000,
            latest_effect_millis=4_000,
        )

        snapshot = MeterSnapshot(physical_grid_watts=500.0, meter_sample_millis=1_500)
        forecast = model.forecast(snapshot, ForecastDomain.STORAGE)

        self.assertEqual(forecast.grid_min_watts, 0.0)
        self.assertEqual(forecast.grid_max_watts, 500.0)

    def test_ordered_chain_does_not_create_impossible_sum(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=700.0)]
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=700.0,
            target_setpoint_watts=300.0,
            effective_delta_watts=-400.0,
            storage_effective_delta_watts=-400.0,
            global_effective_delta_watts=-400.0,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=300.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=200.0,
            storage_effective_delta_watts=200.0,
            global_effective_delta_watts=200.0,
            sent_millis=1_100,
            latest_effect_millis=4_100,
        )

        visible_deltas = model.possible_meter_deltas_for_actuator(
            "bat", ForecastDomain.STORAGE, meter_sample_millis=1_500
        )

        self.assertEqual(visible_deltas, [0.0, 200.0, 400.0])
        self.assertNotIn(-200.0, visible_deltas)

    def test_ambiguous_earlier_request_does_not_make_later_delta_relative_to_safe(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=700.0)]
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=700.0,
            target_setpoint_watts=300.0,
            effective_delta_watts=-400.0,
            storage_effective_delta_watts=-400.0,
            global_effective_delta_watts=-400.0,
            state=RequestState.FAILED_AMBIGUOUS,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=300.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=200.0,
            storage_effective_delta_watts=200.0,
            global_effective_delta_watts=200.0,
            sent_millis=1_100,
            latest_effect_millis=4_100,
        )

        visible_deltas = model.possible_meter_deltas_for_actuator(
            "bat", ForecastDomain.STORAGE, meter_sample_millis=1_500
        )

        self.assertEqual(visible_deltas, [0.0, 200.0, 400.0])
        self.assertNotIn(-200.0, visible_deltas)

    def test_later_request_with_earlier_latest_does_not_settle_alone(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=700.0)]
        )
        request_a = model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=700.0,
            target_setpoint_watts=300.0,
            effective_delta_watts=-400.0,
            storage_effective_delta_watts=-400.0,
            global_effective_delta_watts=-400.0,
            sent_millis=1_000,
            latest_effect_millis=5_000,
        )
        request_b = model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=300.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=200.0,
            storage_effective_delta_watts=200.0,
            global_effective_delta_watts=200.0,
            sent_millis=1_100,
            latest_effect_millis=3_000,
        )

        model.advance_ledger(meter_sample_millis=3_000)

        self.assertEqual(model.actuators["bat"].safe_setpoint_watts, 700.0)
        self.assertEqual(request_a.state, RequestState.ACCEPTED)
        self.assertEqual(request_b.state, RequestState.ACCEPTED)

    def test_failed_queued_request_does_not_suppress_retry(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0.0)]
        )
        request = model.queue_request(
            "bat",
            RequestDomain.STORAGE,
            target_setpoint_watts=1_000.0,
            effective_delta_watts=1_000.0,
            storage_effective_delta_watts=1_000.0,
            global_effective_delta_watts=1_000.0,
        )

        self.assertEqual(model.actuators["bat"].requested_setpoint_watts, 1_000.0)
        model.classify_failure(request.seq, RequestState.FAILED_NO_EFFECT)

        self.assertEqual(model.actuators["bat"].requested_setpoint_watts, 0.0)

    def test_queued_request_can_be_superseded_before_send(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0.0)]
        )
        first = model.queue_request(
            "bat",
            RequestDomain.STORAGE,
            target_setpoint_watts=500.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
        )
        second = model.queue_request(
            "bat",
            RequestDomain.STORAGE,
            target_setpoint_watts=800.0,
            effective_delta_watts=800.0,
            storage_effective_delta_watts=800.0,
            global_effective_delta_watts=800.0,
        )

        self.assertEqual(first.state, RequestState.SUPERSEDED_BEFORE_SEND)
        self.assertEqual(second.state, RequestState.QUEUED)
        self.assertEqual(model.actuators["bat"].requested_setpoint_watts, 800.0)

    def test_queued_request_enters_forecast_but_is_not_visible_until_sent(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0.0)]
        )
        request = model.queue_request(
            "bat",
            RequestDomain.STORAGE,
            target_setpoint_watts=500.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
        )

        snapshot = MeterSnapshot(physical_grid_watts=500.0, meter_sample_millis=1_500)
        queued_forecast = model.forecast(snapshot, ForecastDomain.STORAGE)
        self.assertEqual(queued_forecast.pending_count, 1)
        self.assertEqual(queued_forecast.grid_min_watts, 0.0)
        self.assertEqual(queued_forecast.grid_max_watts, 0.0)

        model.mark_queued_request_sent(
            request.seq,
            sent_millis=1_000,
            state=RequestState.ACCEPTED,
            latest_effect_millis=4_000,
        )
        sent_forecast = model.forecast(snapshot, ForecastDomain.STORAGE)
        self.assertEqual(sent_forecast.pending_count, 1)
        self.assertEqual(sent_forecast.grid_min_watts, 0.0)
        self.assertEqual(sent_forecast.grid_max_watts, 500.0)

    def test_latest_effect_advances_safe_setpoint_and_closes_pending_corridor(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0.0)]
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=0.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )

        model.advance_ledger(meter_sample_millis=4_000)
        forecast = model.forecast(
            MeterSnapshot(physical_grid_watts=0.0, meter_sample_millis=4_000),
            ForecastDomain.STORAGE,
        )

        self.assertEqual(model.actuators["bat"].safe_setpoint_watts, 500.0)
        self.assertEqual(forecast.pending_count, 0)
        self.assertEqual(forecast.grid_min_watts, 0.0)
        self.assertEqual(forecast.grid_max_watts, 0.0)

    def test_pruning_drops_settled_requests_and_keeps_active_ones(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0.0)]
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=0.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=500.0,
            target_setpoint_watts=300.0,
            effective_delta_watts=-200.0,
            storage_effective_delta_watts=-200.0,
            global_effective_delta_watts=-200.0,
            sent_millis=4_500,
            latest_effect_millis=8_000,
        )

        model.advance_ledger(meter_sample_millis=4_000)
        model.prune_inactive_requests()

        self.assertEqual(len(model.ledger), 1)
        self.assertEqual(model.ledger[0].target_setpoint_watts, 300.0)

    def test_ambiguous_send_failure_remains_in_corridor(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0.0)]
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=0.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
            state=RequestState.FAILED_AMBIGUOUS,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )

        snapshot = MeterSnapshot(physical_grid_watts=500.0, meter_sample_millis=1_500)
        forecast = model.forecast(snapshot, ForecastDomain.STORAGE)

        self.assertEqual(forecast.pending_count, 1)
        self.assertEqual(forecast.grid_min_watts, 0.0)
        self.assertEqual(forecast.grid_max_watts, 500.0)

    def test_solar_capacity_limited_increase_without_headroom_does_not_block_storage(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=300.0)]
        )
        model.append_physical_request(
            "solar",
            RequestDomain.STORAGE,
            base_setpoint_watts=300.0,
            target_setpoint_watts=800.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
            effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
            sent_millis=1_000,
            latest_effect_millis=4_000,
            capacity_full_output_possible=False,
        )

        snapshot = MeterSnapshot(physical_grid_watts=500.0, meter_sample_millis=1_500)
        forecast = model.forecast(snapshot, ForecastDomain.STORAGE)
        decision = decide_correction(0.0, forecast)

        self.assertEqual(forecast.grid_min_watts, 500.0)
        self.assertEqual(forecast.grid_max_watts, 500.0)
        self.assertEqual(decision.action, CorrectionAction.MORE_EFFECTIVE_OUTPUT)
        self.assertEqual(decision.residual_effective_watts, 500.0)

    def test_deterministic_earlier_request_is_not_optional_because_later_request_is_uncertain(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=0.0)]
        )
        model.append_physical_request(
            "solar",
            RequestDomain.STORAGE,
            base_setpoint_watts=0.0,
            target_setpoint_watts=300.0,
            effective_delta_watts=300.0,
            storage_effective_delta_watts=300.0,
            global_effective_delta_watts=300.0,
            sent_millis=1_000,
            not_before_effect_millis=2_000,
            latest_effect_millis=4_000,
        )
        model.append_physical_request(
            "solar",
            RequestDomain.STORAGE,
            base_setpoint_watts=300.0,
            target_setpoint_watts=800.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
            effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
            sent_millis=1_100,
            not_before_effect_millis=2_000,
            latest_effect_millis=4_100,
            capacity_full_output_possible=True,
        )

        forecast = model.forecast(
            MeterSnapshot(physical_grid_watts=0.0, meter_sample_millis=1_500),
            ForecastDomain.STORAGE,
        )

        self.assertEqual(forecast.grid_min_watts, -800.0)
        self.assertEqual(forecast.grid_max_watts, -300.0)

    def test_solar_reduction_is_deterministic_even_with_capacity_limited_kind(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=800.0)]
        )
        model.append_physical_request(
            "solar",
            RequestDomain.STORAGE,
            base_setpoint_watts=800.0,
            target_setpoint_watts=300.0,
            effective_delta_watts=-500.0,
            storage_effective_delta_watts=-500.0,
            global_effective_delta_watts=-500.0,
            effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )

        forecast = model.forecast(
            MeterSnapshot(physical_grid_watts=-500.0, meter_sample_millis=1_500),
            ForecastDomain.STORAGE,
        )

        self.assertEqual(forecast.grid_min_watts, -500.0)
        self.assertEqual(forecast.grid_max_watts, 0.0)

    def test_charger_startup_uses_longer_hard_not_before_window(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("charger", ActuatorKind.CHARGER, safe_setpoint_watts=0.0)]
        )
        request = model.append_physical_request(
            "charger",
            RequestDomain.STORAGE,
            base_setpoint_watts=0.0,
            target_setpoint_watts=1_200.0,
            effective_delta_watts=-1_200.0,
            storage_effective_delta_watts=-1_200.0,
            global_effective_delta_watts=-1_200.0,
            transition_kind=TransitionKind.STARTUP,
            sent_millis=1_000,
            not_before_effect_millis=8_000,
            latest_effect_millis=15_000,
        )

        before_startup_can_affect_meter = model.forecast(
            MeterSnapshot(physical_grid_watts=0.0, meter_sample_millis=5_000),
            ForecastDomain.STORAGE,
        )

        self.assertEqual(request.transition_kind, TransitionKind.STARTUP)
        self.assertEqual(before_startup_can_affect_meter.grid_min_watts, 1_200.0)
        self.assertEqual(before_startup_can_affect_meter.grid_max_watts, 1_200.0)

    def test_solar_startup_from_standby_keeps_capacity_uncertainty_until_timeout(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=0.0)]
        )
        model.append_physical_request(
            "solar",
            RequestDomain.STORAGE,
            base_setpoint_watts=0.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
            effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
            transition_kind=TransitionKind.STARTUP,
            sent_millis=1_000,
            not_before_effect_millis=8_000,
            latest_effect_millis=15_000,
            capacity_full_output_possible=True,
        )

        early = model.forecast(
            MeterSnapshot(physical_grid_watts=500.0, meter_sample_millis=5_000),
            ForecastDomain.STORAGE,
        )
        self.assertEqual(early.grid_min_watts, 0.0)
        self.assertEqual(early.grid_max_watts, 500.0)
        self.assertEqual(decide_correction(0.0, early).action, CorrectionAction.HOLD)

        model.advance_ledger(meter_sample_millis=15_000)
        expired = model.forecast(
            MeterSnapshot(physical_grid_watts=500.0, meter_sample_millis=15_000),
            ForecastDomain.STORAGE,
        )
        decision = decide_correction(0.0, expired)

        self.assertEqual(expired.pending_count, 0)
        self.assertEqual(decision.action, CorrectionAction.MORE_EFFECTIVE_OUTPUT)

    def test_solar_probe_blocks_until_short_latest_then_releases_storage(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=300.0)]
        )
        model.append_physical_request(
            "solar",
            RequestDomain.STORAGE,
            base_setpoint_watts=300.0,
            target_setpoint_watts=800.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
            effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
            sent_millis=1_000,
            latest_effect_millis=2_000,
            capacity_full_output_possible=True,
        )

        before = model.forecast(
            MeterSnapshot(physical_grid_watts=500.0, meter_sample_millis=1_500),
            ForecastDomain.STORAGE,
        )
        self.assertEqual(decide_correction(0.0, before).action, CorrectionAction.HOLD)

        model.advance_ledger(meter_sample_millis=2_000)
        after = model.forecast(
            MeterSnapshot(physical_grid_watts=500.0, meter_sample_millis=2_000),
            ForecastDomain.STORAGE,
        )
        decision = decide_correction(0.0, after)

        self.assertEqual(after.pending_count, 0)
        self.assertEqual(decision.action, CorrectionAction.MORE_EFFECTIVE_OUTPUT)
        self.assertEqual(decision.residual_effective_watts, 500.0)

    def test_grid_only_solar_request_does_not_block_storage_forecast(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=300.0)]
        )
        model.append_physical_request(
            "solar",
            RequestDomain.GRID_ONLY,
            base_setpoint_watts=300.0,
            target_setpoint_watts=600.0,
            effective_delta_watts=300.0,
            storage_effective_delta_watts=0.0,
            global_effective_delta_watts=300.0,
            effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )

        snapshot = MeterSnapshot(
            physical_grid_watts=-300.0,
            meter_sample_millis=1_500,
            proven_solar_only_effective_watts=300.0,
        )
        storage_forecast = model.forecast(snapshot, ForecastDomain.STORAGE)
        global_forecast = model.forecast(snapshot, ForecastDomain.GLOBAL)

        self.assertEqual(storage_forecast.pending_count, 1)
        self.assertTrue(dynamic_target_learning_allowed(storage_forecast))
        self.assertEqual(global_forecast.pending_count, 1)

    def test_visible_unproven_grid_only_solar_does_not_trigger_storage_backoff(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=300.0)]
        )
        model.append_physical_request(
            "solar",
            RequestDomain.GRID_ONLY,
            base_setpoint_watts=300.0,
            target_setpoint_watts=600.0,
            effective_delta_watts=300.0,
            storage_effective_delta_watts=0.0,
            global_effective_delta_watts=300.0,
            effect_kind=EffectKind.SOLAR_CAPACITY_LIMITED,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )

        storage_forecast = model.forecast(
            MeterSnapshot(physical_grid_watts=-300.0, meter_sample_millis=1_500),
            ForecastDomain.STORAGE,
        )

        self.assertEqual(storage_forecast.grid_min_watts, -300.0)
        self.assertEqual(storage_forecast.grid_max_watts, 0.0)
        self.assertEqual(
            decide_correction(0.0, storage_forecast).action,
            CorrectionAction.HOLD,
        )

    def test_mixed_solar_forecast_uses_storage_part_for_storage_domain(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("solar", ActuatorKind.SOLAR, safe_setpoint_watts=300.0)]
        )
        model.append_physical_request(
            "solar",
            RequestDomain.MIXED,
            base_setpoint_watts=300.0,
            target_setpoint_watts=800.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=300.0,
            global_effective_delta_watts=500.0,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )

        snapshot = MeterSnapshot(physical_grid_watts=500.0, meter_sample_millis=1_500)
        storage_forecast = model.forecast(snapshot, ForecastDomain.STORAGE)
        global_forecast = model.forecast(snapshot, ForecastDomain.GLOBAL)

        self.assertEqual(storage_forecast.grid_min_watts, 200.0)
        self.assertEqual(storage_forecast.grid_max_watts, 700.0)
        self.assertEqual(global_forecast.grid_min_watts, 0.0)
        self.assertEqual(global_forecast.grid_max_watts, 500.0)

    def test_charger_increase_raises_meter_and_reduces_effective_output(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("charger", ActuatorKind.CHARGER, safe_setpoint_watts=300.0)]
        )
        request = model.append_physical_request(
            "charger",
            RequestDomain.STORAGE,
            base_setpoint_watts=300.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=-200.0,
            storage_effective_delta_watts=-200.0,
            global_effective_delta_watts=-200.0,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )

        forecast = model.forecast(
            MeterSnapshot(physical_grid_watts=0.0, meter_sample_millis=1_500),
            ForecastDomain.STORAGE,
        )

        self.assertEqual(request.setpoint_delta_watts, 200.0)
        self.assertEqual(request.meter_delta_watts, 200.0)
        self.assertEqual(forecast.grid_min_watts, 0.0)
        self.assertEqual(forecast.grid_max_watts, 200.0)

    def test_charger_minimum_power_and_self_consumption_are_part_of_effective_delta(self) -> None:
        charger = ActuatorState(
            "charger",
            ActuatorKind.CHARGER,
            safe_setpoint_watts=0.0,
            capabilities=ActuatorCapabilities(
                min_active_setpoint_watts=300.0,
                max_setpoint_watts=1_200.0,
                self_consumption_watts=50.0,
            ),
        )

        legal_target = charger.legalize_setpoint(100.0)
        effective_delta = (
            charger.effective_output_for_setpoint(legal_target)
            - charger.effective_output_for_setpoint(charger.safe_setpoint_watts)
        )

        self.assertEqual(legal_target, 300.0)
        self.assertEqual(effective_delta, -350.0)
        self.assertEqual(-effective_delta, 350.0)

    def test_inverter_minimum_power_coerces_small_positive_setpoint(self) -> None:
        battery = ActuatorState(
            "bat",
            ActuatorKind.BATTERY,
            safe_setpoint_watts=0.0,
            capabilities=ActuatorCapabilities(min_active_setpoint_watts=150.0),
        )

        legal_target = battery.legalize_setpoint(80.0)
        effective_delta = (
            battery.effective_output_for_setpoint(legal_target)
            - battery.effective_output_for_setpoint(battery.safe_setpoint_watts)
        )

        self.assertEqual(legal_target, 150.0)
        self.assertEqual(effective_delta, 150.0)

    def test_parallel_independent_actuators_are_combined_by_summing_effects(self) -> None:
        model = PredictiveControlModel(
            [
                ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0.0),
                ActuatorState(
                    "charger", ActuatorKind.CHARGER, safe_setpoint_watts=300.0
                ),
            ]
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=0.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )
        model.append_physical_request(
            "charger",
            RequestDomain.STORAGE,
            base_setpoint_watts=300.0,
            target_setpoint_watts=0.0,
            effective_delta_watts=300.0,
            storage_effective_delta_watts=300.0,
            global_effective_delta_watts=300.0,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )

        forecast = model.forecast(
            MeterSnapshot(physical_grid_watts=800.0, meter_sample_millis=1_500),
            ForecastDomain.STORAGE,
        )

        self.assertEqual(forecast.grid_min_watts, 0.0)
        self.assertEqual(forecast.grid_max_watts, 800.0)

    def test_mixed_solar_allocation_is_storage_first(self) -> None:
        allocation = allocate_mixed_solar(
            storage_part_watts=300.0,
            solar_only_part_watts=200.0,
            possible_solar_effective_watts=400.0,
        )

        self.assertEqual(allocation.storage_effective_watts, 300.0)
        self.assertEqual(allocation.solar_only_effective_watts, 100.0)
        self.assertEqual(allocation.storage_shortfall_watts, 0.0)
        self.assertEqual(allocation.solar_only_shortfall_watts, 100.0)

    def test_dynamic_target_learning_is_allowed_while_storage_pending(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=0.0)]
        )
        model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=0.0,
            target_setpoint_watts=500.0,
            effective_delta_watts=500.0,
            storage_effective_delta_watts=500.0,
            global_effective_delta_watts=500.0,
            sent_millis=1_000,
            latest_effect_millis=4_000,
        )

        forecast = model.forecast(
            MeterSnapshot(physical_grid_watts=500.0, meter_sample_millis=1_500),
            ForecastDomain.STORAGE,
        )

        self.assertTrue(dynamic_target_learning_allowed(forecast))

    def test_hysteresis_expands_no_command_band(self) -> None:
        forecast = model_free_forecast(grid_min=100.0, grid_max=200.0)

        self.assertEqual(
            decide_correction(90.0, forecast, hysteresis_watts=10.0).action,
            CorrectionAction.HOLD,
        )

        decision = decide_correction(89.0, forecast, hysteresis_watts=10.0)
        self.assertEqual(decision.action, CorrectionAction.MORE_EFFECTIVE_OUTPUT)
        self.assertEqual(decision.residual_effective_watts, 11.0)

    def test_solar_only_correction_uses_no_effect_edge_for_increases(self) -> None:
        forecast = model_free_forecast(grid_min=-223.0, grid_max=-12.0)

        self.assertEqual(
            decide_correction(-215.0, forecast).action,
            CorrectionAction.HOLD,
        )

        decision = decide_solar_only_correction(-215.0, forecast)
        self.assertEqual(decision.action, CorrectionAction.MORE_EFFECTIVE_OUTPUT)
        self.assertEqual(decision.residual_effective_watts, 203.0)

    def test_expected_meter_value_requires_collapsed_forecast(self) -> None:
        uncertain = model_free_forecast(grid_min=-175.0, grid_max=-4.0)
        self.assertIsNone(deterministic_expected_meter_value(uncertain))

        deterministic = model_free_forecast(grid_min=-61.0, grid_max=-61.0)
        self.assertEqual(deterministic_expected_meter_value(deterministic), -61.0)

    def test_target_ownership_rejects_or_clamps_invalid_global_target(self) -> None:
        self.assertEqual(global_target_from_solar_offset(0.0, 300.0), -300.0)

        with self.assertRaises(ValueError):
            validate_target_ownership(storage_target_watts=0.0, global_target_watts=100.0)

        self.assertEqual(
            validate_target_ownership(
                storage_target_watts=0.0,
                global_target_watts=100.0,
                clamp=True,
            ),
            0.0,
        )

    def test_solar_only_export_waits_for_storage_absorption_headroom(self) -> None:
        self.assertEqual(
            proven_solar_only_effective_output(
                physical_grid_watts=-121.0,
                storage_target_watts=0.0,
                solar_only_export_offset_watts=200.0,
                storage_absorption_headroom_watts=300.0,
                solar_effective_output_watts=463.0,
            ),
            0.0,
        )
        self.assertEqual(
            proven_solar_only_effective_output(
                physical_grid_watts=-121.0,
                storage_target_watts=0.0,
                solar_only_export_offset_watts=200.0,
                storage_absorption_headroom_watts=50.0,
                solar_effective_output_watts=463.0,
            ),
            71.0,
        )

    def test_safety_override_is_still_a_ledger_request(self) -> None:
        model = PredictiveControlModel(
            [ActuatorState("bat", ActuatorKind.BATTERY, safe_setpoint_watts=500.0)]
        )
        request = model.append_physical_request(
            "bat",
            RequestDomain.STORAGE,
            base_setpoint_watts=500.0,
            target_setpoint_watts=0.0,
            effective_delta_watts=-500.0,
            storage_effective_delta_watts=-500.0,
            global_effective_delta_watts=-500.0,
            cause=RequestCause.SAFETY,
            sent_millis=1_000,
            latest_effect_millis=6_000,
        )

        forecast = model.forecast(
            MeterSnapshot(physical_grid_watts=-500.0, meter_sample_millis=1_500),
            ForecastDomain.STORAGE,
        )

        self.assertEqual(request.cause, RequestCause.SAFETY)
        self.assertEqual(forecast.pending_count, 1)


class HouseholdSimulationTest(unittest.TestCase):
    def test_charger_input_is_physical_consumption_only(self) -> None:
        from household_report import HouseholdSimulation

        simulation = HouseholdSimulation()
        simulation.run()

        charger_inputs = [row.charger_input_w for row in simulation.rows]
        self.assertGreaterEqual(min(charger_inputs), 0.0)

    def test_battery_and_charger_are_not_physically_active_together(self) -> None:
        from household_report import HouseholdSimulation

        simulation = HouseholdSimulation()
        simulation.run()

        overlaps = [
            row
            for row in simulation.rows
            if row.battery_output_w > 0.5 and row.charger_input_w > 0.5
        ]
        self.assertEqual(overlaps, [])

    def test_solar_is_not_throttled_while_battery_inverter_is_active(self) -> None:
        from household_report import HouseholdSimulation

        simulation = HouseholdSimulation()
        simulation.run()

        violations = [
            row
            for row in simulation.rows
            if row.battery_output_w > 0.5
            and row.solar_capacity_w > row.solar_output_w + 1.0
        ]
        self.assertEqual(violations, [])

    def test_battery_export_below_storage_target_requires_reduction_request(self) -> None:
        from household_report import (
            ASSERTION_TOLERANCE_W,
            CONTROL_INTERVAL_MS,
            STORAGE_TARGET_W,
            HouseholdSimulation,
        )

        simulation = HouseholdSimulation()
        simulation.run()

        violations = [
            row
            for row in simulation.rows
            if row.battery_output_w > 0.5
            and row.t_ms % CONTROL_INTERVAL_MS == 0
            and row.storage_max_w < STORAGE_TARGET_W - ASSERTION_TOLERANCE_W
            and row.battery_requested_w >= row.battery_output_w - 0.5
            and not row.note.startswith("battery reduce")
        ]
        self.assertEqual(violations, [])

    def test_battery_physical_export_requires_solar_only_allocation(self) -> None:
        from household_report import (
            ASSERTION_TOLERANCE_W,
            CONTROL_INTERVAL_MS,
            STORAGE_TARGET_W,
            HouseholdSimulation,
        )

        simulation = HouseholdSimulation()
        simulation.run()

        violations = [
            row
            for row in simulation.rows
            if row.battery_output_w > 0.5
            and row.t_ms % CONTROL_INTERVAL_MS == 0
            and row.grid_w < STORAGE_TARGET_W - ASSERTION_TOLERANCE_W
            and row.proven_solar_only_w <= 1.0
            and row.battery_requested_w >= row.battery_output_w - 0.5
            and not row.note.startswith("battery reduce")
        ]
        self.assertEqual(violations, [])

    def test_actuator_chart_contains_only_non_negative_physical_series(self) -> None:
        from household_report import HouseholdSimulation

        simulation = HouseholdSimulation()
        simulation.run()

        for row in simulation.rows:
            for value in (
                row.solar_capacity_w,
                row.solar_limit_w,
                row.solar_output_w,
                row.battery_setpoint_w,
                row.battery_output_w,
                row.charger_setpoint_w,
                row.charger_input_w,
            ):
                self.assertGreaterEqual(value, 0.0)

    def test_solar_output_respects_limit_and_capacity(self) -> None:
        from household_report import HouseholdSimulation

        simulation = HouseholdSimulation()
        simulation.run()

        violations = [
            row
            for row in simulation.rows
            if row.solar_output_w > row.solar_capacity_w + 1.0
            or row.solar_output_w > row.solar_limit_w + 1.0
        ]
        self.assertEqual(violations, [])

    def test_active_setpoints_respect_minimum_power(self) -> None:
        from household_report import (
            HouseholdSimulation,
            MIN_BATTERY_OUTPUT_W,
            MIN_CHARGER_INPUT_W,
        )

        simulation = HouseholdSimulation()
        simulation.run()

        violations = [
            row
            for row in simulation.rows
            if (0.5 < row.battery_setpoint_w < MIN_BATTERY_OUTPUT_W - 0.5)
            or (0.5 < row.charger_setpoint_w < MIN_CHARGER_INPUT_W - 0.5)
        ]
        self.assertEqual(violations, [])

    def test_grid_power_matches_physical_power_balance(self) -> None:
        from household_report import HouseholdSimulation

        simulation = HouseholdSimulation()
        simulation.run()

        max_error = max(
            abs(
                row.grid_w
                - (
                    row.load_w
                    + row.charger_input_w
                    - row.solar_output_w
                    - row.battery_output_w
                )
            )
            for row in simulation.rows
        )
        self.assertLessEqual(max_error, 0.001)


def model_free_forecast(grid_min: float, grid_max: float):
    from predictive_control import ControlForecast

    return ControlForecast(
        valid=True,
        domain=ForecastDomain.STORAGE,
        meter_sample_millis=0,
        virtual_meter_watts=(grid_min + grid_max) / 2.0,
        grid_min_watts=grid_min,
        grid_nominal_watts=(grid_min + grid_max) / 2.0,
        grid_max_watts=grid_max,
        pending_count=0,
    )


if __name__ == "__main__":
    unittest.main()
