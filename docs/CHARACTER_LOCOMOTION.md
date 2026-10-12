# Character locomotion

An optional `CharacterController.locomotion` profile selects a native Jolt
CharacterVirtual motor. Its collision sweeps support walking over steps,
walkable slopes, floor following and motion relative to supported kinematic
platforms. The existing [character input](CHARACTER_INPUT.md) and
[stance](CHARACTER_STANCE.md) APIs control it; gameplay needs no new service
extension. Omitting the profile retains the physical-character backend.

## Configure a controller

```json
{
  "radius": 0.3,
  "height": 1.8,
  "speed": 4,
  "jump_speed": 5,
  "camera": null,
  "locomotion": {
    "step_height": 0.4,
    "ground_snap": 0.5,
    "max_slope_degrees": 45
  }
}
```

`locomotion:{}` selects these defaults. Each field is optional. Step height and
ground snap are finite distances in `[0,1]` metres; maximum slope is a finite
angle in `[0,80]` degrees. Zero step height or ground snap disables the respective
correction. Values must remain valid when converted to native floats. Null,
unknown keys, strings and Boolean numeric values reject. There is no authored
`mode` key: supplying the profile selects `virtual-v1`.

The profile is frozen when the runtime starts and is copied into hierarchical
instances. Editing the authored world does not patch a live motor. Ordinary save
restores and custom upgrade plans cannot silently change the frozen backend or
profile. A controller can also declare `stance`; both systems use the same
foot origin, radius, actual posture and controlled child-camera height.

## Movement and observation

The motor integrates once per fixed 60 Hz tick after caller/gameplay input, stance,
platform target velocities and planar root motion have been prepared. Physics
then advances bodies, and the motor refreshes contacts against their final
poses. If that refresh loses a solve-preserved support contact, a bounded
downward sweep can recover floor support when the motor is descending and not
jumping. The reported velocity includes this final correction. Gravity is integrated once. Stair and floor corrections do not become
ballistic vertical speed on the next tick. Blocked upward jumps stop pressing
into ceilings.

Movement retains the existing heading-relative, direct-control policy. Neutral
input clears requested horizontal movement, including in air. A jump requires walkable
ground support. Floor following does not pull an upward jump back down; a drop
outside the configured correction distance uses gravity. Step height alone
does not guarantee passage: the upright capsule still needs horizontal and
overhead clearance. Mesh collision and authored winding remain relevant.

When supported and not moving upward relative to the ground, the motor composes requested
movement with the support body's velocity at the contact point. This includes
translation and rotation of kinematic platforms. It does not rotate the
character's heading automatically. The existing air-control policy does not
retain horizontal platform momentum after neutral airborne input. Arbitrary
elevators, rapidly changing support, dense character stacks, dynamic pushes and
moving navigation remain broader qualification work.

Schema revision 94 adds `locomotion` to guarded `runtime.entity` results only
for configured controllers:

```json
{
  "mode": "virtual-v1",
  "profile": {
    "step_height": 0.4,
    "ground_snap": 0.5,
    "max_slope_degrees": 45
  },
  "ground_normal": [0, 1, 0],
  "ground_velocity": [1, 0, 0]
}
```

Ground vectors are cached native contact observations; they are meaningful in
conjunction with the existing `ground` state. Reading them performs no physics
query or simulation step. Session and tick guards still apply.

After a successful tick, public `velocity` is the actual foot displacement
divided by that tick's duration, including bounded stair/floor corrections and
support movement. Running into a wall therefore reports stopped movement.
Jolt's integration velocity is retained separately for gravity and save state.
Inside a C# callback, `Get` observes pre-physics state after caller/neutral input
preparation; velocity can already reflect that preparation. A staged compiled
setter still takes effect only after the callback succeeds.

[Root motion](ROOT_MOTION.md) supplies the planar movement intent before the
motor sweep. Its applied displacement measures the actor's full resulting
movement, including platform transport; it does not separately attribute
animation movement and support motion.

## Query bodies and ownership

Each motor owns a kinematic collision/query proxy with the controller's stable
body identity. The proxy is offset upward by the native 0.02 m character padding
and retains zero linear/angular velocity; publishing its pose does not cause
physics to integrate character motion twice. Motor sweeps exclude their own
proxy. Other bodies and raycasts see the capsule at its committed pose.

Candidate construction does not add a body to the active physics world. Spawn
publication, retirement, shape changes and rollback retain existing ownership
boundaries. Capsule stance changes publish both the motor and proxy shape.
The controller and proxy remain part of the existing body capacity budget.

The pinned Jolt contact collector has a bounded hit capacity. The integration
rejects a reported collection overflow and rolls back the batch. Jolt's flag
describes the most recent collection; this is not qualification of every
intermediate collection in arbitrary dense geometry.

## Saves and rollback

Frozen content containing a locomotion profile uses runtime snapshot version 9,
including when the only such profile belongs to an unused template. Each live
virtual controller saves `motor.position`, `motor.rotation`, reported
`motor.linear_velocity` and raw `motor.integration_velocity`. Its separate
proxy body must match the saved logical pose plus padding and have zero
independent velocity. Invalid fields, contradictory poses and out-of-budget
velocities reject before installation.

Cold restore installs saved stance shapes before body poses, restores all
support bodies before motors, then rebuilds contacts. Complete saved state
includes posture intent, owned camera state and integration/reporting velocity.
Older snapshots cannot be treated as virtual state merely by changing their
version. Worlds without the profile retain their existing snapshot versions.

An in-process batch checkpoint additionally retains Jolt's virtual contact
state. Failure on a later tick restores the whole batch, including motor,
physics, compiled gameplay and pending commands. Portable saves reconstruct
contacts; they do not serialize Jolt's internal warm contact caches. Compare
independently cold-restored continuations when testing portable reproducibility.

## Reproduce

Build and run the native geometric checks:

```sh
cmake --build --preset runtime-headless --target poima-runtime-character-locomotion-test
./build/runtime-headless/poima-runtime-character-locomotion-test
python3 tests/runtime_character_locomotion.py \
  --binary build/runtime-headless/poima \
  --output build/locomotion-protocol-check
```

For compiled gameplay, build
`tests/managed_character_locomotion_gameplay/Poima.ManagedCharacterLocomotionGame.csproj`
against the matched SDK and run `tests/character_locomotion_gameplay_contract.py`
with `--hostfxr`, `--bridge`, `--assembly` and a fresh `--output` directory.
A Native AOT consumer uses `--native-descriptor` instead of those three managed
paths. Add `--windows-interop` when invoking a Windows executable from WSL.
The fixtures require an external process owner with a bounded timeout.

These synthetic tests target collision geometry, fixed-tick controls and
reconstruction. They do not establish physical-input feel, animated foot
placement, swimming, arbitrary crowded environments or production performance.
