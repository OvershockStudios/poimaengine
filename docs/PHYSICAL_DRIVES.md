<!-- SPDX-License-Identifier: Apache-2.0 -->
# Physical joint drives

An opted-in [PhysicalRig](PHYSICAL_RIGS.md) can hold joint-relative orientation
targets using bounded internal torques. Native bodies, contacts, joint anchors,
hard limits and friction continue to determine the motion; solved body poses
continue to drive the mapped skeleton. This is an orientation-hold mechanism,
not a balance controller, gait generator or recovery-to-animation system.

## Authoring

Add this optional object to a joint in the existing PhysicalRig profile:

```json
"drive": {
  "stiffness": 60,
  "damping": 6,
  "max_torque": 10
}
```

These are example coefficients, not universal character settings. The object
requires all three fields and rejects extra fields, nonfinite numbers, numeric
strings and Boolean numeric substitutes.

| Field | Unit | Admitted range |
| --- | --- | --- |
| `stiffness` | N·m/rad | Greater than 0, at most 1,000,000 |
| `damping` | N·m·s/rad | 0–10,000 |
| `max_torque` | N·m | Greater than 0, at most 100,000 |

The drive profile is immutable during a runtime session. It grants permission
to control that joint; omitting it preserves the earlier passive profile and
encoding. Every configured drive starts at the joint's `neutral_rotation` with
strength zero. `initial_mode` remains `animated` or `passive`.

Drive gains and caps are separate from passive `rest_stiffness`, `rest_damping`,
`soft_limit_stiffness`, `max_torque` and native `friction_torque`. Enabling a drive
does not replace those responses. Their combined torque is not bounded by the
drive's cap alone.

## Targets and ordered writes

A target is a finite XYZW unit quaternion describing the child **joint frame
relative to the parent joint frame**. It is neither a world rotation nor a
bone-local rotation. Existing COM-to-joint calibration applies. Quaternion norm
error may be at most `1e-6`; accepted command quaternions use the existing unit
and sign canonicalization. Hinge targets must be rotations about joint X within
the authored interval. Swing/twist targets must fit the authored swing ellipse
and twist limits. Out-of-range targets reject instead of being clamped.

Use the `physical_rigs` array in `runtime.step`. The following array element
performs explicit takeover followed by one drive write:

```json
{
  "entity": "00000000000000000000000000000400",
  "activate": true,
  "drive": {
    "joint": "elbow",
    "target_rotation": [0.17364817766693033, 0, 0, 0.984807753012208],
    "strength": 0.5
  }
}
```

Use the inspected entity, joint, session, expected tick and structural revision;
the example orientation must fit that joint's limits. The surrounding request
and retry guards follow [PhysicalRig commands](PHYSICAL_RIGS.md#cliservice-commands).
For an already physical rig, omit `activate`. Within one command the order is
activation, optional localized impulse, then drive. Across commands, caller
array order precedes staged gameplay order. There are at most 64 combined
physical commands per tick, one activation per rig and one drive write per
rig/joint, including caller/gameplay combinations.

A drive never takes ownership implicitly. An earlier activation in the same
ordered tick is allowed; a drive before takeover rejects. Same-tick animation,
IK, grounding and root-motion conflicts retain the existing physical-ownership
rules. Failure restores the complete enclosing batch, including drive controls,
native state, animation history, gameplay and pending save intents.

In C#, declare `IPhysicalRigDriveGame` on the game class. During `Game.Tick`, an
already identified rig can be controlled with:

```csharp
context.ActivatePhysicalRig(rig); // Only when the committed rig is Animated.
context.SetPhysicalRigDrive(rig, "elbow",
    new Quaterniond(0.17364817766693033, 0, 0, 0.984807753012208), 0.5);
```

`GetPhysicalRigDrive(rig, joint)` returns the committed target, strength and
immutable gains. An existing entity without a physical rig, or a known
unconfigured joint, returns `null`. A missing entity or an unknown joint on a
physical rig rejects. Reads do not see writes staged earlier in the callback. There are at
most 1,024 drive-read attempts per tick. The SDK requires `physical_rig_drive_v1`
and `physical_rig_v1`, services epoch 7 and the appended 448-byte services
prefix. The existing 424-byte prefix and 96-byte physical command remain
unchanged. Drive-aware `GetPhysicalRig` uses the new summary callback; the old
summary callback rejects a Driven rig rather than misreporting its mode.

Native consumers use the declarations in
[`gameplay_abi.h`](../include/poima/gameplay_abi.h). The appended table slots are:

| Offset in the 448-byte table | Callback | Record contract |
| --- | --- | --- |
| 424 | `physical_rig_summary_v2` | Existing 48-byte summary layout, version 2, modes 0/1/2. |
| 432 | `physical_rig_drive_get` | Version-1 request: 40 bytes; returned state: 168 bytes. |
| 440 | `physical_rig_drive_set` | Version-1 command: 80 bytes. |

These callbacks run during gameplay Tick and use its context and error channel.
Initialize exact version/size headers and zero reserved fields. Joint names are
borrowed ASCII spans of 1–64 bytes for the duration of the call; staged writes
copy their data. The read state returns the requested identity, presence flag,
fixed-capacity name, target, strength and gains. An absent result has no payload
after its identity. Feature negotiation, exact record validation and the older
prefix's semantics remain separate checks.

## Modes, strength and sleep

| Mode | Pose ownership and drive state |
| --- | --- |
| `animated` | Animation owns the pose; drive writes reject. |
| `passive` | Native physics owns the pose; all configured strengths are zero. |
| `driven` | Native physics owns the pose; at least one strength is positive. |

Strength is finite in `[0,1]`. It scales stiffness, damping and the active torque
cap. Zero contributes exactly no active torque and retains the target for later
use. Disabling the last positive drive returns to Passive. It does not restore
animation ownership or re-enroll a suppressed character controller.

A changed control wakes sleeping links once if either its old or new strength
is positive. Identical committed controls do not wake the rig. Changing only a
zero-strength target also preserves sleep. The per-tick force path does not wake
a completely sleeping rig solely to apply internal torques; if any link is
active, sleeping links wake before paired forces are applied. Active links'
sleep timers are not reset every tick by this mechanism.

## Inspection and control math

`runtime.physical_rig` reports the three modes. With `include_joints: true`,
each configured joint adds a `drive` object containing its target, strength,
gains, `evaluated_child_torque`, `angular_error` and `saturation_scale`.
These diagnostics recompute the response from the **currently observed** poses
and velocities at the fixed `1/60` second timestep. They are not a record of
the preceding step's applied torque or proof that a target was reached.

The pure [drive evaluator](../src/physical_drive.cpp) uses a locally implicit
orientation hold. Let `h` be the timestep, `s` the strength, `K` and `D` the
authored gains, `A` the sum of both world inverse-inertia tensors, and `omega`
the child's angular velocity minus the parent's. With `k = s*K`, `d = s*D`:

```text
q_current = inverse(q_parent_joint) * q_child_joint
q_error   = q_target * inverse(q_current)
e         = rotate(q_parent_joint, 2*q_error.w*q_error.xyz)
[I + h*(d + h*k)*A] * torque = k*e - (d + h*k)*omega
```

Hinges use the calibrated parent-joint X projection and its scalar effective
inverse inertia. A common nonnegative scale caps each joint's active torque
norm to `s*max_torque`; the child receives that world torque and the parent its
negative. Torque is in N·m, with no extra timestep factor before native force
application. The corresponding per-joint active angular-impulse bound is
`h*s*max_torque`.

The solve freezes inertia and linearizes orientation for one step. It is not a
coupled articulated solver or a guarantee of stability across arbitrary masses,
contacts, gain choices or timesteps. The sign-invariant chordal error vanishes
at exactly 180 degrees; it does not guarantee global convergence. Inverse
inertias must be finite, symmetric and positive semidefinite; unresolved inputs
reject without hidden conditioning. Existing native body velocity caps still
apply. The drive does not enable native constraint motors or repurpose their
friction warm starts.

## Troubleshooting

| Observation | What to inspect |
| --- | --- |
| A write rejects before motion changes | Confirm the authored joint opted into a drive, then inspect its name, target frame, hard limits and strength. Ensure explicit activation precedes the write. |
| Caller and gameplay changes reject together | Submit one drive write per rig/joint per tick. Choose which caller owns that change; the engine does not silently choose a winner. |
| A target is not reached | Inspect saturation, passive coefficients, contact constraints and actual joint orientation. A capped drive can lack sufficient torque, and the solve has no global convergence guarantee. |
| The character holds a pose but falls over | Joint torques do not supply a support strategy or a balancing root force. Balance and stepping need a separate controller. |
| Setting strength to zero does not restart animation | Zero releases the active torque while physics keeps the skeleton. Recovery to animation is not implemented by this control. |
| Compiled controls report an unavailable service | Use a matching SDK/bridge and declare `IPhysicalRigDriveGame`; an older 424-byte host cannot provide the appended callbacks. |

## Saves and qualification

Definitions containing a drive profile use physical save extension **5**, even
when every drive is disabled or the profile exists only in an unspawned
template. Each live rig retains the complete ordered set of its configured
joint names, saved targets and strengths. Frozen content still supplies the
profile gains. Restore checks target limits, inventory/order, strength, mode and
physical enrollment before publishing state; it retains admitted saved
numerical values without re-canonicalizing the target. Numeric JSON fields
follow existing save semantics; equivalent integer/float spellings are not a
separate preservation contract.

Definitions without drive profiles keep extensions 1–4 and omit the new drive
membership field. Extension 5 retains the applicable controller, procedural and
grounding witnesses; its version does not imply that those optional features
are present. The existing [save prerequisites](GAMEPLAY_SAVES.md) still apply.
This adds no cross-platform future bit-identity guarantee.

The [checkpoint qualification](evidence/m2-physical-joint-drives.json) records
native Linux and Windows checks, managed ABI checks and actual CoreCLR gameplay.
The source tests are
[authoring admission](../tests/physical_drive_authoring.cpp),
[independent drive math](../tests/physical_drive_math.cpp) and
[runtime controls, contacts and saves](../tests/runtime_physical_drive_tests.cpp).
The runtime suite checks complete generated archive roundtrips and continuation,
zero-strength passive equality, isolated target/impulse counterfactuals, sleeping
controls, rollback and dormant-template lifecycle. Those fixtures do not establish
imported-character quality, balancing, recovery or representative performance.


On each platform the runtime fixture passes 18,245 checks and 85 complete
archive pairs; the independent math and authoring fixtures pass 5,107 and 107
checks respectively. The compiled C# workflow retains all 42 fields through
338 RPC calls, 26 saves and thirteen complete byte/typed-state comparisons
across three naturally closed engine processes per platform. Its same-image
reload preserves values with two declared revision/checksum changes, rather
than claiming complete archive equality across reload. Original failed attempts
and subsequent corrections are recorded separately in the qualification record.

The unchanged previously compiled NativeAOT character game also passes on the
new Windows host, including ten complete saves and six whole-state pairs.
That is compatibility evidence for its original 424-byte interface. Publishing
and executing a NativeAOT game that requests the new drives remains separate.

For a [configured simulation build](BUILD.md), build and run the focused suite:

```sh
cmake --build build/linux-game --parallel 2 --target \
  poima-physical-drive-authoring-test poima-physical-drive-math-test \
  poima-runtime-physical-drive-test
ctest --test-dir build/linux-game --output-on-failure -R '^(physical_drive_authoring|physical_drive_math|runtime_physical_drive)$'
```

The [compiled fixture](../tests/managed_physical_drive_gameplay/README.md)
describes its required graph and build. Its actual native consumer is
`tests/physical_drive_gameplay.py`: supply the CLI as the positional argument,
then `--hostfxr`, `--bridge`, `--assembly` and a fresh `--output` directory.
Run it inside a process-tree supervisor when collecting qualification evidence.
It emits original requests, responses, saves and an `evidence.json` result;
exit zero alone does not replace checking that result and process closure.
