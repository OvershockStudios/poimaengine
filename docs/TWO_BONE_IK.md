# Native two-bone IK

Two-bone IK positions a configured hand or foot after native base playback,
crossfades, inertial transitions and masked layers. Point-only controls change
the root and middle rotations, preserving translations, scales and the end
node's local rotation. An optional independent end-orientation goal then
rotates the final joint for an explicit sole or grip frame. Targets and frames
come from the game, or from an explicit [automatic foot-contact profile](GROUND_CONTACTS.md).
Anatomical chains and sole calibration remain explicit.

## Author a frozen chain

Inspect the cooked model's original node indices and its instantiated RigNode
hierarchy. Add optional `ik` to the complete existing `AnimationRig` component:

```json
"ik": [{
  "slot": 1, "root": 2, "middle": 1, "end": 3,
  "target": [1, 1, 0], "pole": [0, 0, 1],
  "enabled": true, "space": "world", "weight": 1
}]
```

This is a component fragment, not standalone transaction parameters. Preserve
the rig's asset, clip, time, speed, loop, playing and any layer configuration
when sending a guarded `world.transact` component replacement. The asset hash
pins the exact model. Indices are not names or automatically inferred bone
roles.

An array contains at most eight unique slots numbered 1–8. `slot`, `root`,
`middle`, `end`, `target` and `pole` are required. `enabled` defaults to true,
`space` to `world`, and `weight` to 1. Booleans are strict; weight is finite
within 0–1. Target and pole contain exactly three finite coordinates within
±1,000,000 units of the selected space. Unknown fields reject.

The three distinct model indices must form the exact direct-parent chain
root → middle → end in both the imported model and authored RigNode graph.
Configured solved branches must not overlap. All model-node bindings remain
required, including importer-created ancestors. Deletion or reparenting that
breaks a configured chain rejects the whole authored transaction. IK wrappers
cannot sit beneath a RigNode; dependencies between nested animated wrappers
are unsupported.

Configured chains require nondegenerate rest segments. The default `uniform`
metric requires positive uniform rest scales along the model ancestors, root
and middle, including disabled chains. An active weighted chain must also
retain positive uniform evaluated scales along those nodes. The explicit
[`near-uniform-v1` profile](IK_METRIC_PROFILES.md) admits bounded deviations
while preserving source transforms and measuring the actual affine solve.
Unsupported evaluated poses reject the complete simulation batch. The wrapper
itself may have an invertible positive affine transform.
Hierarchical templates retain the model indices and remap ordinary RigNode
ownership to each spawned instance. Later authored edits leave the running
definition frozen; restart to adopt a changed chain.

### Follow a moving item

Set `target_node` on an authored slot to use an animated attachment from that
same model. For example, an off-hand wrist can follow the grip on an item
carried by the animated primary hand. Use the item's original model node index.

```json
"ik": [{
  "slot": 1, "root": 9, "middle": 8, "end": 7,
  "target_node": 11, "space": "rig",
  "target": [0.04, 0.09, 0.01], "pole": [-0.24, 0.08, 0.16],
  "end_orientation": {"rotation": [0, 0, 0, 1], "weight": 1}
}]
```

The indices and offsets are illustrative. Inspect the original model first.
Calibrate the wrist point, elbow pole and desired wrist rotation against a good
equipped pose. The complete live control must explicitly specify `space:"rig"`
(`IkSpace.Rig` in compiled gameplay) for a bound slot. The slot interprets the
target and pole as attachment-local points and resolves them through the
sampled node's full affine transform. The end goal is relative to the node's
composed logical quaternion. Position and orientation are resolved after the
same base pose and layers have been sampled, so neither requires a prior-tick
item transform or a per-tick gameplay write.

The anchor and its ancestors must preserve their imported parenting. Choose
an anchor outside every configured solved branch. A shared ancestor or a
separate animated branch can drive several limbs. This rule prevents feedback
and makes independent slot order irrelevant. Grounding and an attachment
cannot own the same slot. Changing the target node requires a fresh runtime.
Live changes to the offsets, weights and enabled flag use the ordinary IK
commands, including for a release and regrip. Reach clamping and metric
requirements still apply. Verify the authored pose and clipped reach with
[`runtime.pose.compare`](POSE_FRAME_COMPARISON.md) and native captures.

## Replace live controls

Supply optional `ik` commands with ordinary guarded `runtime.step` parameters:

```json
{
  "session_id": "00000000000000000000000000000384",
  "request_id": "00000000000000000000000000000385",
  "expected_tick": 60,
  "ticks": 1,
  "ik": [{
    "entity": "000000000000000000000000000000c8",
    "slot": 1, "target": [1, 1, 0], "pole": [0, 0, 1],
    "space": "world", "enabled": true, "weight": 1
  }]
}
```

Replace the IDs, tick and points with current inspected values. Each command
completely replaces the configured slot's controls on the first Tick and
persists until another command. Omitted optional fields use the fixed API
defaults true/world/1, not the previous live controls. `world` points stay
fixed as the actor moves; `rig` points move with the wrapper or the configured
attachment. Pole is a point that selects the bend plane, in the same space as
target.

At most 64 combined caller/gameplay IK commands are admitted per Tick.
Duplicate `(entity, slot)` writes and caller/gameplay conflicts reject;
playback and IK may target the same rig. The existing animation-command
budget remains separate. Disabled chains preserve incoming local poses exactly.
A zero positional weight also preserves them when no orientation goal is active.
No implicit interpolation or target smoothing occurs.

Unreachable goals clamp to the chain's reach in rig coordinates without
stretching. Under an affine wrapper this is not a nearest-point guarantee in
world distance. A collinear pole uses the current bend plane and then a
deterministic Cartesian fallback. Clock histories retain pre-IK poses, so
corrections do not feed into inertial transitions.

## Align the end joint

Add `end_orientation` to an authored chain or a complete live IK command:

```json
"end_orientation": {"rotation": [0, 0, 0, 1], "weight": 1}
```

The quaternion is XYZW in the rig's logical model frame, regardless of whether
the target and pole use `world` or `rig` space. If the slot has a `target_node`,
use the attachment's logical frame. Choose it from the imported
joint's actual axes. It describes the desired final end frame, rather than a
delta from the rest pose. Its weight is independent of the positional weight;
position weight zero can therefore rotate a hand without moving the chain.
The solver uses the actual parent orientation after positional blending,
converts the goal into that parent's local frame and blends only the end
rotation. Root/middle results, endpoint position, translations and scales remain
unchanged by this extra stage. Descendant attachments and skinning follow the
new end rotation.

Both fields are required. Rotation needs four finite numbers with a finite,
nonzero norm; weight is finite in 0–1. Null and unknown fields reject. Supplied
goals are normalized unless their norm is already within eight double-precision
epsilons of one; that guard makes repeated save/restore canonicalization
idempotent. The first nonzero component in W, X, Y, Z order is made positive,
and signed zero components become positive zero. Opposite quaternion signs
therefore express the same stored goal, including a 180-degree rotation.

Omission or weight zero clears the goal in the replacement controls. A supplied
zero-weight goal must still pass validation before becoming absent. This uses
the original point-only solve and runtime serialization path. An authored
document explicitly containing a zero-weight field remains a different authored
document; it is not silently rewritten to remove that field. `enabled:false`
disables both applications while retaining a nonzero orientation intent.

Active orientation application has the same selected metric restrictions as
positional IK, including when only the orientation weight is nonzero. The
default profile requires positive uniform evaluated ancestor/root/middle
scales. The wrapper may still be positive affine;
the goal is a logical rig quaternion, not a decomposed world rotation under a
nonuniform or sheared wrapper. [Ground queries](GROUND_CONTACTS.md) and optional
[planted-foot locking](FOOT_PLANTING.md) use separate profiles. Pelvis compensation
and joint limits remain further work.

## Probe a reach for obstacles

`runtime.ik.clearance` tests a proposed manual two-bone goal over the
committed animation and physics. It reads the current three world-space
joint points, runs the same solver on a copy of the current clip and
layer sample, and returns the three candidate joint points. It sweeps
world-space capsules along the upper and lower segments, with independently
authored radii. An eye ray can miss a blocker that the sweep finds.

```json
{
  "session_id": "00000000000000000000000000000384",
  "tick": 60,
  "structure_revision": 2,
  "goal": {
    "entity": "000000000000000000000000000000c8",
    "slot": 1,
    "target": [1, 1, 0],
    "pole": [0, 0, 1],
    "weight": 0.65
  },
  "radii": [0.04, 0.03],
  "ignore": ["00000000000000000000000000000001"]
}
```

Use the current session, tick and structure revision. `goal` accepts the
[complete IK command](#replace-live-controls). The point path is unaffected by
rotation at the end joint. Radii are 0.001–1 m, in upper-to-lower order, and
should enclose the art at the scale used by the character. Ignore entries are
explicit, unique live identities. Typically exclude the actor's collision
owner and the prop's release proxy if the hand is intended to touch it.
Covers animated manual chains. Automatic grounding and physical rig owners
reject the query.

`current` and `candidate` list root, middle and end points. `blocker` is
null when the corridor is clear. Otherwise it reports the segment (1 upper,
2 lower), the interval (1–4) and the native capsule hit. Intervals are
checked in time order, with the upper segment first within each interval.
Contact fraction and overlap depth belong to that capsule cast. An overlap
in a later interval is relative to that interval's start. The reported
segment and interval give a stable first blocked interval, with upper
segment priority when both block in the same interval.

The probe connects committed and candidate joint points linearly. Each of
four intervals retains its starting capsule axis, sweeps its midpoint, and
expands its radius by the differential endpoint travel. This encloses the
interval's interpolated segment and can report an obstruction before the
actual limb mesh touches it. The probe leaves animation, physics, ticks and
saves unchanged. Evaluate the result again when the scene changes. Later
physics, staged movement or animation changes can move the next pose. Use
the separate attachment contact guards to authorize a grip.

Compiled gameplay declares `IIkClearanceGame` and calls
`context.ProbeIkClearance(goal, upperRadius, lowerRadius, ignore)` during
`Tick`. The gameplay result contains the same points and the blocker
identity, segment, interval, fraction and penetration. Each call reserves
eight of the 32 native capsule queries allowed per Tick. Exhaustion rejects
the call. The named feature is `animation_ik_clearance_v1`, with
`animation_ik_v1` and `capsule_cast_v1`. It uses a 288-byte version-2
payload on the existing point IK callback and a 400-byte minimum epoch-7
table. The legacy point and orientation payloads retain their layouts.

[Carry Props](../examples/carry-props/README.md#check-the-arm-corridor) uses
the probe before staging each reach and return in its opt-in module.
[Verification](evidence/m2-ik-clearance.json)

## Observe and save

`runtime.ik` requires `session_id`, exact `tick`, `entity` and slot 1–8.
It returns `{session_id, tick, source:"runtime", state}`. Existing nonrig
entities or unconfigured slots return null; missing entities reject. A state
contains entity, slot, root/middle/end indices, enabled, space, target, pole,
weight, world `end_position`, world-distance `target_error`, `clamped` and
`pole_fallback`. These are cached committed observations: inspection does not
sample animation or advance time. Skinning and attached HitRegions consume
the corrected committed pose.

Set `include_end_orientation:true` on `runtime.ik` to add `state.end_orientation`.
It is null without a retained goal; otherwise it contains the canonical goal's
`rotation` and `weight`, the cached logical rig `actual_rotation`, and the
shortest angular error in radians. A disabled chain can retain a goal and
report its unapplied error. Missing state remains null. Omitted or false keeps
the existing response shape. Inspection never solves the chain.

Set `include_target_node:true` to add the frozen index or null on an
unbound slot. For an anchored slot, `target`, `pole` and the requested
`end_orientation.rotation` remain attachment-local values. The world
`target_error` and `end_orientation.angular_error` already account for the
sampled attachment, including when the reach clamps or the slot is disabled.
The selected model index is retained in the save and verified against the
frozen rig on restore. Unbound slots keep their original save fields.

To reproduce the attachment check with the
[pinned original Knight](CHARACTER_LIBRARY.md#original-source):

```sh
python3 tests/runtime_ik_attachment.py --binary build/linux-game/poima \
  --source /path/to/Knight.glb --output build/ik-attachment-check
```

On Windows pass `--gpu` with the Vulkan adapter index to capture two target
poses with the slot disabled and enabled. From WSL, also pass
`--windows-interop` to open the Windows executable and translate asset paths.

Commands participate in complete physics/gameplay/animation/component/save
rollback. Removing an instance removes its IK state; newly spawned rigs use
frozen defaults, while surviving rigs retain controls. IK-configured worlds
use nested animation-state version 4, or version 5 with root motion. A retained
nonzero end-orientation goal selects version 6, including while disabled.
Version 6 records every chain's nullable orientation and each rig's nullable
root motion alongside all existing controls and history. Removing the last goal
returns to the appropriate legacy format. Legacy versions 1–5 restore absent
orientation controls rather than inheriting authored orientation defaults.
Fresh restore reconstructs diagnostics at the saved physical boundary.
Existing save-upgrade plans do not authorize adding, replacing or remapping
frozen IK chains.

## Compiled gameplay

Declare independent `ITwoBoneIkGame` for `GameContext.GetTwoBoneIk` and
`SetTwoBoneIk`. It negotiates `animation_ik_v1` through a 288-byte epoch-7
services prefix; getters and setters are at offsets 272 and 280. Earlier ABI
prefixes remain unchanged, and this marker grants no other extension.

```csharp
using Poima;

public struct HandState { public EntityId Rig; }

[GameModule("example.hand-ik")]
public sealed class HandGame : Game<HandState>, ITwoBoneIkGame
{
    public override void Initialize(ref HandState state)
    {
        state = default;
        state.Rig = new(0, 200); // Replace with an existing configured rig.
    }

    public override void Tick(ref HandState state, GameContext context)
    {
        if (context.GetTwoBoneIk(state.Rig, 1) is null) return;
        context.SetTwoBoneIk(state.Rig, 1, new(1, 1, 0), new(0, 0, 1),
                             space: IkSpace.Rig);
    }
}
```

Queries are Tick-only, with at most 1,024 admitted reads per Tick. Writes are
staged and remain invisible to getters until publication. Pending instance
births may receive configured-slot writes but are not visible to getters;
removal/write conflicts reject. The immutable `IkState` exposes the committed
controls and diagnostics described above. Trusted C# external side effects
remain outside native rollback.

For independent hand or foot rotation, declare `ITwoBoneIkOrientationGame`.
It inherits `ITwoBoneIkGame` and additionally negotiates
`animation_ik_end_orientation_v1` with a 352-byte epoch-7 services table.
The new methods share the existing 1,024-read and 64-write Tick budgets and
same-slot conflict checks with the point-only API:

```csharp
var observed = context.GetTwoBoneIkOrientation(state.Rig, 1);
if (observed is not null)
{
    context.SetTwoBoneIkOrientation(new IkOrientedCommand(
        state.Rig, 1, new(1, 1, 0), new(0, 0, 1),
        true, IkSpace.Rig, 1,
        new IkEndOrientationGoal(new Quaterniond(0, 0, 0, 1), 1)));
}
```

This is a Tick-body fragment for an orientation-marked game. Replace the
identity quaternion with the desired imported joint frame; it does not infer
a sole axis. `IkOrientedState.Point` contains the existing `IkState`, and its
nullable `EndOrientation` contains `Goal`, `ActualRotation` and `AngularError`.
`IkOrientedCommand` requires all eight constructor arguments explicitly.
Both setters replace complete controls: the old point-only setter, a null
orientation, or a valid zero-weight orientation clears a previous goal.
The old setter's overloads and record layouts remain unchanged.

## Reproduce the checks

Use a matching engine build and SDK/bridge. The commands below are recipes,
not recorded qualification results. Every native Windows execution, including
Python fixtures that launch the engine or .NET tools, must run beneath an
external Job owner with a finite deadline, bounded output and natural process
tree cleanup. The public fixtures do not establish that outer ownership.

Build the native fixture through the normal configured build, then run it
under that owner:

```sh
cmake --build BUILD --target poima-runtime-two-bone-ik-test
BUILD/poima-runtime-two-bone-ik-test.exe
python tests/runtime_two_bone_ik.py --binary ENGINE --output NEW_PROTOCOL_OUTPUT
```

The native fixture has a portable math/animation branch and, with native
simulation enabled, physical-wrapper, skin, HitRegion and instance checks.
The JSON-RPC fixture authors an original imported chain and exercises frozen
controls, guarded queries, rollback, templates and fresh durable continuation.

For compiled CoreCLR gameplay, build the public game and a matching bridge
into separate fresh destinations, then supply the selected hostfxr DLL:

```sh
dotnet build tests/managed_two_bone_ik_gameplay/Poima.ManagedTwoBoneIkGame.csproj -c Release -o NEW_GAME
dotnet build managed/Poima.ManagedBridge/Poima.ManagedBridge.csproj -c Release -o NEW_BRIDGE
python tests/two_bone_ik_gameplay_contract.py ENGINE --hostfxr HOSTFXR_DLL --bridge NEW_BRIDGE/Poima.ManagedBridge.dll --assembly NEW_GAME/Poima.ManagedTwoBoneIkGame.dll --output NEW_CORECLR_OUTPUT
```

Alternatively publish that game with [the native gameplay publisher](NATIVE_GAMEPLAY.md)
and pass its descriptor to the same public consumer fixture:

```sh
python scripts/publish_native_gameplay.py --project tests/managed_two_bone_ik_gameplay/Poima.ManagedTwoBoneIkGame.csproj --type Poima.Tests.ManagedTwoBoneIkGame --rid win-x64 --engine-version 0.0.118 --output NEW_NATIVE_GAME --work NEW_PUBLISH_WORK
python tests/two_bone_ik_gameplay_contract.py ENGINE --native-descriptor NEW_NATIVE_GAME/native-gameplay.json --output NEW_NATIVE_OUTPUT
```

The consumer checks actual committed bone/HitRegion observations, staged
writes, quotas, callback and later-tick rollback, and saved continuation.
Compatible hot reload is checked only in the CoreCLR branch.

The end-orientation fixture uses a separate public compiled game. Build
`tests/managed_two_bone_ik_orientation_gameplay/Poima.ManagedTwoBoneIkOrientationGame.csproj`
and a matching bridge, then run beneath the same external process-tree owner:

```sh
python tests/runtime_two_bone_ik_orientation.py --binary ENGINE --output NEW_JSON_OUTPUT
python tests/runtime_two_bone_ik_orientation.py --binary ENGINE --hostfxr HOSTFXR_DLL --bridge NEW_BRIDGE/Poima.ManagedBridge.dll --assembly NEW_GAME/Poima.ManagedTwoBoneIkOrientationGame.dll --output NEW_CORECLR_OUTPUT
```

For Native AOT, publish that project with type
`Poima.Tests.ManagedTwoBoneIkOrientationGame` and the intended RID, then supply
`--native-descriptor NEW_NATIVE_GAME/native-gameplay.json` instead of the three
CoreCLR arguments. This fixture checks geometry, complete checkpoint pairs,
shared quotas, conflicts and fresh-process continuation. It does not exercise
compatible hot reload; the independent `managed_service_abi` executable checks
the extension's layout, malformed replies and short-host rejection separately.

For the separate GPU comparison, run this recipe once per actual adapter and
lighting path, with a fresh output for each run:

```sh
python tests/runtime_two_bone_ik_capture.py --binary ENGINE --gpu 0 --lighting-path forward --output NEW_GPU_OUTPUT
```

Repeat with `deferred` and, where a second hardware adapter exists, its selected
index. Each run compares the original weighted ribbon against independently
specified static deformed positions using complete BMP pixels and native
HDR/depth/normal probes. The fixture records the observed device; two-adapter
coverage requires two distinct actual device identities. This synthetic
comparison does not qualify an unpaused player window or imported character art.

[Recorded 0.0.196 end-orientation qualification](evidence/m2-two-bone-ik-orientation.json)
covers selected Linux native checks, the opt-in query, actual CoreCLR and
Native AOT consumers, and complete warm/cold checkpoint continuation. A separate
unchanged compiled-game workflow checks compatibility through cooked reopening.
These are CPU checks; they do not extend the earlier GPU qualification to the
new orientation feature or establish automatic ground contacts and foot locking.

[Recorded 0.0.118 qualification](evidence/m2-two-bone-ik.json) covers native
and JSON-RPC checks, actual CoreCLR and Native AOT consumers, exact durable
continuation and eight forward/deferred GPU captures on two adapters. Complete
pixels and HDR/depth/normal probes match the independent static reference
exactly. This contract does not establish automatic foot grounding, full-body
IK, joint limits, root-motion controller integration, game-scale performance
or Alpha completion.
