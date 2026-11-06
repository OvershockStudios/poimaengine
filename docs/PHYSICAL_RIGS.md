<!-- SPDX-License-Identifier: Apache-2.0 -->

# Physical rigs

`PhysicalRig` binds calibrated rigid-body proxies and passive joints to an
imported `AnimationRig`. Animation owns the skeleton initially; explicit
activation transfers its committed pose and momentum to native physics. Solved
physical poses then drive the render hierarchy.

Optional [joint drives](PHYSICAL_DRIVES.md) add strength-controlled orientation
targets to selected joints. They preserve native contacts, passive responses
and friction. Driven rigs retain physical ownership; balance and recovery are
separate systems.

An opt-in [animation target player](PHYSICAL_ANIMATION.md) streams original clip
and layer poses to those drives, with independent playback and explicit joint
limit fitting. It supplies desired orientations while native physics continues
to solve the visible body.

The initial 0.0.218 integration has bounded native, CLI, CoreCLR and complete-save
qualification on Linux headless and Windows, plus Windows Vulkan skin-publication
checks. The opt-in fixed-scale frame additionally has licensed full-body
[Linux](evidence/m2-skeletal-physical-rigs.json) and
[Windows](evidence/m2-skeletal-physical-rigs-windows.json) workflows, plus selected
[Vulkan observations](evidence/m2-skeletal-physical-rigs-rendering.json). Earlier
[evidence](evidence/m2-physical-rigs.json) covers separate procedural fixtures.
Production humanoid motion/contact quality, active balance and recovery remain
separate.

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

Every displayed field is required. The optional `pose_frame` field is described
below. Objects reject unknown fields, missing fields,
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
  configurations, except the explicit [manual-IK ownership](#manual-ik-ownership)
  policy below.
- A moving ordinary body, animated `RigNode` or another PhysicalRig in its
  ancestry. Controller ancestry requires the explicit root-actor link described
  in [Controller handoff](#controller-handoff); unlinked ancestry rejects.
- Ordinary box/mesh colliders or controllers beneath the PhysicalRig. Their
  native poses would otherwise remain separate from the moving render hierarchy.
- Reflection, invalid or changing admitted scales. The default frame additionally
  rejects skeletal shear and nonuniform scale; the opt-in frame below preserves
  fixed nonuniform local scales.

Static ancestry and unrelated ordinary bodies remain available. These are
initial ownership limits, not the final FPS feature boundary. The opt-in
controller handoff below supports one-way takeover; opt-in manual IK uses the
ownership policies below. Coordinated root motion, attached equipment bodies, active balance,
physical animation, recovery blending and stand-up behavior remain separate
work. No public command returns a Passive rig to Animated mode.

### Skeletal frame selection

`pose_frame` accepts `rigid_uniform`, `skeletal_fixed_scale` or
`skeletal_bounded_scale`. Omission selects
`rigid_uniform`; canonical serialization omits this default to preserve earlier
configuration bytes. The default keeps its existing arithmetic and bounds.

With `skeletal_fixed_scale`, every original node retains its exact initialized
positive local scale, including nodes without physical proxies. The initialized
sample comes from the authored starting animation, rather than an averaged or
normalized model rest pose. A later scale difference rejects atomically, even if
it comes from another internally constant-scale clip. Select the intended initial
clip before starting the runtime. This policy does not support arbitrary scale
curves or blends that change the initialized scales.

A bone's rigid axes are the ordered quaternion product through its ancestry;
its origin is the translation of its actual full affine world matrix. Metric
`body_in_bone` offsets use those logical axes. After physics, the complete parent
affine inverse recovers local translation, the logical parent quaternion recovers
local rotation, and the original local scale remains unchanged. Unmapped nodes
retain their sampled locals. A sheared visual basis is therefore distinct from
the explicit orthonormal frame of its rigid proxy; the render mesh is not silently
repaired or converted into a rigid collision surface.

Local scale axes and world-column norms must lie in `1e-8..1e8`. The world linear
inverse infinity norm is at most `3e8`, and its infinity-norm condition number is
at most `1e6`. All complete candidate transforms must remain finite and reproduce
the requested origins and logical rotations within the documented native bounds
before publication. The wrapper and external ancestry retain their proper,
positive-uniform requirements. Every existing ownership guard still applies.

The frozen local-scale baseline derives from original content and birth state
before a saved playback cursor is restored. It is not re-frozen from a passive
snapshot. The C# 424-byte services extension is unchanged; native C++ consumers
must rebuild against changed configuration headers.

### Bounded source-scale composition

Use `skeletal_bounded_scale` when original clips or their composition change
local scales slightly. Add these two fields to the complete `PhysicalRig`
profile; every part, joint and other required field remains required:

```json
{
  "pose_frame": "skeletal_bounded_scale",
  "max_scale_deviation": 0.00001
}
```

The bound is dimensionless, finite, greater than zero and at most `0.01`.
For every axis of every original node, admission requires
`abs(current - initial) <= max_scale_deviation * initial`. The reference is the
actual initialized animation sample used to construct the binding, including
original birth configuration. It is immutable and is not recaptured from a
saved cursor or a later animation. Both earlier policies forbid an authored
`max_scale_deviation` field. `skeletal_fixed_scale` still rejects a one-ULP
change; omission still selects the earlier uniform frame and canonical bytes.

The runtime retains the actual composed scale values. It does not round them,
normalize source keys, clamp a rejected value or drop an animation layer.
Admission checks every final node, including nodes without physical proxies,
after crossfades, layers and inertial corrections. Source key extrema alone
cannot prove that a composition fits: additive layers and inertial corrections
can exceed their endpoints. A rejected sample identifies its node, axis,
reference, current value and bound. The complete fixed-tick batch rolls back;
several earlier ticks in that batch do not remain applied.

The same rules apply in Animated and Passive modes. Existing animation clocks
continue sampling; solved physics overrides mapped positions and rotations,
while every node retains its actual sampled scale. There is no hidden freeze at
takeover. Bone origins use full affine transforms and rigid axes use logical
quaternion chains, with the same positive-scale and conditioning bounds above.

Rigid proxies remain metric. Their shape dimensions, COM offsets, mass,
inertia and joint anchors do not resize with visual scale. This policy admits
small visual-scale variation; it does not implement deforming collision bodies
or establish proxy quality for every permitted composition. Authors must select
an appropriate envelope and calibrated proxies for their content.

Cold restore derives the baseline from original content and birth state before
restoring animation clocks, layers and inertial history. Those existing saved
states reconstruct the current values; no separate mutable scale buffer is
added. Every saved `RigNode` scale on a bounded rig must equal its reconstructed
value exactly, including unmapped nodes. Altering a redundant saved scale by one
ULP rejects instead of silently replacing it with reconstructed data. Other
policies retain their earlier save validation. The C# 424-byte services extension
is unchanged; native C++ configuration consumers must rebuild.

The [Linux checks](evidence/m2-bounded-skeletal-scales.json) cover declared
boundaries, independent affine/COM publication, crossfades, continued passive
scales, complete cold saves, template lifecycle, additive/inertial overshoot
rollback and forged saved-scale rejection. They retain the earlier failed
float-precision oracle attempts separately. The original-character workflow
uses three existing takes in one world; it does not establish every possible
layer, rate, interruption or exporter combination.
[Windows CPU qualification](evidence/m2-bounded-skeletal-scales-windows.json)
separately passes all 29 selected native suites and the same original-character
sequence, with retained CoreCLR consumers. Rendered qualification for this
frame remains separate.

### Imported full-body specimen

[Physical Character Yard](../examples/physical-character-yard/README.md) supplies
a reviewable 15-part, 14-joint, 72 kg profile for the unchanged CC0 KayKit Knight.
It retains all 57 original nodes, 41 skin joints and 76 animation takes. The original
Idle and Running_A takes have different initialized scales and are exercised in
separate worlds; no scale-channel removal, crossfade or asset repair is used.
The original base animation plays for six committed ticks before takeover.

Capsule directions and shoulder/wrist twist axes derive from actual source
segments. Torso proxies use explicit source landmarks. The source audit records
all nonadjacent proxy pairs, including remaining overlaps; it does not add
collision exclusions or require zero internal contact in every authored pose.
Geometry coverage and source math alone do not establish contact quality. The
profile's identity neutral rotations refer to its original rest/T-pose joint
frames, so nonzero rest stiffness can bias a fallen body toward that pose.
Relaxed neutrals need explicit calibration within hard limits.

The [0.0.219 Linux qualification](evidence/m2-skeletal-physical-rigs.json) passes
40 selected CTest checks and the 758-request full-body workflow. It compares all
57 initialized/played nodes against original source FK, independently checks
saved COM and angular finite-difference history, then checks actual first-step
aggregate momentum with gravity and drag. Individual post-solver velocities may
change through joint and contact impulses. Both selected worlds fall, receive an
offset shoulder hit, settle and wake; one has stairs and the other a slope.
Eighteen complete archives and eight whole-byte pairs cover same-owner warm and
fresh-process cold restore plus future continuation across four naturally closed
CLI owners. The first verifier attempt remains failed after requesting a compact
projection and then indexing detailed parts; explicit detail flags correct the
verifier without changing engine bounds or the gameplay assertions.

The [Windows CPU workflow](evidence/m2-skeletal-physical-rigs-windows.json) also
passes both original takes. Separate
[Windows Vulkan checks](evidence/m2-skeletal-physical-rigs-rendering.json) retain
eleven actual captures: eight isolated original right-arm views with independent
source skin/projection checks, and three complete original-material character
views. The isolated mesh keeps all original nodes and physical bodies; it is an
inspection fixture, not a whole-character pixel oracle. Four whole-BMP pairs
match through warm/cold restore and future continuation. Visual review confirms
visible original materials and articulated poses on the steps, without
establishing motion quality. The first full-mesh diagnostic remains failed
because its stale-palette control had no exposed witnesses.

A retained two-part Native AOT consumer separately passes on the 0.0.219 Linux
host; that does not qualify full-body Native AOT, production humanoid quality,
active control, controller transfer or representative performance.

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

Activation conflicts with same-tick base/layer animation, animation-rate, IK,
grounding or root-motion writes to its owner. Passive owners also reject those writes until a
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

This is not a general save-schema migration system. The 0.0.218 native integration
fixture checks exact warm/cold snapshot continuation, final skin palettes and
structural rollback. That checkpoint's Linux qualification composes 23 selected native suites from
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
an invalid emissive value before any GPU capture. A separate
[Linux Native AOT execution](evidence/m2-physical-rigs-native-aot.json) passes
the complete compiled-game workflow with dynamic code disabled. The publication
supervisor's adopted-descendant failure remains failed; independent artifact
admission and native execution use a separate naturally closed owner. Windows
AOT, standalone export, production humanoid motion and representative performance
remain unqualified for this physical-rig consumer.
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

For the imported full-body specimen, obtain the unchanged source and license
listed in [Physical Character Yard](../examples/physical-character-yard/README.md).
Use a 0.0.219+ build advertising the fixed-scale frame, fresh output directories
and an external process-tree owner:

```sh
python3 -B tests/physical_character_calibration.py
python3 -B tests/physical_character_source.py --source /path/to/Knight.glb --license /path/to/LICENSE.txt
python3 -B tests/physical_character_native.py /path/to/poima --source /path/to/Knight.glb --license /path/to/LICENSE.txt --output build/physical-character-check-001
```

The first two checks inspect original source/math without running the engine.
The native verifier preserves the complete imported library, literal RPC traces,
all detailed observations and save archives. It requests `include_parts` and
`include_joints` explicitly; the default physical projection is compact.
The source recipe must reproduce every byte of the selected checked-in profile.
These checks launch no compiler, renderer or content-generation tool.

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
Run it under an owned CI process tree and timeout. The separate Native AOT
descriptor mode retains the same gameplay assertions:

```sh
python3 scripts/publish_native_gameplay.py --project tests/managed_physical_gameplay/Poima.ManagedPhysicalGame.csproj --type Poima.Tests.PhysicalGame --dotnet /path/to/dotnet10/dotnet --rid linux-x64 --engine-version 0.0.218 --output build/physical-aot-check-001/artifact --work build/physical-aot-check-001/work
python3 tests/managed_physical_gameplay_contract.py /path/to/poima --native-descriptor build/physical-aot-check-001/artifact/native-gameplay.json --output build/physical-aot-check-001/verification
```

The publisher needs a supported matching-host .NET 10 SDK, Native AOT packages
and native linker prerequisites; see [native gameplay](NATIVE_GAMEPLAY.md).
All three output directories must be new. Publication rebuilds the game source;
it does not promise identical IL to an earlier CoreCLR build. The verifier admits
the complete descriptor, payload inventory and typed schema before loading the
native export, then checks actual callbacks, rollback and cold continuation.
Whole-archive equality is checked within each backend's run; CoreCLR and Native
AOT archives have different backend/library identities.

## Controller handoff

A physical rig can take over from an explicitly linked virtual character.
The controller must be an unscaled hierarchy root with `locomotion` configured;
its `AnimationRig`/`PhysicalRig` wrapper must be a direct child. Add this member
to the existing complete physical profile:

```json
"controller_handoff": {
  "actor": "00000000000000000000000000000100",
  "follow": "translate_keep_heading"
}
```

The actor ID names the controller, not a bone. The profile must start
`animated`. Run at least one completed native tick before activation; a rig
cannot transfer in its birth tick. Hierarchical templates use recipe-local
actor IDs, remapped with the complete instance. The link is frozen content.
One actor owns one linked rig. Additional colliders/controllers, another physical
rig and root motion in that actor's subtree are unsupported. Manual IK requires
its own rig's explicit [procedural ownership](#manual-ik-ownership); automatic
grounding and IK on other descendant rigs remain unsupported.
An outside rig also cannot reserve the linked actor as its root-motion target.
These ownership restrictions apply even to disabled configurations.

Use the existing ordered `physical_rigs` activation command or
`GameContext.ActivatePhysicalRig`. Takeover uses the last committed final pose
and its COM/angular finite-difference history, including resolved controller
and support motion. It does not add motor velocity again. The movement capsule
leaves broadphase before the articulated bodies enter it, retaining its exact
allocated body ID. Returning to locomotion requires a future recovery policy;
repeated activation rejects.

Valid held move, look, jump and stance requests are superseded for the actor on
the takeover tick and subsequent ticks. Malformed inputs, duplicate writers and
command-budget violations still reject the complete batch. Other actors retain
their controls. During `Game.Tick`, caller-input preparation remains visible
through a pure, joined actor/camera/HitRegion preview. It does not rotate the
native capsule, touch contact caches or rewrite committed pose history. Staged
gameplay movement and activation remain invisible until the normal command
boundary. Explicit native ray/capsule queries still use real collision geometry.
Ignoring the linked actor also ignores its physical parts; HitRegion owner-proxy
suppression follows the same ownership route.

`translate_keep_heading` follows the solved wrapper's translation while retaining
the actor's captured world heading. Actor and wrapper locals are prepared
together, preserving their scales and absolute solved bone poses. An owned
camera inherits that translation and retains its captured local pitch and
stance height. Camera effects retain their separate controls. This is a simple
death-camera policy, not an eye attachment, spectator controller or polished
cinematic transition.

### Inspect ownership

`runtime.controller_handoff` accepts exact `session_id`, `tick` and rig `entity`
guards. Its envelope contains `session_id`, `tick`, `source: "runtime"` and
`state`; an existing unlinked entity returns `state: null`, while a missing
entity rejects. This additive method is outside runtime-game-session-v1.

| State field | Meaning |
| --- | --- |
| `entity`, `actor` | Physical wrapper and its frozen controller reference. |
| `phase` | `locomotion` or `physical`. |
| `capsule_body_id` | Full native allocation identity, retained after removal. |
| `capsule_enrolled` | Actual broadphase membership. |
| `takeover_tick` | Last committed pose boundary consumed by activation; null during locomotion. |
| `support` | Null during locomotion; otherwise `semantics: "cached_at_takeover"`, ground state, body ID, normal and velocity. |

Suppressed `runtime.entity` controller velocity/grounding and stance observations
retain the last motor state. They do not describe current corpse motion or
support. Read physical parts for current native velocities and the entity world
transform for current followed position. Cached support IDs can name retired
bodies; `4294967295` means no support. Navigation refuses a suppressed controller
rather than starting from its frozen motor position. Reads never sample, tick,
wake or publish proxies.

### Preserve the ownership phase

Frozen content containing links, including dormant templates, uses physical
snapshot extension 2. It adds named per-rig handoff state: phase, takeover tick,
follow reference, frozen support and exact off-world capsule witness. Existing
body/controller fields remain present. Content without links retains extension 1.
Cold construction installs suppression before support queries or synchronization,
replays the original logical-to-native COM publication, and verifies the exact
saved proxy. Complete rollback restores capsule membership, IDs, shape, native
physics state and ownership together. Linked actor and rig retire together.

[Linux qualification](evidence/m2-controller-handoff.json) covers native and
CoreCLR ownership, joined callback aim, exact saves, lifecycle and negative
queries. The [Windows follow-up](evidence/m2-controller-handoff-windows.json)
records native and CoreCLR execution, the original transcript-audit failure and
separately recovered workflows with corrected literal RPC recording. It also
corrects the earlier Linux request-history claim without rewriting its records.
Earlier [input-planning checks](evidence/m2-character-input-planning.json) cover
the separate 0.0.221 prerequisites. New-feature Native AOT and broader imported
humanoid quality remain open.

The [original-character example](CHARACTER_CONTROLLER_HANDOFF.md) combines this
link with a moving/falling virtual actor, original clip crossfades and all
fifteen articulated bodies. Its selected Windows Vulkan observations retain
all original meshes and materials; source depth, complete saves and restored
images have separate [evidence](evidence/m2-character-controller-handoff.json).
Broader motion quality and character families remain open.


## Manual IK ownership

Add `"procedural_pose": "animated_only"` to a linked `PhysicalRig` to coordinate
its own manual two-bone IK with one-way physical takeover. The profile must have
`controller_handoff`, start `animated`, and map every configured chain's root,
middle and end to physical parts. An IK chain with automatic grounding or planted
contacts rejects, even when disabled. Root motion and independent descendant
pose writers remain unsupported. Omit the policy to retain the earlier ownership
checks and physical save format.

For this policy, place `Camera` on a separate child entity: it cannot share the
linked actor, rig wrapper or any of the rig's model `RigNode` entities, because
camera effects otherwise add a transform writer outside the IK source pose.
Ordinary child cameras, including the controller's eye camera, remain supported.

While animated, source animation and layers compose before the ordinary manual
IK solve. The committed final corrected pose supplies physical COM and angular
finite differences at takeover. Caller movement, look and jump accepted during
that callback are still staged and may be superseded by activation. A callback
observes a pure scratch IK solution under the prepared actor/wrapper transform;
its node transforms and IK diagnostics belong to that same candidate pose. This
preview writes no live clocks, animation history, solver diagnostics or physics.
Staged gameplay IK edits become visible at their usual command boundary. An
activation combined with an animation/rate/IK/grounding/root-motion edit on the
same rig rejects the complete batch.

After takeover, only physics writes the mapped bones. Source/layer clocks and
source scales continue; manual IK no longer solves that rig. Other rigs continue
their own procedural evaluation. Existing IK inspection and compiled gameplay
getters retain the last committed animated controls and diagnostics; those values
are historical, not the current corpse's end position. Use
`runtime.procedural_pose` with exact `session_id`, `tick` and `entity` to distinguish
`phase: "animated"`, `diagnostics: "current"` from `phase: "physical"`,
`diagnostics: "last_animated_pose"`. `sampled_tick` retains the original animated
boundary, and `slots` lists the configured chains. When no policy is configured,
`state` is null;
a missing entity or stale session/tick rejects. The query does not sample or
advance the world. This additive inspection is outside runtime-game-session-v1.

Frozen content containing the policy, including dormant templates, uses physical
snapshot extension 3. Its per-rig `procedural_terminal` is null while animated or
for other rigs; after takeover it retains a separately versioned historical
animation context, captured wrapper and solved diagnostics. Restoration validates
the historical context before installing its ownership and never solves IK under
the corpse's later wrapper. The historical sample tick must equal the controller
takeover tick. The complete current animation, source scales, physical history,
native bodies and controller witnesses remain separate and must also agree.
Extension 1 and 2 retain their earlier shapes when the new policy is absent.

No recovery command is provided. Automatic ground-contact/planting suspension
uses the separate [grounded ownership policy](GROUNDED_PHYSICAL_HANDOFF.md);
root-motion handoff still needs its own controller-driver transfer.
Native C++ configuration consumers must rebuild; the C# services
prefix and ABI epoch are unchanged.


### Qualification and reproduction

The [0.0.225 record](evidence/m2-procedural-physical-handoff.json) qualifies Linux
headless ownership, source/physics observations, exact complete saves and an
actual compiled C++ gameplay callback. Its four-node procedural model has three
mapped spherical proxies; it does not establish imported-humanoid motion quality,
new-feature C#/Native AOT compilation, Windows graphics or representative speed.
The separate original-character regression retains its earlier policy and checks
that existing imported movement, crossfades, takeover and complete saves survive.

In a configured simulation build with native gameplay enabled:

```sh
cmake --build build/linux-game --target poima-runtime-physical-ik-test poima-physical-controller-handoff-json-test --parallel 2
ctest --test-dir build/linux-game -R '^(runtime_physical_ik|physical_controller_handoff_json_native)$' --output-on-failure
```

`POIMA_TEST_ARTIFACTS` may point to a fresh output directory to retain every
complete compared snapshot and the donor-record rejection inputs, with a SHA
manifest and a 128 MiB cap. Without it, the native test writes no archive files.
The shared-library fixture exercises the native gameplay ABI; it is compiled C++
and makes no C# or Native AOT compiler claim. The compatibility record leaves
older physical save extensions unchanged when the policy is absent.


The [0.0.226 follow-up](evidence/m2-procedural-physical-handoff-windows.json)
qualifies the Windows native path and an actual compiled C# CoreCLR consumer on
Windows and Linux. The Windows cohort executes 51 native tests, four owned
inertial-save lifecycle commands and three CLI protocols. Five older Python
protocols that use source-relative scratch directories are explicitly outside
that Windows cohort; the earlier Linux 49-test record remains separate.

The new [C# fixture](../tests/managed_procedural_handoff_gameplay/README.md)
checks all 80 gameplay fields and the complete native state across three fresh
CLI processes per platform. Each run retains 30 archives, 30 observations and
17 complete save pairs. Point and oriented setters, prepared aim, conflicting
writes, late-Tick rollback and passive historical reads execute in real managed
callbacks. This extends development-host qualification; it does not qualify
Native AOT compilation or rendering for the combined procedural handoff.
