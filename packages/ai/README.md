# Poima AI gameplay helpers

This optional compiled C# package provides [behavior trees](BEHAVIOR_TREE.md),
bounded sight cones, line-of-sight through the native `Raycast` service,
explicit last-seen target memory and acceleration-limited yaw steering.
The game chooses targets, sample points, budgets and responses. Behavior
trees execute the game's authored conditions and actions in a fixed order.

The native engine does not install this package for unrelated games. It adds no
native ABI, service marker or state-field type.

## Sight

```csharp
var observer = new SightObserver(eye, forward, new Vector3d(0, 1, 0));
var settings = SightSettings.YawPitch(horizontalHalfAngle: 0.70,
    verticalHalfAngle: 0.45, minRange: 0.25, maxRange: 12);
ReadOnlySpan<Vector3d> points = [torsoCenter, headCenter];   // 1..8, evaluated in order
ReadOnlySpan<EntityId> target = [torsoCollider, headCollider]; // every target collider
ReadOnlySpan<EntityId> self = [postCollider, visorCollider];   // every self collider
Span<SightSample> samples = stackalloc SightSample[2];
SightResult sight = Sight.Evaluate(observer, settings, points, target, self,
    rayBudget: 4, samples, context);
```

Units are metres and radians. `Forward` and `Up` must be unit vectors within
`1e-6` and not parallel. `Up` is a reference that is orthogonalized against
`Forward`. Angles are relative to the supplied gaze. To measure elevation from
the horizon, pass a level heading. Each sample must satisfy all of these:

- an inclusive range from `MinRange` to `MaxRange`, where
  `.001 <= MinRange <= MaxRange <= 10000`;
- `Angle <= HalfAngle`, where Angle is the cone angle off `Forward`, from 0 to π;
- `|Azimuth| <= HorizontalHalfAngle`, from 0 to π. Positive azimuth is toward
  the observer's right (`Forward × Up`);
- `|Elevation| <= VerticalHalfAngle`, from 0 to π/2.

`Cone(...)` disables the yaw/pitch limits by setting them to π and π/2, and
`YawPitch(...)` disables the cone by setting it to π. Rejections are
reported in this order: `Coincident`, `TooClose`/`TooFar`, `OutsideCone`,
`OutsideHorizontal`, `OutsideVertical`. A point that coincides with the eye is
always rejected and all of its diagnostics are 0. Directly along local up or down,
azimuth is defined as 0. Angles use `atan2`, so they have no tangent singularity
at 90°. `Sight.Classify` performs the same validation and measurement for one
point without making any query.

Each accepted sample casts one ray from the eye exactly to the point, while
budget remains. The closest collider determines the result:

| Sample status | Meaning |
| --- | --- |
| `TargetHit` | First collider is one of the declared target colliders. |
| `Clear` | No collider through the point, endpoint included. |
| `Occluded` | Any other collider, including an undeclared part of a compound target. A hit at distance 0 means that the eye starts inside a collider, typically an unignored self collider. |
| `Rejected` | Geometry rejected the sample (see `Geometry.Reject`); no ray. |
| `BudgetExhausted` | The sample was accepted, but `rayBudget` was spent; no ray and no visibility claim. |
| `Skipped` | `FirstVisible` sampling had already established sight; no ray. |

`SightStatus` is `Visible` if any sample is Clear or TargetHit. `OutOfView`
means every sample was geometrically rejected, so no ray was cast. `Occluded`
requires every accepted sample to have been cast. If any accepted sample went
unchecked and none is visible, the status is `Inconclusive`. Clear and
TargetHit both establish sight. For a stricter policy, inspect
`ClearSamples`/`TargetHitSamples`. Visibility is tested at the authored
points; it is not silhouette coverage.

Limits: 1–8 samples, at most 32 target IDs and 128 ignored IDs (nonzero, unique
and disjoint), a ray budget of at least 0, and positions within ±100,000 m. All
inputs are validated before the first query. Invalid inputs throw
`ArgumentOutOfRangeException` or `ArgumentException`. The native overload also
requires every listed ID to pass `IsAlive` before any ray is cast. The destination is
written only after every query succeeds. Provider exceptions, or a hit with a
zero/ignored entity or an out-of-segment distance, therefore leave the
destination unchanged.

Native notes: `ignore` excludes bodies, not descendants, so list every self
collider. Queries see committed geometry, not motion staged earlier in the same
Tick. Exactly equal hit distances resolve to the smallest entity ID. The budget
covers one call; callers own any aggregate per-Tick budget shared by several
NPCs. Use the generic `Evaluate<TQuery>` overload with an `ISightQueries` struct
to test math without the engine.

## Last-seen memory

```csharp
var memory = Load(state);                                 // rebuild from saved scalars
var step = SightMemory.Update(memory, intruder,
    sight.Visible ? intruderRoot : null, SightMemory.Tick(context.Tick), retainTicks: 90);
Store(ref state, step.State);                             // persist every scalar
```

A `SightMemoryState` slot is `Empty`, `Seeing`, `Remembering` or `Expired`. It
stores `Target`, `LastKnownPosition`, `FirstSeenTick`, `LastSeenTick`,
`RetainUntilTick` (= last sighting + retention), `LastUpdateTick` and
`Reacquisitions`. An unseen update after `RetainUntilTick` expires the memory.
`FirstExpiredTick` and `TicksSinceSeen` expose that timing. Expired memory retains its
data for inspection until the game assigns `SightMemoryState.Empty`. A sighting
from Remembering or Expired is reported as `Reacquired`. A `Seeing` slot that is not
seen produces `Lost`. With zero or sparse retention, `Lost|Expired` can occur together.

A non-Empty slot accepts only its own target and strictly increasing ticks.
Combine sensors into one update per tick, and forget the slot before tracking another
target. Ticks run from 0 to 2^53−1 and retention from 0 to 10^9 ticks (at 60 Hz,
90 ticks is 1.5 s). The game chooses the stored position, for example the target
root. An `Inconclusive` sight result is simply "not seen". Corrupt or inconsistent
states, mismatched targets, non-increasing ticks and out-of-domain values throw
before a result is produced. Retention deadlines may extend beyond `MaxTick`;
`FirstExpiredTick` then reports a mathematical deadline outside the supported
update domain. No updates or expiry transitions are available past `MaxTick`.

## Yaw steering

Advance once per committed 60 Hz tick for an upright actor whose local forward
is −Z. This example keeps a kinematic body at its committed position while
turning toward a game-selected target.

```csharp
var actor = context.Get(self).Transform;
var origin = actor.Position;
Vector3d direction = new(target.X - origin.X, 0, target.Z - origin.Z);
var profile = new YawTurnProfile(2.1, 8, 0.035);
var yaw = new YawSteeringState(state.AngularVelocity, (YawHalfTurn)state.HalfTurn);
var turn = YawSteering.Step(actor.Forward, direction, yaw, profile);
if (turn.AppliedRadians != 0)
    context.MoveKinematic(self, origin, turn.Rotation, ticks: 1);
state.AngularVelocity = turn.State.AngularVelocity;
state.HalfTurn = (int)turn.State.HalfTurn;
```

The profile accepts speed from 0.01 to 19 rad/s, acceleration from 0.01 to 600
rad/s² and dead zone from `1e-5` to π/4 radians, inclusive. The saved velocity
must be within the profile's speed limit. Every component of both vectors must
be finite and within ±2,000,000. Y is ignored for heading. Committed forward
must have XZ length greater than `1e-6`. A desired displacement with XZ length
at most `1e-6` has `HasDirection == false` and reports zero angular error.
Invalid inputs throw `ArgumentException` or `ArgumentOutOfRangeException`.

Positive yaw goes toward −X from −Z. The signed error follows the shortest
arc. Within `1e-12` of an exact half turn, `YawHalfTurn.Positive` (0, the
default) selects +π and `YawHalfTurn.Negative` (1) selects −π. The helper
limits speed with the distance available to stop. A missing direction, an
error inside or on the dead zone, or a reversed target while the actor is
turning requests braking to zero. Each tick changes speed by at most
`MaxAcceleration / 60` and applies `AngularVelocity / 60` at the updated speed.
`CommittedYaw` and `CommandedYaw` are in (−π, π]. A moving target or rounding
in the committed pose can cause a crossing while braking.

Store angular velocity as a `double` and the tie policy as an `int` in game
state. Velocity describes the steering command. Each new step measures yaw
from the committed transform, so physics rounding or a recorded pose needn't
be reconstructed from the prior command. An actor given a different rotation
owner should reinitialize steering velocity before resuming. Inspect the next
committed transform to measure the actual movement. The helper outputs a pure
Y rotation with single-precision quaternion components for `MoveKinematic`.
The speed and acceleration limits apply to the command. The native kinematic
integrator can round very small rotations to zero. Choose a dead zone for the
actor and verify its committed heading with the actual physics runtime.
Use one owner for the actor's kinematic motion and compose translation into
the same command when needed. Same-input repeatability has the platform and
runtime scope described below.

## Ownership, saves and determinism

The package records are transient values. Store each scalar explicitly in
`Game<TState>` and rebuild the records each Tick. The sight tests show memory
fields using `int`, `long`, `double` and `EntityId`. `SightMemory.Validate`
can check edited or restored memory fields. The package has no mutable static
state, cache, clock, randomness or steady-path allocation.

For the same inputs and provider replies, the package returns identical results
within one process. The results use floating-point square roots and
trigonometry. Exact cross-platform or cross-runtime replay needs separate
verification, including the native physics path.

## Use and verification

Reference `Poima.Ai.csproj` from a trusted game project, or include the package
source files you use. It requires the same .NET 10/C# 14 toolchain and Poima
gameplay SDK as the game. The native sight overload calls the baseline
`Raycast` and `IsAlive` services. The behavior tree evaluator uses the supplied
game callbacks.

`tests/ai_perception` contains synthetic managed checks with scripted tapes and
an independent analytic box model. They cover cone/range boundaries, frames,
rejection atomicity, ray accounting, blockers, memory and allocation. **They are
not native evidence.** The compiled [Perception Yard](../../examples/perception-yard/README.md)
separately qualifies native rays, rollback and complete same/fresh-process saves
on Linux headless and Windows CoreCLR in
[0.0.217](../../docs/evidence/m2-npc-perception.json). Each run checks 55 typed
fields, 176 requests and five complete archive-byte pairs. Hearing, navigation,
general tactical decisions, Native AOT for this sample and game-scale throughput
remain separate work.

The [heading fixture](../../tests/managed_heading_contract.py) compares the
command with an independent braking calculation and reads the native
kinematic pose after each selected tick. It checks both wrap directions,
braking and reversing, rejected-tick rollback and exact fresh-process
continuation. The [qualification record](../../docs/evidence/m2-yaw-steering-387.json)
identifies the tested runtime and compiler configurations.
