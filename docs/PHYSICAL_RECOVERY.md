<!-- SPDX-License-Identifier: Apache-2.0 -->
# Guarded physical return

A character that has briefly entered physical simulation can return to its
character controller and animation without resetting its pose or moving its
actor to a guessed standing location. This optional policy admits a return only
when the current body is sufficiently upright, slow, supported and clear of
obstacles. It does not lift a fallen character, balance a ragdoll or choose a
get-up animation.

The contract below describes the implemented path. Selected native checks pass
on Windows and Linux; the compiled recovery consumer passes on Linux CoreCLR.
Windows compiled-consumer checks and publication of the checkpoint's complete
qualification record remain pending. Imported-character recovery and rendered
motion quality are not qualified.

## Configure the rig

Add `recovery` to an existing [PhysicalRig](PHYSICAL_RIGS.md) with
`initial_mode: "animated"` and a valid
[controller handoff](PHYSICAL_RIGS.md#controller-handoff). The linked controller
must use the supported standing capsule: radius 0.05–2 metres and total height
at most 4 metres. Drives and an animation-target player are optional.

This fragment illustrates the required fields. Replace the part names and sole
coordinates with calibration for the actual physical proxies:

```json
"recovery": {
  "supports": [
    {"part": "left_foot", "point": [0, -0.1, 0]},
    {"part": "right_foot", "point": [0, -0.1, 0]}
  ],
  "uprights": [
    {"part": "pelvis", "up_axis": [0, 1, 0]}
  ],
  "max_tilt_degrees": 20,
  "max_linear_speed": 0.6,
  "max_angular_speed": 2,
  "stability_ticks": 3,
  "blend_ticks": 12
}
```

All seven fields are required; unknown fields, nonfinite numbers and incorrect
types reject. The profile is immutable during the runtime session.

| Field | Meaning and bounds |
| --- | --- |
| `supports` | Exactly two distinct existing part names. Each `point` is a sole point in that part's **COM-local physical frame**, in metres; every component is within ±10 m. |
| `uprights` | One to four distinct existing parts. Each `up_axis` is a unit vector in that part's COM-local frame. Its transformed direction is compared with world +Y. |
| `max_tilt_degrees` | Greater than zero, at most 30°. Every specified upright must pass. |
| `max_linear_speed` | 0–2 m/s. Bounds the velocity norm of **every** physical part. |
| `max_angular_speed` | 0–4 rad/s. Bounds the angular velocity norm of every physical part. |
| `stability_ticks` | Integer 2–60. Required consecutive eligible completed ticks on the same two support body IDs. |
| `blend_ticks` | Integer 2–120. Duration of the return transition after its exact initial pose. |

COM-local coordinates are independent of imported model units and render scale.
They are not bone-local mesh coordinates. See `body_in_bone` in the
[physical profile contract](PHYSICAL_RIGS.md#author-a-profile).

## Eligibility and clearance

The runtime retains the current and previous completed local poses of the actor,
rig wrapper and every source-model node, including unmapped nodes. A return needs
this consecutive history and a standing controller; crouching, an existing
crouch request or a blocked standing request prevents it.

In addition to the authored posture and speed limits, fixed safeguards require:

- The rig wrapper to remain within 0.1 m and 30° of its authored local transform.
  The rotation bound includes yaw; return does not correct an arbitrary heading.
- Each sole to be within 4 m of the motor origin. Its downward probe runs from
  0.03 m above the sole to 0.05 m below it, and must find a static surface with an
  upward normal dot product of at least 0.95. Moving supports reject.
- Clearance for both the controller's collision proxy and its padded standing
  motor shape at the unchanged actor pose. Only this rig's outgoing bodies and
  its suppressed controller proxy are ignored. A floor and a wall belonging to
  the same mesh do not make the wall exempt.
- An incoming actor velocity, derived from its two retained frames, of at most
  2 m/s.

The motor uses 0.02 m padding; its narrow floor-contact allowance is at most
0.001 m penetration and checks the individual contact location and normal.
Each assessment permits at most 256 body-filter visits and 1,024 reported hits
across its support and clearance queries. It refuses on exhausted budgets or
invalid native results. These counts do not bound elapsed query time or every
internal mesh traversal operation.
Sole probes establish nearby support geometry, not a measurement of load-bearing
contact force.

Inspection reads the last committed assessment without issuing new queries or
advancing simulation. Structural changes at the same tick make that assessment
stale, reported as `history_unavailable`; `assessment_revision` identifies its
original structural revision. A return request checks current support and full
clearance again after same-tick structural edits. An earlier `ready` observation
therefore does not guarantee that a later request succeeds.

## Inspect and request through the service

Read the rig using the actual session, current tick and entity:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "runtime.physical_recovery",
  "params": {
    "session_id": "00000000000000000000000000000300",
    "tick": 120,
    "entity": "00000000000000000000000000000400"
  }
}
```

The response's `state` is null for an existing entity without a recovery profile;
a missing entity rejects. A present state reports `phase` (`physical`,
`returning` or `locomotion`), `status`, stability counts, generation, assessment
revision, last attempt and return ticks, posture/speed measurements and nullable
`support_body_ids`. These are native body IDs, not entity IDs. `wrapper_angle`
is in radians; `wrapper_distance` is in metres.

Typical physical statuses are `stabilizing`, `ready`, `not_standing`,
`not_upright`, `moving`, `wrapper_displaced`, `missing_support`,
`moving_support`, `invalid_support`, `proxy_blocked`, `motor_blocked`,
`sole_out_of_range`, `query_budget` and `invalid_native_result`.
`history_unavailable` also covers insufficient consecutive pose history.

Submit the request through the existing guarded `runtime.step` transaction:

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "runtime.step",
  "params": {
    "session_id": "00000000000000000000000000000300",
    "request_id": "00000000000000000000000000000200",
    "expected_tick": 120,
    "ticks": 1,
    "physical_rigs": [
      {"entity": "00000000000000000000000000000400", "recover": true}
    ]
  }
}
```

Replace the illustrative IDs and tick; include the current
`expected_structure_revision` when runtime membership has changed. The existing
[command and retry rules](PHYSICAL_RIGS.md#cliservice-commands) apply.

Acceptance stages an attempt. Inspect the next committed state for its result:
`last_attempt_result: "returned"` and an incremented `generation` identify a
successful transfer. An unsafe request keeps physical ownership and records its
refusal. Malformed commands, absent permission or conflicting writes reject the
batch. Recovery must be the sole physical action on that rig in the tick; it
cannot share the tick with its animation, rate, IK, grounding or root-motion
writes. Caller and compiled-game writes share these rules and the 64-command
physical limit.

## Pose and controller ownership

For an accepted return from completed tick T, the first returned completed pose
at T+1 retains the current complete node and wrapper locals. The actor transform
is unchanged, and movement, look, stance changes and jump integration are held
for that transfer tick. Subsequent locomotion uses the restored controller.
The engine removes the outgoing articulation from collision before re-enrolling
the original controller proxy, refreshes support and transfers the bounded actor
velocity. The controller and physical limbs do not both own collision.

The transition uses the captured current and previous poses to seed inertial
corrections toward the continuing composed animation. It also brings the wrapper
back to its authored local transform. Playback is not restarted. This overlay
applies before procedural correction; manual IK and grounding correction ramp
from zero at the first boundary. Grounding starts fresh without reusing the
pre-takeover foot anchors or contact decisions. Requested controls remain
distinct from those effective ramped corrections.

During the return blend, conflicting animation/procedural edits reject. A new
physical activation can interrupt it, taking the actual current solved pose and
wrapper into physics. A later successful return starts a new generation.
Successful return clears drive strengths and selects manual target delivery;
it does not leave an independent animation-target player controlling the body.

The movement hold ends after the transfer tick publishes its final pose and
history. A structural edit made at that same completed tick refreshes controller
contacts normally, including when a supporting surface is removed.

## Diagnose a refused return

Inspect `runtime.physical_rig` with `include_parts: true` alongside the recovery
assessment. The thresholds apply to every physical part: a quiet torso does not
make a rapidly moving hand eligible. Use the authored physical frames when
checking sole points and upright axes.

| Observation | What to check |
| --- | --- |
| `moving` | Per-part linear/angular velocity, initial collider overlap and the active drive targets. Recovery does not stop the body for you. |
| `not_upright` | The configured COM-local upright axes and the actual body posture. A fallen rig needs a separate get-up or balance policy. |
| `missing_support` / `invalid_support` | Sole calibration, probe reach and the nearest surface's normal. A nearby visible floor alone is insufficient. |
| `proxy_blocked` / `motor_blocked` | The standing capsule at the actual actor position, including floor penetration. Clearance uses collision geometry rather than the visible mesh. |
| `history_unavailable` | Consecutive completed ticks and `assessment_revision`. A scene mutation invalidates the earlier assessment. |
| `stabilizing` | Wait for the authored consecutive eligibility window; losing support or eligibility resets it. |

Keep the refusal result available to gameplay. Repeatedly requesting a return
does not override the geometry or stability checks.

## C# gameplay and native ABI

Declare `IPhysicalRecoveryGame` on the game class. During `Game.Tick`, using an
already resolved rig entity, the minimal request pattern is:

```csharp
var recovery = context.GetPhysicalRecovery(rig);
if (recovery is { Physical: true, Status: PhysicalRecoveryStatus.Ready })
    context.RequestPhysicalRecovery(rig);
```

Reads expose committed state, including `LastAttemptResult` and `Generation`;
they do not see requests staged earlier in that callback. The read limit is
1,024 attempts per tick. These APIs are not available in a UI control callback.
The marker requires `physical_recovery_v1`, `physical_rig_v1`, services epoch 7
and the 512-byte table. It does not grant drive or animation-target APIs; those
retain their separate feature markers.

The [native ABI](../include/poima/gameplay_abi.h) appends
`physical_recovery_get` at offset 496 and `physical_recovery_request` at offset
504. Their version-1 state and command records are 144 and 32 bytes respectively.
Headers, reserved fields, chronology, identities and result bounds are validated.
The previous 496-byte prefix and earlier negotiated views remain unchanged.

## Persistence and limits

Content that opts into recovery, including dormant templates, selects physical
snapshot extension 8. Its complete state retains pose history, assessment and
structural revision, attempt/transfer lineage, exclusive collision ownership,
the complete source-player history even before the first return, and the
version-1 animation-return sidecar with requested procedural controls.
Cold restore validates these joins before
publishing the replacement runtime. Whole-batch rollback includes the return
transition and late failures. This does not promise cross-platform future
bit-identical simulation.

Omitting recovery preserves the earlier profile encoding and physical snapshot
selection. Previously one-way controller handoff does not acquire return
permission implicitly. Existing [save configuration and UI/gameplay guards](GAMEPLAY_SAVES.md)
still apply.

This policy is limited to near-upright characters on static support. Prone
get-up, balance, moving-platform return, arbitrary root relocation and
contact-aware recovery animation selection remain outside it. Its support
checks do not prove that every visible limb will avoid intersection throughout
the animation blend.

## Qualification

The selected 26-test native regression passes on Windows and Linux. Focused
checks cover the profile, support/clearance queries, strict saved-state codec,
full runtime return and animation-return histories. They include immediate
support removal after return across live, cold-restored and rolled-back owners.
After building those targets in a test-enabled Jolt configuration, the focused
CTest selection is:

```sh
ctest --test-dir <build> --output-on-failure -R '^(physical_recovery_profile|physical_recovery_query|physical_recovery_state|runtime_physical_recovery|runtime_animation_return_native)$'
```

The Linux CoreCLR fixture passes 566 RPC calls, 34 complete saves and 19 whole
byte/state comparisons across two naturally closed engine processes. It checks
actual recovery, subsequent movement, permission denial, fresh obstruction,
rollback and cold continuation. Its 28 gameplay fields occupy 176 bytes.

The small analytic runtime fixture and supplied-pose animation fixture have
different scopes. Their results do not establish imported-character motion
quality, rendered continuity, physical device input or production-scale
performance. A separate imported Knight experiment did not reach a safe return
window; character calibration and controller placement remain further work.
Windows compiled recovery and retained-consumer compatibility checks are the
next qualification work. No new recovery NativeAOT publication is claimed.
