# Camera and weapon pose effects

Camera presentation uses two independent rigid offsets: a world-camera offset
and a foreground weapon/arms offset. Gameplay owns their timing and persistent
state. Apply recoil, bob, sway or shake without replacing the native character's
look or crouch pose. Lens controls remain separate; see [camera lenses](CAMERA_LENSES.md).

## Aim and presentation

`BaseWorld` is the committed camera world matrix with every camera presentation
offset omitted, including offsets on ancestor cameras. Native look, crouch,
physical movement and authored transforms remain included. Compute gameplay
rays from this matrix plus your saved **aim recoil**. Compute camera shake and
weapon kick separately. Visual shake never needs to change bullet direction.

`runtime.entity` and C# `Get(camera).Transform` expose the effective camera
matrix, including the world offset. The selected camera's rendered snapshot
also applies its foreground offset to foreground draw matrices. Foreground
presentation does not move physics, hit regions or logical entity transforms.
The world offset propagates through the camera's transform descendants;
world-pose writes reject cameras with physical descendants. Camera targets
cannot themselves be physics bodies, controllers or animation-owned rig nodes.

Offsets compose on the right of the camera's base local matrix. Position is
in local metres; rotation is an X,Y,Z,W unit quaternion. A selected channel
replaces the previous offset; it is not an incremental transform. Identity
clears the channel. Engine transforms never accumulate the effect into their
base local pose. This also preserves exact legacy arithmetic when no effects
are active.

## Authoritative API

Read `runtime.camera_pose` with the exact `session_id`, `tick` and `entity`.
Its result contains `source: "runtime"` and `state`, with `entity`, `world`,
nullable `viewmodel` and column-major `base_world_matrix` (16 numbers).
A live noncamera returns null; an absent entity rejects. Reads do not advance
the simulation or prove a presented frame. The query is available while an
interactive player runs; refresh the tick guard after a concurrent tick.

Write through guarded, retryable `runtime.step`:

```json
{
  "session_id":"<session>", "request_id":"<request>",
  "expected_tick":12, "ticks":1,
  "camera_poses":[{
    "entity":"<camera>",
    "world":{"position":[0,0.01,0],"rotation":[0,0,0,1]},
    "viewmodel":{"position":[0,0,0.025],"rotation":[0,0,0,1]}
  }]
}
```

Provide at least one complete channel. Omitted channels keep their offsets;
null and empty commands reject. Translation must be finite and within ±10 m
per axis. Quaternion components must be finite and within ±1.00001, with
squared norm within 0.00001 of one. Stored values are not clamped or rewritten. Matrix evaluation uses the same
normalized-quaternion convention as authored transforms; smoothing is caller-owned. A foreground offset requires an authored foreground
lens. Camera and lens membership cannot be added by a pose command.

At most 64 unique cameras may be written in one tick, including caller and
compiled-game commands. Caller controls apply before the first callback of a
batch. Compiled setters stage until the callback succeeds; reads still return
callback-entry committed values. Duplicate or conflicting targets reject.
A later failure rolls back the complete batch, including offsets and game
state. Reserved camera births accept staged controls; retirement conflicts
reject, and a later birth starts with its own identity offsets.
For a reserved camera birth, `BaseWorld` uses its authored recipe and committed
ancestors; it does not predict the birth's first animation sample.

## C#

Declare `ICameraPoseGame`. This independently negotiates `camera_pose_v1`;
it does not grant lens or animation permissions.

```csharp
var pose = context.GetCameraPose(camera)
    ?? throw new InvalidOperationException("Camera is missing.");
Vector3d aimOrigin = pose.BaseWorld.Position;
Vector3d baseForward = pose.BaseWorld.Forward;
// For local aim recoil, rotate local -Z, then transform that direction
// through BaseWorld's basis before a hit query.
context.SetCameraPose(camera,
    world: new CameraPoseOffset(new(0, 0.01, 0), Quaterniond.Identity),
    viewmodel: new CameraPoseOffset(new(0, 0, 0.025), Quaterniond.Identity));
```

These methods are Tick-only; getters allow at most 256 reads per callback.
Store spring positions/velocities, shot index, seed and timing in supported
primitive game-state fields. Helper records are transient values, not new
supported nested save-field types. The optional [FPS package](../packages/fps/README.md)
provides deterministic procedures for these effects.

## Persistence and compatibility

Active offsets are saved as a sorted sparse `camera_poses` snapshot extension,
version 1. It records both complete channels for each included camera; identity
cameras are omitted. Old/default snapshots retain their original shape.
Restoration reconstructs omitted identity channels with canonical defaults;
signed-zero bits in an identity command are not preserved through that path.
Cold restoration validates camera membership, channel presence, bounds and
physical ownership before publishing the restored state. Rollback retains full offsets.

The call ABI remains 1/80 and the service epoch remains 7. The new 392-byte
service table preserves the earlier 376-byte prefix and appends getter/setter
pointers at offsets 376/384. Version-1 commands are 144 bytes; returned states
are 272 bytes, including the base matrix at offset 144. Older declared service
prefixes and permissions remain unchanged. Matched SDK/bridge cohorts validate
marked-game negotiation before game construction.

Pose interpolation between simulation ticks, collision-aware weapon lowering,
full-body weapon IK and production animation authoring remain separate work.
