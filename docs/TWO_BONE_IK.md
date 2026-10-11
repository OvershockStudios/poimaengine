# Native two-bone IK

Two-bone IK positions a configured hand or foot after native base playback,
crossfades, inertial transitions and masked layers. It changes the chain's root
and middle rotations, preserving translations, scales and the end node's local
rotation. The end's world orientation can change with its ancestors. Targets
are explicit points; the engine does not discover ground contacts or anatomical
chains.

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

Configured chains require nondegenerate rest segments and positive uniform
rest scales along the model ancestors, root and middle, including disabled
chains. An active weighted chain must also retain positive uniform evaluated
scales along those nodes. Unsupported evaluated poses reject the complete simulation
batch. The wrapper itself may have an invertible positive affine transform.
Hierarchical templates retain the model indices and remap ordinary RigNode
ownership to each spawned instance. Later authored edits leave the running
definition frozen; restart to adopt a changed chain.

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
fixed as the actor moves; `rig` points move with the wrapper. Pole is a point
that selects the bend plane, in the same space as target.

At most 64 combined caller/gameplay IK commands are admitted per Tick.
Duplicate `(entity, slot)` writes and caller/gameplay conflicts reject;
playback and IK may target the same rig. The existing animation-command
budget remains separate. Disabled or zero-weight chains preserve incoming
local poses exactly. No implicit interpolation or target smoothing occurs.

Unreachable goals clamp to the chain's reach in rig coordinates without
stretching. Under an affine wrapper this is not a nearest-point guarantee in
world distance. A collinear pole uses the current bend plane and then a
deterministic Cartesian fallback. Clock histories retain pre-IK poses, so
corrections do not feed into inertial transitions.

## Observe and save

`runtime.ik` requires `session_id`, exact `tick`, `entity` and slot 1–8.
It returns `{session_id, tick, source:"runtime", state}`. Existing nonrig
entities or unconfigured slots return null; missing entities reject. A state
contains entity, slot, root/middle/end indices, enabled, space, target, pole,
weight, world `end_position`, world-distance `target_error`, `clamped` and
`pole_fallback`. These are cached committed observations: inspection does not
sample animation or advance time. Skinning and attached HitRegions consume
the corrected committed pose.

Commands participate in complete physics/gameplay/animation/component/save
rollback. Removing an instance removes its IK state; newly spawned rigs use
frozen defaults, while surviving rigs retain controls. IK-configured worlds
use nested animation-state version 4, saving controls and frozen chain
identity rather than derived solver diagnostics. Fresh restore reconstructs
the pose at the saved physical boundary. Worlds without IK retain prior
animation-state serialization. Existing save-upgrade plans do not authorize
adding, replacing or remapping frozen IK chains.

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

[Recorded 0.0.118 qualification](evidence/m2-two-bone-ik.json) covers native
and JSON-RPC checks, actual CoreCLR and Native AOT consumers, exact durable
continuation and eight forward/deferred GPU captures on two adapters. Complete
pixels and HDR/depth/normal probes match the independent static reference
exactly. This contract does not establish automatic foot grounding, full-body
IK, joint limits, root-motion controller integration, game-scale performance
or Alpha completion.
