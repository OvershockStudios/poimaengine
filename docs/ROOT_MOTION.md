# Root motion

Root motion transfers an imported clip's planar movement to the native character controller. Animation supplies a requested displacement; Jolt resolves collision and vertical movement. The selected bone's visual XZ movement is removed, so moving the actor does not also move its skeleton a second time.

This is an opt-in `AnimationRig` binding. Ordinary in-place animation keeps its existing behavior. Two policies are available:

| Policy | Behavior |
| --- | --- |
| `character` | Drives a configured `CharacterController` through physics. Reports requested and actual movement separately. |
| `extract` | Reports the clip interval without moving a physical actor. Reports its displacement projected through the current rig wrapper. |

## Bind an imported rig

Add `root_motion` to the existing `AnimationRig` component. Preserve its existing asset and playback fields:

```json
"root_motion": {
  "node": 1,
  "mode": "character",
  "actor": "00000000000000000000000000000100",
  "enabled": true,
  "yaw": false
}
```

`node` is the original imported model index, not a joint-list position or a live entity ID. Select the bone that owns locomotion, including its animated ancestry. `character` requires an actor on the rig itself or an ancestor. `extract` is the default mode and accepts no actor. `enabled` defaults to true; `yaw` defaults to false.

The source, mode, actor and yaw policy are frozen when the runtime starts. Live enable controls do not change this binding. Hierarchical templates remap the actor reference to the spawned character; each instance gets its own clock and controls. A new or reborn member starts with its authored enable state and a neutral interval at the current tick.

The source ancestry must retain positive uniform scale. Its model parents must match the authored RigNode parents, and every model root must be directly below its rig wrapper. A root-motion rig cannot be below another RigNode. The mounting path to a Character actor must contain ordinary upright transforms with positive uniform scale, without another physics or animation owner. Mount translation, yaw and scale are included in movement conversion; a turning offset rig retains its pivot contribution.

Only one root-motion driver may be configured for a Character actor, even when disabled. Every layer mask must exclude the selected source and all its ancestors, even when the layer currently has zero weight. Upper-body or limb layers can still animate their own branches. IK runs after root compensation and layer composition.

## Control and observe

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "runtime.step",
  "params": {
    "request_id": "00000000000000000000000000000200",
    "session_id": "00000000000000000000000000000300",
    "expected_tick": 120,
    "ticks": 1,
    "root_motion": [{
      "entity": "00000000000000000000000000000400",
      "enabled": false
    }]
  }
}
```

Caller enable commands apply at the first tick of a batch. Supply the current `expected_structure_revision` to `runtime.step` after membership changes. Omitted `enabled` means true. Duplicate targets reject. Caller and compiled-game commands share a separate limit of 64 root-motion writes per tick; a caller/game write conflict on the same rig rejects. Base-animation and root-enable commands may target the same rig in one tick.

Query `runtime.root_motion` with `session_id`, current `tick` and the rig `entity`. The `state` is null for a live entity without root configuration; a missing entity rejects. Queries never consume motion or advance the clock.

| State field | Meaning |
| --- | --- |
| `from_tick`, `to_tick` | Last committed interval. Both equal the current tick for a newly configured member. |
| `local_delta`, `yaw_delta` | Rig-space planar displacement and shortest signed heading change in radians. Local Y is zero. |
| `world_requested` | Interval projected into world space; includes mounting pivot conversion for Character policy. |
| `world_applied` | Character position after physics minus its position before physics, including actual vertical movement. |
| `applied_yaw` | Heading change applied to the Character owner. |
| `translation_error` | XZ distance between requested and actual displacement. |

Extract has neutral applied movement, applied yaw and error. A disabled root interval has neutral movement diagnostics, while ordinary character input remains available. Pose compensation still runs when disabled, allowing an in-place version of the same clip. Error is a displacement diagnostic, not proof of which obstacle or contact caused it.

## Use from C#

Opt into `IRootMotionGame` independently of animation layers, hit queries or IK. It requires `animation_root_motion_v1` and the append-only 304-byte services prefix in epoch 7. Existing required prefixes retain their layouts.

Inside `Tick`, use:

```csharp
RootMotionState? previous = context.GetRootMotion(rig);
context.SetRootMotion(rig, enabled: true);
context.SetAnimation(rig, clip: 1, time: 0, speed: 1,
    loop: true, playing: true);
```

The getter returns the committed interval. A staged setter remains invisible to reads during the same callback. There are at most 1,024 admitted root-motion reads and 64 writes per tick. Enable writes may target configured pending instances; they apply after membership publishes. They reject if the instance is removed or the rig is unconfigured. The extension does not grant other optional services automatically.

Enabled Character root motion owns horizontal movement, so nonzero move input rejects instead of fighting the animation. With `yaw: true`, nonzero look-yaw input also rejects. Pitch input, jump and named actions remain available; `yaw: false` retains ordinary controller yaw and the clip's visual rotation. Disabling root motion permits normal movement input.

## Loops and transitions

Movement is evaluated over one fixed-tick interval from the base clip clock. Clip cycles are unwrapped using planar rigid transforms, including translation and turning. Issue playback replacements on state changes, rather than every Tick. Seeking or replacing playback reanchors the interval rather than teleporting from the old pose. Stopped, zero-speed and completed non-looping clips produce no locomotion.

Crossfades blend interval velocities using the midpoint fade weight. Interrupting a fade retains the last committed blended velocity; inertial transitions use a separate decaying velocity correction. These histories are distinct from visual pose and IK histories. Sampling, skinning, inspection and restoration can run repeatedly without applying movement.

Planar source intervals are bounded to 100 m/s and 20 rad/s. Yaw extraction supports upright rest ancestry and pure Y-axis LINEAR rotation channels, with unambiguous shortest key segments and a summed ancestry rate bound evaluated at maximum playback speed 8. Unsupported pitch/roll, STEP/cubic yaw profiles or faster trajectories reject when yaw extraction is enabled. Translation-only extraction retains visual rotation. This tier reports shortest net heading change, not arbitrary angular winding.

## Save and failure behavior

Root-configured worlds use nested animation-state version 5. The frozen binding, enable control, fade, velocity history and committed diagnostics persist through checkpoints and saves. IK-only worlds retain version 4; worlds without either feature preserve their earlier animation formats.

Restore reconstructs physical poses before deriving animation and IK, without advancing an interval. Failed callbacks, input conflicts, invalid evaluated poses, physics failures and later-tick errors roll back the whole batch, including movement, controller angles, root state, gameplay, membership and save intents. Blocked requested movement is not stored as future motion debt.

Durable saves restore typed body and controller state, but do not serialize Jolt contact-solver caches. Immediate checkpoint restoration is exact. Subsequent grounded ticks can differ numerically from an uninterrupted warm-contact simulation; this is separate from complete in-process rollback.

Vertical animation-driven climbing, stride warping, automatic foot placement, contact-aware turning, animation graphs and general full-body locomotion remain separate work. The current root-motion route is planar Character locomotion and Extract diagnostics.

## Qualification

[The 0.0.119 results](evidence/m2-root-motion.json) join native, API, compiled
CoreCLR/Native AOT and independent GPU checks. They include real capsule
collision, jumping and turning, complete failure rollback, compatible CoreCLR
reload and exact immediate save restoration. The three-tick grounded
continuation check compares two independent reconstructions of the same
serialized checkpoint. The earlier uninterrupted warm-contact comparison
remains recorded as a failure, with a roughly 3.3e-8-metre vertical difference.

Four GPU comparisons cover both recorded laptop adapters and forward/deferred
lighting. Full BMP pixels and nine HDR/depth/normal probes per comparison match
independently authored weighted reference positions exactly. This synthetic
fixture does not establish animation throughput, a production locomotion
system, physical controller input or an unpaused player-service route.
