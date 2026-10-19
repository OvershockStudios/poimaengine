# Automatic foot contacts

An optional `AnimationRig.ik[].grounding` profile adapts an animated foot to
physics surfaces. It runs after the final physics update of each 60 Hz tick,
using the blended, root-stripped pose before IK. Both feet query that same
uncorrected pose before either leg is solved. Sampling jobs join before the
owner queries Jolt; workers never query the physics world.

This is terrain conformance. Planted-foot locking, gait/contact annotation,
pelvis compensation, dynamic-body support and full-body balance remain future
work. A ray and one calibrated sole point cannot guarantee that an entire shoe
fits stairs, narrow ledges or irregular geometry.

## Author a foot

Add `grounding` to an otherwise complete [two-bone chain](TWO_BONE_IK.md):

```json
{
  "slot": 1, "root": 2, "middle": 3, "end": 4,
  "target": [0, 0, 0], "pole": [0, 1, 1], "space": "rig",
  "enabled": true, "weight": 1,
  "grounding": {
    "sole_offset": [0, -0.1, 0.05],
    "sole_up": [0, 1, 0],
    "sole_forward": [0, 0, 1],
    "reference_height": 0,
    "plant_height": 0.03,
    "lift_height": 0.15,
    "probe_above": 0.3,
    "probe_below": 0.6,
    "max_correction": 0.25,
    "max_slope_degrees": 55
  }
}
```

These illustrative indices and frame values must be calibrated to the imported
character; they are not an anatomical mapping. `target` is retained as a
required chain field but automatic placement replaces it. `pole` still selects
the knee's bend plane, using the chain's authored `space`. Automatic ownership
is frozen for the runtime session. Caller and C# writes to that slot reject;
other manual slots keep their existing behavior. Do not supply an authored
`end_orientation` on an automatic slot. Restart to adopt changed configuration.

`sole_offset` is a point in the end joint's local coordinates. `sole_up` is its
local surface normal, and `sole_forward` defines its heading. The axes must be
unit and orthogonal within `1e-6`. Offsets and `reference_height` are bounded by
±1,000 model units. All values must be finite; booleans are not numbers.
`sole_offset`, both axes and `reference_height` are required.

The prepared sole's model-space Y minus `reference_height` determines contact
weight. At or below `plant_height`, the full chain weight applies; at or above
`lift_height`, the source animation remains untouched and no ray is cast.
Between them, smoothstep fades both positional and orientation correction.
The bounds are `0 <= plant_height < lift_height <= 10` model units. This explicit
height policy preserves ordinary swing lift but is not a general gait detector.
Clips that roll, climb or use a different ground reference need suitable
configuration or a separate animation/rig policy.

The wrapper must remain upright, with a positive uniform orthogonal transform
within `1e-8`; yaw, translation and uniform scale are supported. Model ancestors
still obey their selected [IK metric profile](IK_METRIC_PROFILES.md). Source
scales, translations, geometry and animation curves are preserved. This version
does not implement general affine ground adaptation.

## Probe and solve

An eligible foot casts one world-Y downward ray from `probe_above` metres above
its prepared sole, extending `probe_above + probe_below` metres. Each probe
extent is `0.001–10` metres. At most two automatic feet are admitted per rig and
64 per world, including disabled feet. There are no adaptive recasts.

Supports are static and kinematic colliders, including explicit triangle-mesh
colliders. Dynamic bodies and character capsules are excluded before closest-hit
selection. The nearest physical ancestor owns the character's excluded subtree;
without such an ancestor, the rig's own subtree is excluded. Organizational
ancestors do not exclude unrelated ground. Other raycast behavior, including
exact tie ordering and triangle IDs, follows [physics queries](PHYSICS_INTERACTIONS.md).

A hit must provide an upward-facing normal within `max_slope_degrees` (`0–80`).
Placement projects the original heading onto its tangent plane, or rejects a
degenerate heading. It derives an ankle target from the calibrated sole offset,
retaining the source affine offset rather than replacing imported scales.
Orientation maps the actual affine sole frame relative to the logical joint
rotation, so positive nonuniform end-joint scale does not silently tilt the
requested normal. This does not extend the ancestor metric profile to general
affine IK.
Corrections above `max_correction` (`0–10` metres, exclusive zero) and targets
outside the two links' reach fall back to the original pose. No pelvis movement
or forced limb stretching hides an unreachable target. Partially weighted
contacts need not reach the surface exactly.

Disabled, raised and rejected feet do not retain an old correction. Statuses
are `uninitialized`, `disabled`, `lifted`, `miss`, `no-normal`, `slope`,
`heading-degenerate`, `correction-limit`, `unreachable` and `contact`.
Rejected hits with valid normals remain inspectable; misses and missing normals
have no retained support. Invalid definitions, unsupported wrappers and solver
metric failures reject the complete simulation batch rather than masquerading
as a missed surface.

## Inspect and persist

Request `runtime.ik` with `include_grounding: true` to add `state.grounding`.
Manual slots return null; omitting the flag preserves the earlier result shape.
The cached record includes the decision tick, cumulative committed cast count, whether
that decision cast, status, effective weight, prepared sole, ray origin, support
entity/triangle/point/normal and achieved sole/normal with residuals. Normal error
is in radians. The achieved normal uses the actual affine inverse transpose;
a logical quaternion error alone is not surface-alignment evidence.

Initialization leaves automatic controls disabled and status `uninitialized`.
The first completed physics tick establishes contacts. Preliminary animation
samples reuse the last committed effective controls; only the final tick phase
refreshes contacts. Inspection, skin queries, serialization and warm/cold save
restoration never cast new foot rays. Existing physics restoration has its own
support rebuilding; this is not a promise that it executes no physics queries.

Grounded sessions use nested animation-state version 7. It retains the frozen
profiles, generated controls and complete contact diagnostics/counters. Sessions
without grounding retain their existing animation-state versions and shapes.
Initialized saved decisions must belong to the saved tick. A restore verifies
their achieved geometry against the reconstructed pose without rewriting the
saved numbers; the next tick queries the restored physical world. Direct native
animation sampling at a later historical tick must refresh its final decisions
before exporting a current grounded save. Checkpoint rollback covers histories, controls,
diagnostics and counters together with physics and gameplay state. Failed
batches may have performed read-only probes before rollback; their discarded
queries do not remain in the committed counter.

## Verification

Build `poima-runtime-grounding-test` with simulation enabled and run:

```sh
ctest --test-dir build/linux-game -R '^runtime_grounding_native$' --output-on-failure
```

`tests/runtime_ground_contacts.py` additionally accepts `--binary`, `--source`
(the unchanged licensed Knight GLB), `--license`, and a fresh `--output` directory.
It checks original geometry/curves, complete 57-node affine observations, moving
and sloped physics support, source swing lift, cached inspection, and complete
warm/cold saves. Run it under an external process-tree owner. This CPU fixture
cannot qualify rendered contact quality, hardware input or production crowds.

The [0.0.198 qualification](evidence/m2-ground-contacts.json) records the selected
native checks and two automatic-contact consumers, plus a retained manual
CoreCLR regression across all 76 original takes. The automatic cases cover
Idle/Running_A; they do not establish contact quality for every take.

Design references include Daniel Holden's July 2026
[IK and foot-locking discussion](https://theorangeduck.com/page/inverse-kinematics-foot-locking),
SideFX's [foot-planting workflow](https://www.sidefx.com/docs/houdini/crowds/footplanting),
and Khronos's [terrain-placement tutorial](https://github.khronos.org/Vulkan-Site/tutorial/latest/Advanced_glTF/Procedural_Animation_IK/04_foot_placement.html).
These are practitioner documentation, not claims of peer-reviewed validation;
Poima's implementation is original.
