<!-- SPDX-License-Identifier: Apache-2.0 -->

# Physical rigs

`PhysicalRig` binds calibrated rigid-body proxies and passive joints to an
imported `AnimationRig`. Animation owns the skeleton initially; explicit
activation transfers its committed pose and momentum to native physics. Solved
physical poses then drive the render hierarchy.

This experimental integration has bounded native, CLI, CoreCLR and complete-save
qualification on Linux headless and Windows, plus Windows Vulkan skin-publication
checks. [Evidence](evidence/m2-physical-rigs.json) separates those workflows from
synthetic ABI tests and preserved failed attempts. These small procedural rigs do
not establish production humanoid quality, active balance or recovery.

## Author a profile

Add `PhysicalRig` to the same entity as the existing `AnimationRig`. Part `node`
values are original model-local indices, not live entity IDs or skin-joint-list
positions. The runtime requires the complete model's `RigNode` mapping with its
original direct-parent relationships. The physical root must be a model ancestor
of every mapped part, and each physical joint's parent must be a model ancestor
of its child. The component uses no additional asset or editor-only rig state.

This two-part example assumes model node 0 is the pelvis and node 1 is its child,
0.5 metres above it. Preserve the owner's existing `AnimationRig` fields and its
matching imported nodes:

```json
{
  "root": "pelvis",
  "initial_mode": "animated",
  "parts": [
    {
      "name": "pelvis",
      "node": 0,
      "body_in_bone": {
        "position": [0, 0, 0],
        "rotation": [0, 0, 0, 1]
      },
      "shape": {"kind": "sphere", "radius": 0.16},
      "mass": 8,
      "friction": 0.5,
      "restitution": 0,
      "linear_damping": 0.05,
      "angular_damping": 0.05
    },
    {
      "name": "chest",
      "node": 1,
      "body_in_bone": {
        "position": [0, 0, 0],
        "rotation": [0, 0, 0, 1]
      },
      "shape": {"kind": "capsule", "radius": 0.12, "half_height": 0.2},
      "mass": 10,
      "friction": 0.5,
      "restitution": 0,
      "linear_damping": 0.05,
      "angular_damping": 0.05
    }
  ],
  "joints": [
    {
      "name": "spine",
      "parent": "pelvis",
      "child": "chest",
      "parent_frame": {
        "position": [0, 0.25, 0],
        "rotation": [0, 0, 0, 1]
      },
      "child_frame": {
        "position": [0, -0.25, 0],
        "rotation": [0, 0, 0, 1]
      },
      "limits": {
        "kind": "swing_twist",
        "swing_y": 0.7,
        "swing_z": 0.7,
        "twist_min": -0.5,
        "twist_max": 0.5
      },
      "neutral_rotation": [0, 0, 0, 1],
      "rest_stiffness": 12,
      "rest_damping": 1.3,
      "soft_limit_stiffness": 20,
      "soft_limit_zone": 0.1,
      "max_torque": 100,
      "friction_torque": 0
    }
  ]
}
```

Every displayed field is required. Objects reject unknown fields, missing fields,
wrong vector dimensions, nonfinite numbers, numeric strings and Boolean numeric
substitutes. `initial_mode` is exactly `animated` or `passive`. Discover the
component schema through `world.describe`; semantic tree, calibration and native
ownership checks supplement JSON Schema.

Shapes are centered `box` (`half_extents`), `sphere` (`radius`) or Y-axis `capsule`
(`radius`, `half_height`). Capsule half-height describes the cylindrical section;
zero selects a sphere. Shapes use explicit uniform-density analytic mass and
principal inertia. They do not derive anatomy or mass distribution from a render
mesh. Boxes have no hidden convex-radius rounding.

`body_in_bone` calibrates the centered proxy/COM frame relative to the mapped
bone. Its translation and primitive dimensions are metres, independently of
imported model units and render scale. Joint frames are local to their respective
part COM frames. Rotations use XYZW unit quaternions; joint angles use radians.

There must be 1–64 unique parts and exactly `parts - 1` joints forming one
connected directed tree. Part names, part node indices and joint names are
unique. Names contain 1–64 ASCII characters matching
`[A-Za-z0-9][A-Za-z0-9_.-]*`; node indices are integers in 0–9999 and must exist in
the actual imported model. Hierarchical templates retain these model-local
indices while remapping their entity references when instantiated.

## Ownership and initial limits

An Animated rig has allocated native bodies outside the broadphase. Animation
continues normally; those allocations are not continuously moved to animation
targets. Activation copies the final committed world-space COM poses and their
finite-difference linear/angular velocities. The first committed sample has
explicit zero velocities. Later samples advance once per committed 60 Hz tick;
reads and repeated pose publication do not advance this history.

Passive bodies enter the broadphase with native joints. Directly connected parts
ignore collision with each other; other part pairs retain collision. Each rig has
its own collision group. Physical solutions update mapped bones and the wrapper
while unmapped bones retain their sampled local animation. Final transforms are
republished after animation sampling and synchronization before presentation.

The initial integration rejects:

- An ordinary collider/controller on the PhysicalRig wrapper.
- Configured root motion or IK/grounding on that rig, including disabled
  configurations.
- A controller, moving ordinary body, animated `RigNode` or another PhysicalRig
  in its ancestry.
- Ordinary box/mesh colliders or controllers beneath the PhysicalRig. Their
  native poses would otherwise remain separate from the moving render hierarchy.
- Reflection, shear, nonuniform scale, or changes to the binding's frozen positive
  uniform world scales.

Static ancestry and unrelated ordinary bodies remain available. These are
initial ownership limits, not the final FPS feature boundary. Coordinated
controller/root-motion/IK transfer, attached equipment bodies, active balance,
physical animation, recovery blending and stand-up behavior remain separate
work. No public command returns a Passive rig to Animated mode.

## Joint behavior

Hinge limits use `{"kind":"hinge","minimum":-1,"maximum":1}`. The hinge axis is
constraint X; its Y axes define zero angle. Swing/twist joints use X for twist
and the pinned native solver's quaternion ellipse for Y/Z swing. The backend
maps `swing_y` to its plane half-cone and `swing_z` to its normal half-cone.

The native solver owns anchor constraints, hard angular limits and joint
`friction_torque`. The additional passive response provides:

- A sign-invariant SO(3) rest potential `U = k * (1 - cos(theta))` about
  `neutral_rotation`.
- Smooth approach energy in `soft_limit_zone` before the hard angular endpoints
  and cone boundary. Its potential is C2 across the approach start and endpoint.
- Relative angular damping `-rest_damping * (omega_child - omega_parent)`.
- One nonnegative saturation scale for the sum of elastic and damping torques,
  bounded by `max_torque`, applied as equal/opposite world-space internal torques.

The energy gradient uses central Lie perturbations of `1e-5` radians. Soft-limit
response rejects the ambiguous 180-degree swing/twist chart neighborhood instead
of inventing an angle. Native joint friction is not applied a second time by the
passive evaluator.

The damping contribution has nonpositive relative damping power. Reported
potential energy is the **unsaturated authored potential**: torque clipping does
not turn it into the integral of the clipped response. This is not a proof of
passivity, energy conservation or monotonic kinetic-plus-potential energy for the
constrained discrete simulation.

A completely sleeping rig is not woken solely to apply its passive torques. If
any link is active, sleeping links are activated before paired torques are
applied; active links' sleep timers are not reset every tick. Explicit impulses
use native activation behavior.

## CLI/service commands

Use the shared service's `runtime.step` operation. Caller commands apply on the
first tick of a batch; compiled-game commands apply on their callback's tick.
`physical_rigs` is optional and defaults to an empty array:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "runtime.step",
  "params": {
    "session_id": "00000000000000000000000000000300",
    "request_id": "00000000000000000000000000000200",
    "expected_tick": 120,
    "ticks": 1,
    "physical_rigs": [
      {
        "entity": "00000000000000000000000000000400",
        "activate": true,
        "impulse": {
          "part": "chest",
          "world_point": [0, 1.5, 0],
          "impulse": [0, 0, 12]
        }
      }
    ]
  }
}
```

Replace the IDs, tick and impact position with actual inspected state. Supply the
current `expected_structure_revision` after runtime membership changes.
`activate` defaults to false; at least activation or an impulse is required.
Activation requires a currently Animated rig. An impulse requires Passive
ownership, or activation earlier in that same ordered sequence. It never
activates a rig implicitly.

Caller commands run first in their literal array order, followed by gameplay
commands in their staged order. Multiple impulses targeting the same rig or part
are distinct applications. They are not averaged, dropped or reordered. There
are at most 64 combined commands per tick and at most one activation per rig,
including caller/gameplay combinations. A command may activate and then apply
its own impulse. An impulse placed before activation rejects.

Activation conflicts with same-tick base/layer animation, animation-rate, IK or
root-motion writes to its owner. Passive owners also reject those writes until a
recovery policy exists. Failed commands, callbacks, structural edits, evaluated
poses, physics steps or later ticks roll back the complete enclosing batch,
including membership, body IDs, animation history, constraints, physics state,
gameplay, sound and save intents. A successful staging callback is not proof that
the eventual tick committed.

## Inspect committed state

Query `runtime.physical_rig` with `session_id`, current `tick` and `entity`.
`include_parts` and `include_joints` are optional Booleans, both false by default.
A live entity without PhysicalRig returns `state: null`; a missing entity rejects.
A stale tick rejects. This additive operation is outside the frozen
`runtime-game-session-v1` contract; discover it before use.

The default projection returns `entity`, `mode`, `body_count` and `joint_count`.
It does not copy every body or evaluate every joint response.

| Projection | Meaning |
| --- | --- |
| `parts` | Named original node mapping, exact native body ID, world COM position/rotation and instantaneous velocities, body-space row-major 3×3 inertia, actual mass/material/damping/velocity caps, enrollment/activity/sleep and cap indicators. |
| `joints` | Named relationships and native limits, relative rotation/angular velocity, and `evaluated_passive_child_torque` at the inspected state. |

Part poses are actual native allocations. In Animated mode, an off-broadphase
allocation can retain its initial or last physical pose; it is **not** the current
bone pose or animated target. Joint torque diagnostics are instantaneous
re-evaluations, not a record of the torque applied during a previous step. Reads
do not sample animation, synchronize, wake bodies or advance ticks.

Public quaternion observations use a canonical unit/sign representation.
Canonical sign selects positive W, or the first nonzero XYZ component at W=0.
Portable solver metadata separately retains raw native quaternion components and
warm-start values. Diagnostic normalization is not an exact-byte checkpoint
witness, and instantaneous native velocities are not pose-history finite
differences.

Existing physical queries route all part hits to the rig's logical owner.
Ignoring that exact owner excludes its part allocations; it does not implicitly
ignore arbitrary hierarchy descendants. Exact same-distance hits use stable
owner identity and native body ID tie-breaking. Physical proxies and authored
hit-region/damage-zone queries remain distinct mechanisms.

## C# gameplay

Opt into `IPhysicalRigGame` on the gameplay class. This requires
`physical_rig_v1`, services epoch 7, and an append-only **424-byte** services
prefix. Earlier prefixes retain their layouts; declaring this feature does not
grant unrelated optional features. Host requirements and callback presence are
validated separately from the epoch number.

Inside `Tick`, the API is:

```csharp
PhysicalRigState? rigState = context.GetPhysicalRig(rig);
PhysicalRigPartState? chest = context.GetPhysicalRigPart(rig, "chest");

// Issue once when gameplay decides to relinquish animation ownership.
context.ImpulsePhysicalRig(rig, "chest",
    new Vector3d(0, 1.5, 0), new Vector3d(0, 0, 12), activate: true);
```

`ActivatePhysicalRig`, `ImpulsePhysicalRig` and `SetPhysicalRig` stage copied
commands; borrowed name spans do not escape the callback. Reads continue to show
committed state during that callback. A selective part read does not evaluate all
joints. Missing parts return null; missing live entities reject. Pending spawned
instances have no committed read state, but commands can target their configured
parts after membership publishes. A later removal/conflict still rejects the
whole batch.

Native callback budgets are 256 summary-read attempts, 1,024 selective part-read
attempts and 64 command attempts per Tick, with the same 64 combined caller/game
write limit. These are attempt limits, not only successful-operation counts.

## Bounds and calibration

| Field | Admission bound |
| --- | --- |
| Primitive radius/box half-extent | 0.005–10 m |
| Capsule cylindrical half-height | 0–10 m |
| Part mass | 0.0001–10,000 kg |
| Friction / restitution | 0–2 / 0–1 |
| Linear/angular body damping | 0–10 per second |
| Authored frame translation | Each component within ±10,000 m |
| Quaternion norm error | At most `1e-6` before canonical helpers |
| Swing half-cone | 0.01–3 radians |
| Hinge/twist endpoints | Within ±3 radians, containing zero; width ≥0.01 and at least one extent ≥0.01 radians |
| Rest/soft stiffness | 0–1,000,000 N·m/rad |
| Relative rest damping | 0–10,000 N·m·s/rad |
| Maximum response / joint friction torque | 0–100,000 N·m |
| Soft approach zone | 0.001–0.5 radians, no wider than half the angular interval or either swing half-cone |
| Native world position / impulse point | Each component within ±1,000,000 m |
| Body linear/angular speed | Magnitude ≤1,000 m/s / 10,000 rad/s, including native float projection |
| Relative angular-velocity evaluator input | Each component within ±20,000 rad/s |
| Localized impulse | Magnitude ≤100,000 N·s, impact lever ≤100 m from current part COM |
| Runtime physics capacity | 4,096 allocated ordinary bodies and rig parts combined, including off-broadphase allocations |

Neutral rotation must lie within hard limits; a hinge neutral rotates only about
X. Frames, derived joint-world poses and velocity conversions are validated before
native admission. These bounds are safety/admission ceilings, not recommended
character dimensions, simulation quality settings or performance budgets.

## Checkpoints, durable saves and failure recovery

Step and structural checkpoints retain physical membership, exact IDs/groups,
current/previous committed pose history, root reference, named joint warm starts,
stable global constraint order, raw native physics state and typed active-body/
invalidation-owner metadata. Membership and constraint order are restored before
raw physics state; owner state follows native and character restoration.
Retired owners remain allocated until commit so rollback can recover them.

Durable snapshots preserve every existing legacy field and base version. Frozen
PhysicalRig content adds a separately versioned, checksum-covered `physical`
extension, required even when only templates contain rigs and none are live.
Worlds without such content omit it and retain their old payload shape.

The extension stores:

- The complete **used-slot** body ledger, including released generations and
  exhausted slots, ordinary entity-to-body IDs, named rig-part IDs, unique
  collision groups and their frontier.
- Every live rig's ordered membership, off-broadphase allocation poses, final
  pose history, root reference and named warm starts, plus global constraint
  order.
- Complete typed Global/Bodies/Contacts state and typed active/invalidation owner
  state. Cached retired contact/CCD IDs retain their exact generations and join
  the used ledger; they are not remapped or discarded.

Cold restoration first expands the complete saved live template graph and
validates bootstrap metadata against it. Native bodies are created with their
original IDs from the first allocation. Typed owner arrays are parsed after
native allocator initialization. Legacy nodes, controllers, animation and motion
clocks are reconstructed alongside physical membership; joined native state is
restored after support setup, then physical locals are published before final
saved-local checks.

Legacy ordinary-body rows remain independent duplicate witnesses: restored
shape-origin position, quaternion, instantaneous velocities and activity must
match actual native getters after the joined restoration. Physical membership
and warm starts must match too. Current history and root calibration are checked
against actual final poses without rewriting saved fields. History velocities
remain finite differences, separately from instantaneous solver velocities.

Unknown/missing fields, invalid types/ranges, native-float incompatibilities,
profile/membership/order mismatches, invalid generation lineage and inconsistent
witnesses reject the detached candidate. Failure does not publish a partially
restored world. Keep the previous valid snapshot/content package and inspect the
reported mismatch; resetting IDs, stripping contacts or deleting history fields
is not a supported recovery strategy.

The extension is tied to frozen content and the pinned native physics profile.
The save-upgrade command currently rejects snapshots containing this extension,
including gameplay-only schema changes; it does not strip or migrate physical
state. Keep the matching content and gameplay build for those saves. Supporting
upgrades while preserving physical state requires separate implementation and
qualification.

This is not a general save-schema migration system. The native integration
fixture checks exact warm/cold snapshot continuation, final skin palettes and
structural rollback. Linux qualification composes 23 selected native suites from
separately retained cohorts; their original failed aggregate verdicts remain
failed. Windows passes all 23 selected native tests and the 76-request CLI
workflow. On both platforms, the engine-hosted CoreCLR consumer checks all 33
fields, 75 native boundary comparisons, eleven complete archives and five
whole-byte pairs through rollback and warm/fresh-process continuation.

Six Windows Vulkan captures separately qualify physical skin publication on a
two-part analytic ribbon. Independent CPU skinning/projection checks silhouette
bounds and deep foreground occupancy; nine complete-archive pairs establish
capture purity and continuation, and three whole-BMP pairs compare restored and
future poses. The original render attempt remains failed after authoring rejected
an invalid emissive value before any GPU capture. Native AOT for this workflow,
production humanoid motion and representative performance remain unqualified.
See the [evidence](evidence/m2-physical-rigs.json) and
[implementation status](IMPLEMENTATION_STATUS.md).

## Reproduce the focused checks

Use a configured simulation build; substitute its path for `build/linux-game`:

```sh
cmake --build build/linux-game --target poima poima-runtime-physical-integration-test --parallel 2
ctest --test-dir build/linux-game -R '^runtime_physical_integration_native$' --output-on-failure
python3 tests/world_physical_contract.py build/linux-game/poima --runtime 1 --output build/physical-cli-check-001
```

The CLI output directory must be new. It retains original requests, responses,
imported input, complete save files and closure results. The native test uses
analytic mass/momentum and skinning oracles; it also requires exact snapshot
continuation, including contact state and subsequent body-ID allocation.

The compiled consumer is
[`tests/managed_physical_gameplay`](../tests/managed_physical_gameplay).
Build it and the [CoreCLR bridge](MANAGED_GAMEPLAY.md) with the supported .NET
toolchain, then run the verifier with those actual compiled inputs:

```sh
python3 tests/managed_physical_gameplay_contract.py /path/to/poima --hostfxr /path/to/hostfxr --bridge /path/to/Poima.ManagedBridge.dll --game /path/to/Poima.ManagedPhysicalGame.dll --output build/physical-gameplay-check-001
```

This checks 33 typed fields, native callback observations, literal repeated
impulses, failed-batch restoration and complete warm/cold save continuation.
It retains raw evidence and requires natural closure of its engine children.
Run it under an owned CI process tree and timeout. Its separate Native AOT
descriptor mode retains the same gameplay assertions; availability of that mode
does not itself qualify a published Native AOT artifact.
