# Imported IK scale profiles

An imported rig can contain small differences between its three scale axes.
Poima's default two-bone IK profile requires exact positive uniform scales.
Choose the explicit `near-uniform-v1` profile for a rig whose small deviations
fit the fixed bounds below. The engine preserves its original imported and
animated scales; it validates the actual composed poses instead of replacing
those values.

This profile supports bounded approximation around a similarity transform.
It does not provide general affine IK, detect the floor, choose anatomical
chains or lock feet during locomotion.

## Configure a chain

Use **0.0.197 or later / schema revision 109**. Add `metric` to the selected
chain in the complete existing `AnimationRig` component:

```json
{
  "slot": 1,
  "root": 25, "middle": 24, "end": 23,
  "target": [0.1, 0.1, 0], "pole": [0, 0, 1],
  "enabled": true, "space": "rig", "weight": 1,
  "metric": "near-uniform-v1"
}
```

This is a chain fragment with caller-chosen goals. Keep the rig's asset,
playback and other chain/layer fields when replacing its component through a
guarded transaction. Inspect the original model to select its actual indices
and suitable targets. The pinned [Knight library](CHARACTER_LIBRARY.md) uses
left leg `25 → 24 → 23` and right leg `29 → 28 → 27`; other sources have their
own topology and coordinate frames.

Omitting `metric`, or explicitly selecting `"uniform"`, uses the existing
strict profile. Unknown strings and non-string values reject. The profile is
frozen content configuration: `runtime.step` IK commands cannot replace it.
Point targets, poles, weights and optional end orientation remain independently
controlled through the [existing IK API](TWO_BONE_IK.md).

## Fixed bounds and units

The near-uniform profile checks every model ancestor through the middle joint.
All local scale axes must be finite and positive. For each relevant node:

- Local scale error is `max(axis_scale) / min(axis_scale) - 1`.
- Let `A` be its composed model-space linear transform and `R` the rotation
  obtained by multiplying the local quaternions through its ancestry.
  The fitted positive scalar is `s = trace(Rᵀ A) / 3`; define `D = A / s`.
- Frame error is the largest absolute entry of `D - R`.
- Gram error is the largest absolute entry of `Dᵀ D - I`.

Each error must be at most **0.0001**. These dimensionless bounds apply to the
incoming pose and the final pose after IK and positional weighting. A rig
whose scales happen to cancel at rest can fail after a joint rotates. Rest
admission checks the profile even when a chain is disabled; each active solve
checks its current evaluated pose, including animation blends and layers.

The full positional solve also checks its actual affine endpoint before
applying the positional blend weight. If the incoming link lengths are `a`
and `b`, its model-space distance from the analytically clamped endpoint must
be at most:

```text
max(0.000000001, 0.0001 × (a + b))
```

This limit uses model coordinates. It does not promise a fixed world-space
distance under an arbitrarily scaled wrapper. The final `target_error` still
measures the world-space distance to the original requested goal. It can be
larger because the target is unreachable or the positional weight is below
one. Inspect `clamped`, the final endpoint and `target_error` for gameplay
acceptance.

The solver changes joint rotations under the existing ownership rules.
Translations, local scales, original curves, geometry and inverse binds remain
intact. End-orientation diagnostics retain their logical rig quaternion frame;
they do not represent an exact world-space sole normal under affine distortion.

## Inspect cached measurements

Add `include_metric:true` to a guarded `runtime.ik` request:

```json
{
  "session_id": "<current session>",
  "tick": 60,
  "entity": "<rig entity>",
  "slot": 1,
  "include_metric": true
}
```

Substitute the actual session, entity and committed tick. The response adds
`state.metric`; the default response shape stays unchanged. The value is null
for the uniform profile. A near-uniform chain reports:

| Field | Meaning |
| --- | --- |
| `profile` | `near-uniform-v1`. |
| `evaluated` | Whether the active solve measured this pose. |
| `relative_limit` | The fixed `0.0001` bound. |
| `local_scale_error` | Maximum local scale error across the checked input and final poses. |
| `frame_error` | Maximum cumulative frame error across those poses. |
| `gram_error` | Maximum cumulative Gram error across those poses. |
| `unweighted_position_error` | Full positional solve's model-space endpoint residual before blending, when performed. |
| `unweighted_position_limit` | Its model-space residual budget. |

Disabled chains and zero-weight chains without an active orientation goal
report `evaluated:false` with null errors. An orientation-only solve measures
the scale/frame bounds but has null positional residual fields. A missing
configured slot still returns `state:null`. Inspection reads cached state;
it neither samples animation nor advances simulation. The flag is a strict
boolean and can be combined with `include_end_orientation`.

## Save, failure and recovery

The metric profile stays in the complete authored document carried by saves.
It introduces no new dynamic animation-state version or gameplay ABI layout.
Existing point-only and end-orientation control blobs retain their versioned
shapes. Pose diagnostics are derived again when the saved state is restored.

A failed evaluated metric or endpoint bound rejects the entire simulation
batch, including earlier ticks in that batch. The engine preserves committed
physics, animation, gameplay and save intent state through rollback. Inspect
the original error and the source pose; choose an appropriate rig/profile or
change the intended goal deliberately. There is no automatic scale rewriting
or silent fallback to a different solver.

Only use this profile after inspecting the actual source. A finite sample of
one take does not establish that every layer, transition, authored override
or future animation remains within its bounds.

## Qualification

The 0.0.197 CLI and CoreCLR consumers each pass 86 finite poses across all
76 original Knight takes. Each pose observes all 57 nodes for independent
affine recomposition and metric checks. Original scales and translations remain
exact; complete saved-state pairs cover rollback, warm restoration and cold
continuation. These are controlled pose checks, rather than a gait or contact
controller. [Results and separate regression boundaries](evidence/m2-ik-metric-profile.json).

## Reproduce the imported-character fixture

[`tests/runtime_ik_metric_profile.py`](../tests/runtime_ik_metric_profile.py)
builds nothing. Supply the unchanged original Knight GLB and its original CC0
license described in [Character Library](CHARACTER_LIBRARY.md), a matching
simulation-enabled CLI, and a fresh output directory whose parent exists:

```sh
python3 -B tests/runtime_ik_metric_profile.py \
  --binary /path/to/poima \
  --source /path/to/Knight.glb \
  --license /path/to/LICENSE.txt \
  --output build/ik-metric-knight-1
```

Run within an external process-tree owner. The fixture bounds admission to
180 seconds, 700 RPCs and 128 MiB of retained output; its individual native
process EOF checks do not establish descendant closure. It retains original
logical JSON requests/responses, authored boundaries, complete save payloads and a
final `evidence.json`, including failures. Choose a new output for another run.

The source checks cover the complete original geometry and all animation
curves against the actual cooked package. Runtime checks select one midpoint
in each of the 76 takes and additional Idle/Running_A boundaries. They observe
all 57 nodes, independently recompose affine transforms, compare cached input
and final metric maxima, and inspect both leg endpoints and logical foot
orientations. Full saved-state pairs check malformed/stale requests, exact
retry, warm restore and fresh-process cold continuation. These are fixture
requirements; this recipe alone does not establish a passing qualification.

An optional original C# consumer in
[`tests/managed_ik_metric_gameplay`](../tests/managed_ik_metric_gameplay/README.md)
uses the existing 352-byte oriented-IK extension to read and write both leg
controls. Build it separately with a compatible .NET SDK:

```sh
dotnet build tests/managed_ik_metric_gameplay/Poima.ManagedIkMetricGame.csproj \
  -c Release --artifacts-path build/ik-metric-consumer \
  --disable-build-servers -m:1 -nodeReuse:false \
  -p:UseSharedCompilation=false -p:UseAppHost=false
```

Add all three arguments to the fixture command:

```sh
  --hostfxr /path/to/libhostfxr.so \
  --bridge /path/to/Poima.ManagedBridge.dll \
  --assembly build/ik-metric-consumer/bin/Poima.ManagedIkMetricGame/release/Poima.ManagedIkMetricGame.dll
```

Use a matching qualified bridge/SDK closure. On Windows supply the respective
`hostfxr.dll` and native paths; a WSL Python invocation can use
`--windows-interop`. The compiled mode additionally checks both staged controls,
intentional Tick failure rollback and complete gameplay/save continuation.

The fixture preserves the caller's original GLB and license and changes no
source scales or curves. Its source-derived skinning witnesses are not native
vertex-buffer or GPU observations. The original checkout remains readable.
Neither mode chooses contacts, calibrates a sole, locks feet, establishes gait
quality or qualifies rendering, physical input or performance.
