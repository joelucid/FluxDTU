# Power Limiter Predictive Control Specification

This document specifies the intended control algorithm for the dynamic power
limiter when solar inverters, battery inverters, smart-buffer inverters, and a
grid charger are controlled together.

It is a specification for the controller behavior. It intentionally separates
the control model from current implementation details.

## Goals

The controller shall:

- keep the grid meter close to the configured targets with low latency,
- issue new commands before all previous commands are fully visible when that is
  safe,
- avoid oscillation caused by stale inverter statistics or command
  acknowledgements,
- evaluate storage and solar target corrections in separate regulation steps,
  coupled only through predictive forecasts, and
- make every pending actuator effect explicit and inspectable.

## Sign Conventions

Grid meter power uses the existing OpenDTU sign convention:

- positive grid power means grid import,
- negative grid power means grid export,
- increasing inverter output lowers the grid meter value,
- increasing grid charger input raises the grid meter value.

The specification uses two explicit delta signs.

`effectiveDeltaWatts`:

- positive means more effective output,
- positive lowers the grid meter,
- inverter output increase is positive,
- grid charger input reduction is positive.

`meterDeltaWatts`:

- positive means the grid meter value rises,
- `meterDeltaWatts = -effectiveDeltaWatts`.

Examples:

```text
Battery inverter: 700 W -> 900 W
setpointDeltaWatts  = +200
effectiveDeltaWatts = +200
meterDeltaWatts     = -200

Grid charger: 300 W -> 500 W
setpointDeltaWatts  = +200
effectiveDeltaWatts = -200
meterDeltaWatts     = +200
```

Request fields must use these names or define equivalent signs explicitly.
Ambiguous names such as `gridDeltaWatts` are not allowed without a documented
sign convention.

## Target Model

The controller has two targets and two normal regulation steps.

### Storage Target

The storage target is the grid meter value that storage-backed devices regulate
to.

Storage-backed devices are:

- grid charger,
- battery inverters,
- smart-buffer inverters.

The storage controller does not command solar. It only chooses setpoints for
storage-backed devices. Solar regulation runs afterwards.

### Global Grid Target

The global grid target may include an export offset below the storage target.
The solar controller regulates the global target with solar inverters after the
storage controller has planned its changes.

Example:

- storage target: `0 W`,
- global grid target: `-300 W`,
- the `-300 W` export offset belongs to the global target.

With the OpenDTU sign convention the global target must be less than or equal
to the storage target:

```text
globalTargetWatts = storageTargetWatts - exportOffsetWatts
exportOffsetWatts >= 0
```

If configuration produces `globalTargetWatts > storageTargetWatts`, the
controller must reject the configuration or clamp the global target to the
storage target. This is target ordering, not actuator priority.

## Forecast Domains

The physical grid meter is the input for both regulation steps.

```text
globalDomainMeter = physicalGridMeter
storageDomainMeter = physicalGridMeter
```

Both forecasts apply all pending physical meter effects from the predictive
ledger, regardless of whether the command came from the storage step or the
solar step. They are both forecasts of the real grid meter. The domain label
only determines which target and correction step consumes the forecast.

The storage-domain forecast is compared to the storage target. The global-domain
forecast is compared to the global grid target. The solar step runs after the
storage step and therefore sees storage changes that were planned earlier in
the same controller pass.

## Control Priority

When more effective output is needed in the storage domain, apply controls in
this order:

1. increase smart-buffer output,
2. reduce grid charger input,
3. increase battery output.

When less effective output is needed in the storage domain, apply controls in
this order:

1. reduce battery output, including standby when allowed,
2. reduce smart-buffer output,
3. increase grid charger input.

The controller derives storage-backed changes first. It then refreshes the
global forecast with the planned storage effects and lets the solar controller
derive the solar target contribution from that updated forecast.

The exact distribution between multiple inverters of the same class may still
use existing weighting rules such as thermal balancing.

The priority order is also a physical mutual-exclusion constraint for storage
devices during normal regulation: grid charger input and battery inverter output
must not be physically active at the same time. When control changes direction
from charger input to battery output, or from battery output to charger input,
the controller must keep the opposite actuator blocked until the current
actuator is physically off or still represented as pending-off in the ledger.

Solar curtailment is not a storage-domain option. If the storage controller
cannot fully reach the storage target with storage-backed devices, it leaves the
remaining storage-domain error for the next pass. The solar controller still
runs afterwards and independently corrects the global target with solar.

At every control evaluation, if battery output is active and the storage-domain
forecast is below the storage target, the controller must request a battery
reduction before any other normal-regulation action. Physical grid samples may
briefly remain below the target while the reduction is in flight, but the
requested battery setpoint must already be moving down.

## Actuator State

Each physical actuator has a stable identity called `actuatorKey`.

Examples:

- solar inverter serial,
- battery inverter serial,
- smart-buffer inverter serial,
- grid charger provider instance.

For every actuator the controller keeps three separate values.

### `safeSetpointWatts`

The latest actuator setpoint that is no longer considered pending.

For deterministic actuators, its expected grid-meter effect may be treated as
part of the safe baseline.

For capacity-limited solar increases, only the limit command is settled. The
actual production effect must still be derived from the current meter sample
and fresh solar telemetry. A settled solar limit is not proof that solar output
rose to that limit.

`safeSetpointWatts` may be advanced only when one of these is true:

- a meter sample is at or after the request's `latestEffectMillis`,
- actuator-specific telemetry proves the command effect with sufficient
  freshness and tolerance,
- the request failed with no possible physical effect and is explicitly
  discarded.

`safeSetpointWatts` is the only setpoint that may be used as the physical
control basis.

### `requestedSetpointWatts`

The latest target the controller wants the actuator to reach.

This value may be updated immediately to suppress duplicate queued commands and
to construct the next queued command. It is not proof of physical output and
must not be used as the safe physical basis.

When a queued or physical request fails, is discarded, or is superseded,
`requestedSetpointWatts` must be recomputed from the remaining non-failed
request chain. It must never suppress a retry solely because a failed request
once targeted that value.

### `measuredOutputWatts`

The latest actuator telemetry, such as inverter AC output or charger input.

Telemetry may lag behind the grid meter. It is useful for diagnostics,
availability, capacity estimates, and detecting failures. It must not overwrite
the safe basis merely because it is newer than a command acknowledgement.

Telemetry may advance the safe state only if its timestamp is at or after the
relevant command send or acknowledgement timestamp, adjusted for the telemetry
source's sampling semantics, and the value is consistent with the requested or
clipped output within configured tolerance.

Battery inverters are especially strict: a command acknowledgement never proves
battery AC output.

## Actuator Capability Constraints

Before a desired correction can become a physical request, it must be mapped to
a legal actuator setpoint.

Each actuator has at least:

```cpp
ActuatorCapabilities {
    minActiveSetpointWatts;
    maxSetpointWatts;
    standbySetpointWatts;
    selfConsumptionWatts;
}
```

For inverters, a positive setpoint below `minActiveSetpointWatts` is not a
stable physical operating point. The controller must either command standby or
the minimum active setpoint, according to the selected correction policy.

For the grid charger, the minimum charging power and the charger's own
self-consumption are part of the physical grid-meter effect. If the charger is
started from standby at `300 W` minimum charging power and has `50 W`
self-consumption, the effective-output delta is `-350 W` and the meter delta is
`+350 W`.

Capability coercion happens before the request is added to the physical
pending ledger. The request must record the legal target setpoint and the
effective/meter deltas derived from that legal target, not only the unclamped
desired correction.

## Pending Request Ledger

Every physical setpoint command is represented by a request ledger entry.

Queued requests may be coalesced or superseded before sending according to a
deterministic rule, because their effect is definitely not yet visible in the
grid meter.

Queued requests update `requestedSetpointWatts` and participate in the
forecast as the future target state. Their visible prefix contribution is
always zero until the command is actually sent. The send timestamp, not the
planning timestamp, starts the command latency window.

All later correction decisions must be based on a forecast that includes
remaining queued requests. This is not limited to duplicate suppression for the
same actuator: a queued charger increase, solar reduction, battery reduction, or
other actuator change must reduce the residual available to later storage and
global corrections. The request leaves the forecast only when it is superseded,
discarded, failed with no physical effect, or settled by the normal ledger rules.

Sent or accepted requests are physical active requests. They must not be
silently overwritten, deleted, or merged except by explicit settlement, failure
classification, or recorded supersession.

Each request contains at least:

```cpp
PendingControlRequest {
    seq;                    // monotonically increasing id
    actuatorKey;            // type + serial/provider id
    domain;                 // Storage or Global
    cause;                  // Normal, Safety, Manual, Recovery
    state;                  // see Request States
    effectKind;             // Deterministic, SolarCapacityLimited, Ambiguous

    baseSetpointWatts;      // previous requested setpoint for this actuator
    targetSetpointWatts;    // target setpoint for this command
    setpointDeltaWatts;     // target - base in actuator-native sign

    effectiveDeltaWatts;        // positive lowers the grid meter
    meterDeltaWatts;            // positive raises the grid meter

    createdMillis;
    sentMillis;
    ackMillis;
    notBeforeEffectMillis;      // optional hard lower bound
    earliestExpectedMillis;     // soft timing estimate
    typicalEffectMillis;
    latestEffectMillis;
}
```

## Request States

The request state model must distinguish physical no-effect failures from
ambiguous failures.

Required states or equivalent state plus flags:

- `Queued`: desired target exists, no physical command sent yet; future effect
  is forecast, visible effect is zero.
- `SupersededBeforeSend`: queued request was replaced before transmission.
- `Sent`: command was transmitted, result not known yet.
- `Accepted`: provider or device acknowledged the command.
- `Rejected`: explicit no-effect response.
- `FailedNoEffect`: safe to discard; command definitely did not reach actuator.
- `FailedAmbiguous`: command may have reached actuator; effect remains possible.
- `SettledByMeter`: settled because meter sample is at or after latest effect.
- `SettledByTelemetry`: settled by fresh actuator telemetry.
- `SettledByTimeout`: settled by latest-effect timeout without better proof.
- `SettledSuperseded`: settled because a later absolute setpoint superseded it.
- `SettledDiscarded`: removed without committing an effect.

At minimum, failures need:

```cpp
failureEffectCertainty; // NoEffect, Ambiguous, EffectPossible
```

Ambiguous failures must remain in the uncertainty corridor until they can be
classified or settled. They must not be silently discarded as no-effect
failures.

## Multiple Requests For One Actuator

Multiple active physical requests for the same actuator are allowed and must be
ordered by `seq`.

They must not be treated as independent binary effects. A chain of absolute
setpoint commands describes possible actuator setpoints, not arbitrary sums.

Example:

- safe setpoint is `700 W`,
- request A targets `300 W`,
- request B targets `500 W`.

Possible actuator states are:

- `700 W` if neither request is visible,
- `300 W` if A is visible but B is not,
- `500 W` if B is visible.

The impossible state `900 W` must never appear from adding B's `+200 W` delta
without A's `-400 W` delta.

For a single actuator, active physical requests are evaluated in `seq` order as
an absolute setpoint chain. Settlement may remove earlier requests from the
chain, but must never create a state that was not reachable by applying a
prefix of the ordered command sequence or by applying a later absolute command
that supersedes previous commands.

If a later absolute setpoint request is accepted and settled, earlier unsettled
requests for the same actuator that target intermediate setpoints may be marked
`SettledSuperseded`, provided the implementation can prove:

- the later request was sent after them, and
- the actuator command semantics are last-writer-wins.

Timing windows do not override this sequence invariant. If request B has a
shorter or earlier closing timing window than request A, B may still settle
before A only when the resulting state is reachable by the ordered command
chain or by last-writer-wins supersession. Otherwise the chain remains
uncertain until enough requests can be settled without creating an impossible
physical state.

If an earlier request fails with no possible physical effect and a later
absolute request succeeds, the later request remains valid because it sets the
actuator target absolutely. If the earlier failure is ambiguous, it remains in
the uncertainty model unless the later request can explicitly supersede it.

Uncertainty in a later request may widen only the suffix of that actuator's
ordered chain. Deterministic earlier requests that are still active remain
mandatory final effects; a later capacity-limited solar increase must not make
an earlier deterministic request optional again.

## Timing Model

All request timing fields used for ordering and settlement must be based on the
same monotonic controller clock.

If a meter provides its own remote timestamp, the controller must translate it
into the controller clock domain or use a locally captured sample acquisition
or receipt timestamp with known semantics. Wall-clock time must not be used for
effect ordering unless clock jumps are handled explicitly.

The meter timestamp used for settlement must represent the end of the sample
interval, or a conservative time no earlier than the end of the sample interval.
A request cannot be visible in a meter sample whose acquisition interval ended
before the request was sent.

### `notBeforeEffectMillis`

Optional hard lower bound. If known, a request cannot be visible before this
time.

### `earliestExpectedMillis` And `typicalEffectMillis`

Soft timing estimates. They influence only the nominal forecast. They do not
close the conservative min/max corridor unless a hard `notBeforeEffectMillis`
exists.

### `latestEffectMillis`

For a meter sample at or after this time, the request leaves the uncertainty
set.

If the command was accepted and no failure was detected, deterministic
actuators advance `safeSetpointWatts` to the request target. Any remaining grid
error is treated as a new measured disturbance and corrected by later requests.

If the command failed with no possible effect, the request is discarded and
`safeSetpointWatts` remains unchanged.

If the command failed ambiguously, it remains in the uncertainty model until
classified or until a conservative settlement rule applies.

Startup requests use longer timing windows than ordinary setpoint changes.
Waking a battery inverter from standby or enabling the grid charger is not the
same as changing an already active setpoint.

Startup may be represented either as a single request with a startup timing
profile or as two linked requests:

- availability transition,
- power setpoint transition.

Until startup is confirmed, the actuator's effective capacity is uncertain.

## Solar Capacity Uncertainty

Solar reductions are deterministic: lowering a solar limit can cap existing
production.

Solar increases are capacity-limited: raising a limit only allows more
production. It does not prove irradiance is available.

For solar increase requests, the forecast must include:

- no additional output if irradiance is unavailable,
- full requested output if irradiance is available,
- an optional nominal estimate based on recent solar telemetry.

Capacity-limited solar increases may block storage-backed correction only while
recent solar telemetry indicates plausible headroom or until a short configured
solar-probe latest-effect time expires.

If recent solar telemetry proves insufficient headroom, the full-output branch
of that solar request must not prevent storage-backed devices from correcting
the residual.

## Request Domains

A normal regulation request belongs to exactly one control step:

- `Storage` for storage-backed devices and the storage-domain forecast,
- `Global` for solar devices and the global forecast.

The predictive model forecasts the physical grid meter, so pending deltas from
both domains are visible to both correction steps. The request domain documents
which controller step issued the command; it does not filter the meter delta
out of the other forecast.

## Forecast Semantics

The current meter sample may already include some pending effects. A forecast
must not double-count pending requests by simply adding every pending delta to
the current meter.

For each pending request chain, the forecast evaluates plausible visible states
at the meter sample time and plausible final states after pending settlement.

For each candidate:

```text
futureGrid =
    currentGrid
    - meterDeltaAlreadyVisibleInCurrentSample
    + meterDeltaExpectedAfterSettlement
```

The corridor is the min/max over all valid candidates, combined across
actuators. Same-actuator candidates must obey ordered absolute setpoint-chain
semantics. Distinct actuators may be combined by summing their meter effects.

Forecasts have at least:

```cpp
ControlForecast {
    valid;
    domain;             // Storage or Global
    meterSampleMillis;
    virtualMeterWatts;  // domain-adjusted meter used for solving
    gridMinWatts;       // lowest plausible future meter value
    gridNominalWatts;
    gridMaxWatts;       // highest plausible future meter value
    pendingCount;
}
```

Trace may expose a single expected grid value only when the forecast corridor
has collapsed to one value. For capacity-limited solar or other uncertain
branches, the corridor is the prediction; using the arithmetic mean as an
`expected` line is misleading because it creates a stable offset whenever the
real meter follows one corridor edge.

Forecast domains do not filter physical meter effects. Both storage and global
forecasts start from the physical meter and apply all pending physical deltas
from the ledger. The domain only selects the target and correction rule that
will consume the forecast.

## Correction Rule

Let:

```text
corridor = [gridMinWatts, gridMaxWatts]
gridMinWatts <= gridMaxWatts
hysteresisWatts >= 0
```

No command is issued if:

```text
targetWatts >= gridMinWatts - hysteresisWatts
and
targetWatts <= gridMaxWatts + hysteresisWatts
```

If:

```text
targetWatts < gridMinWatts - hysteresisWatts
```

then more effective output is needed:

```text
residualEffectiveWatts = gridMinWatts - targetWatts
```

If:

```text
targetWatts > gridMaxWatts + hysteresisWatts
```

then less effective output is needed:

```text
residualEffectiveWatts = gridMaxWatts - targetWatts
```

A negative `residualEffectiveWatts` means reduce effective output. A positive
value means increase effective output.

This is the core low-latency rule: the controller may act before all previous
requests have settled, but it must size the new command against the uncertainty
corridor edge, not against stale measured actuator output or the latest
requested setpoint.

## Main Control Loop

Each regulation loop performs these steps.

### 1. Read A Consistent Snapshot

Read:

- grid meter value and meter timestamp,
- actuator telemetry and telemetry timestamps,
- command completion states,
- configuration and battery state.

The grid meter value and timestamp must come from one locked sample.

### 2. Advance The Ledger

For every request:

- mark command failures with effect certainty,
- mark commands as sent or accepted when the device path reports it,
- supersede queued requests before send when appropriate,
- for meter samples at or after `latestEffectMillis`, settle the request,
- update `safeSetpointWatts` only during settlement or explicit confirmation,
- recompute `requestedSetpointWatts` after failure, discard, or supersession.

Queued requests that were superseded before sending do not become physical
pending effects.

Queued requests that are not superseded remain forecast-relevant. They prevent
the controller from re-issuing the same correction on every new meter sample and
from issuing another actuator correction for residual power that an existing
queued request is already expected to cover. They must not be treated as already
visible in the physical meter.

### 3. Compute Forecasts

Compute at least:

- storage forecast,
- global forecast.

Forecasts are derived from domain-adjusted meter samples, safe baselines, and
remaining ordered pending request chains.

### 4. Decide Whether To Plan New Commands

The controller still updates the dynamic battery target, ledger, forecasts, and
trace for every new grid meter sample. It does not have to plan new actuator
commands for every sample.

After a planner run, later samples may be observed without issuing new
commands while the current meter value still matches the planner's expected
pending-request corridor. The configured target-power hysteresis is used as
the planner's tolerance around that corridor.

An immediate planner run is due when the current grid meter is outside the
planner estimate corridor by more than the configured target-power hysteresis,
and that deviation is meaningful relative to the remaining corridor
uncertainty. The deviation is meaningful if either:

- the corridor uncertainty is no larger than the configured target-power
  hysteresis, or
- the outside-corridor deviation divided by the corridor uncertainty reaches
  the configured minimum outside-uncertainty ratio.

This means a large physical meter jump does not by itself force a new actuator
command if the jump is still inside a plausible pending-request corridor. Slow
drift is still planned at least every `5 s`.

A storage or global target change outside the same threshold must also be
planned immediately on the next sample, even if the grid meter itself remains
inside the expected pending-request corridor. Target changes are planner input,
not ledger effects.

The target-power hysteresis is not an actuator-setpoint deadband. It gates
whether a new planner pass is needed while pending requests are still modeled.
Once the planner runs, normal correction decisions use the storage and global
forecast corridors directly unless a separate correction hysteresis is
explicitly configured for that decision rule.

### 5. Regulate Storage Domain

Compare the storage target to the storage forecast.

If the storage target is inside the storage corridor for the active correction
rule, do not command storage-backed devices.

Otherwise command the residual according to the storage priority order.

### 6. Plan Solar Target Contribution

The storage target and the global grid target are evaluated in the same
controller pass, but by separate regulation steps. After deriving the
storage-backed setpoint changes, refresh the global forecast so planned storage
effects are already included. Then compare the global grid target to that
updated solar-target forecast.

Pending requests from earlier controller passes remain part of the forecast and
continue to prevent duplicate commands. Setpoints chosen earlier in the current
controller pass are recorded in the predictive model before the solar step, so
the solar controller sees the storage step's planned meter effect and does not
regulate the same distance twice. The current pass still produces one setpoint
set and commits that set together.

Only solar acts on the global target in this step.

For global solar increases, compare the global target to the no-further-effect
meter value (`virtualMeterWatts`). For a pending solar increase this is the high
edge of the global forecast corridor, so a capacity-limited solar increase does
not stop further solar probing merely because the optimistic full-output edge
would reach the target. For a pending solar reduction it is the low edge; the
controller must not raise solar again only because the possible full-reduction
edge is above the global target.

This probe rule must not override a forecast corridor that is already wholly
below the global target. In that case deterministic pending effects such as a
charger input reduction have already moved the expected meter too far toward
export, so the solar step must reduce solar output instead of probing upward.

Solar target commands are not blocked merely because storage output, charger
input, or storage-domain requests are active. Those effects are already included
in the refreshed global forecast.

### 7. Send Commands

Send queued commands according to device capability and command backoff.

When a command is actually sent or accepted, fill in its timing window and turn
it into a physical pending request.

### 8. Record Trace

Trace data must expose enough state to debug the controller:

- grid meter sample time,
- storage and global virtual meter values,
- storage and global forecast corridors,
- safe, requested, and measured setpoint per actuator class,
- pending request count and oldest/latest timing,
- command state counters,
- correction residual and chosen nearest corridor edge,
- reason no command was issued,
- per-request settlement reason,
- per-request uncertainty kind,
- telemetry ages and stale flags,
- request domain and cause.

## Dynamic Battery Target

The dynamic battery target always learns from valid grid meter samples,
including while storage-relevant requests are pending.

Pending request corridors are used for control decisions and trace
explainability, but they do not freeze dynamic-target learning. This keeps the
dynamic target responsive to actual household behavior even when actuator
effects are still settling.

The planner feed-forward for the dynamic target uses the forecast before
same-pass storage-domain actuator plans are applied. Pending effects from
earlier controller passes still participate, but a grid-charger reduction staged
in the current pass must not lower the dynamic target in that same pass. Solar
then compensates the storage-domain step against the target snapshot that caused
the pass, instead of chasing a target that was moved by the just-planned
actuator effect.

## Safety Overrides

Safety actions may bypass normal target-domain constraints only to reduce risk.

Examples:

- emergency full output for flexible-load emergency stop,
- battery stop threshold forces battery inverter standby,
- invalid grid meter data sheds grid charger input.

Safety actions still need ledger entries if they send physical commands, so the
next normal loop has a correct pending-state model.

`domain` describes which forecast domain a command affects. `cause` identifies
why the command was issued, for example `Safety` or `Recovery`.

## Test Matrix

The implementation should include tests or trace-driven validation for these
cases.

### Global Solar Pending Effect Contributes To Storage Forecast

- storage target: `0 W`,
- global target: `-300 W`,
- physical grid meter: `-300 W`,
- a pending global solar increase may still lower the physical grid meter.

Expected: storage forecast includes the pending global solar meter effect. If
that corridor is below the storage target, storage may correct with
storage-backed devices.

### Battery Pending Command Visible Early

- safe battery setpoint: `0 W`,
- request: `+500 W` effective output,
- meter already drops by `500 W` before `latestEffectMillis`.

Expected: controller waits if target is inside corridor; it does not add
another `500 W`.

### Battery Pending Command Not Visible

- safe battery setpoint: `0 W`,
- request: `+500 W` effective output,
- meter does not change,
- target lies inside pending corridor.

Expected: controller waits until the configured latest effect time, then treats
remaining error as disturbance or failure according to confirmation policy.

### Ordered Chain Avoids Impossible Sum

- safe: `700 W`,
- request A: `300 W`,
- request B: `500 W`.

Expected possible states are `700 W`, `300 W`, and `500 W`; never `900 W`.

### Failed Queued Request Does Not Suppress Retry

- queued command targets `1000 W`,
- send fails before physical transmission.

Expected: request is discarded or marked failed-no-effect; next regulation may
issue a new command.

### Queued Request Dispatch Latency

- request is planned at `t=1000 ms`,
- matching inverter command is physically sent at `t=2500 ms`,
- inverter latency is `2000 ms`.

Expected: the request is forecast as the future target immediately, has zero
visible meter contribution before `t=2500 ms`, and becomes certainly visible
only at `t=4500 ms`.

### Queued Correction Prevents Duplicate Future Correction

- grid meter drops to `-350 W`,
- storage target is `-18 W`,
- charger request from `58 W` to `390 W` is queued but not sent yet.

Expected: the storage forecast includes the queued `+332 W` meter effect and is
inside the storage target. The next correction pass must not also curtail solar
or use another actuator for the same storage residual unless the charger request
is superseded, discarded, failed, or settled with remaining error.

### Queued Solar Backoff Prevents Duplicate Solar Backoff

- global target is `-218 W`,
- grid meter is `-350 W`,
- solar reduction from `562 W` to `430 W` is queued but not sent yet.

Expected: the global forecast includes the queued `+132 W` meter effect and is
inside the global target. The next global correction pass must not issue another
solar backoff for that same residual unless the queued request leaves the
forecast.

### Ambiguous Send Failure Remains Uncertain

- command send times out after possible transmission.

Expected: request remains in uncertainty corridor or is classified ambiguous;
it is not silently discarded.

### Solar Capacity-Limited Increase

- solar limit is raised by `500 W`,
- irradiance is unavailable.

Expected: no false assumption that solar produced `500 W`; storage correction
follows the explicitly configured capacity-limited policy.

### Dynamic Battery Target Learning During Pending Requests

- storage-relevant request pending,
- raw grid meter swings due to pending command.

Expected: dynamic battery target continues learning from valid meter samples.

### Safety Override Ledger Entry

- battery stop threshold forces standby.

Expected: physical command is recorded in the ledger and later normal control
sees the pending standby effect.

### Actuator Minimum Power And Charger Self-Consumption

- battery inverter minimum active power is `150 W`,
- requested positive battery setpoint is `80 W`,
- charger minimum charging power is `300 W`,
- charger self-consumption is `50 W`.

Expected: battery command is coerced to a legal active setpoint or standby
before the request is recorded. Starting the charger records a `-350 W`
effective-output delta and a `+350 W` meter delta.

## Implementation Checklist

The implementation must satisfy these invariants:

- storage-domain and global-domain forecasts use the physical grid meter plus
  all pending physical meter effects,
- `effectiveDeltaWatts` and `meterDeltaWatts` signs are explicit and tested for
  inverter and charger commands,
- `safeSetpointWatts` represents settled actuator setpoint, not necessarily
  proven solar production for capacity-limited solar increases,
- `safeSetpointWatts` is the only physical basis for target solving,
- `requestedSetpointWatts` is never used as proof of output,
- `requestedSetpointWatts` is recomputed after failure, discard, or
  supersession,
- physical pending requests are recorded at send or acknowledgement time, not
  at local target calculation time,
- queued requests participate in future forecasts while contributing zero
  visible meter effect before send,
- multiple requests for one actuator are forecast and settled as an ordered
  absolute setpoint chain,
- ambiguous command failures are not silently discarded,
- storage and global forecasts are separate,
- solar requests use the global domain but their pending physical meter
  deltas still contribute to storage and global forecasts,
- actuator minimum active power, maximum power, standby behavior, and charger
  self-consumption are included before request deltas are recorded,
- battery and grid charger only act in the storage regulation step,
- battery output and grid charger input are mutually exclusive during normal
  regulation, including pending-off transitions,
- a solar target command is not blocked solely because battery output is active
  or pending-active,
- battery output is reduced at the next control evaluation whenever the
  storage-domain forecast is below the storage target,
- dynamic battery target continues learning from valid raw meter samples even
  while storage requests are pending,
- trace output can explain every control decision from safe state, pending
  requests, and forecast corridors.
