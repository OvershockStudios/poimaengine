# Reference-pose bone-frame conversion

Separate exports can describe the same joint locations using different bone
coordinate axes. `asset.import` offers an explicit `reference-frame-v1` policy
for that case. It converts selected animation curves into the target rig's
frames while retaining the target model's hierarchy, geometry, rest transforms
and inverse binds.

The default remains [exact-skeleton composition](FBX_IMPORT.md#compose-separate-clips).
The optional policy needs matching named ancestry and coincident reference
joint origins. Different proportions, missing bones and different reference
stances require a broader retargeting workflow. This operation does not create
IK constraints or remove root motion.

## Select the references and motion

Use a selection object for each donor that needs conversion:

```json
{
  "source": "../characters/body.fbx",
  "animations": [{
    "source": "../characters/motions.fbx",
    "clip": 1,
    "name": "Walk",
    "frame_transfer": {
      "policy": "reference-frame-v1",
      "source_pose": {"kind": "sample", "clip": 0, "time": 0},
      "target_pose": {"kind": "rest"}
    }
  }]
}
```

Send this object as the parameters of `asset.import`. Paths resolve relative to
the open world document. Supply your actual files and indices: this example
assumes donor take 0 is a deliberately chosen reference and take 1 is motion.
A take's name or position in the file does not establish that it is a suitable
reference.

Both reference selectors are required:

| Selector | Meaning |
| --- | --- |
| `{"kind":"rest"}` | Use the imported node defaults. No `clip` or `time` fields are permitted. |
| `{"kind":"sample","clip":0,"time":0}` | Sample one original take at a finite time, with imported defaults for properties absent from its channels. |

Source references use the **original unfiltered donor**; target references use
the original base model. Their indices are independent of the donor's selected
motion and renamed output. Indices must be 0–255 and identify an existing take.
Time must be within 0–3600 seconds and within the reference take's duration.
Reference sampling neither loops nor clamps an out-of-range time.

Imported defaults, skin bind frames and sampled reference poses are distinct.
The importer does not infer a bind/reference pose from frame zero or a take
named “Targeting Pose.” An explicit reference does not rewrite the target's
rest transforms or inverse binds.

## Optional component-space alignment

The default alignment is identity. If the two exports use different overall
orientations or offsets, declare a single rigid transform inside
`frame_transfer`:

```json
"alignment": {
  "position": [0, 0, 0],
  "rotation": [0, 0, 0, 1]
}
```

Position is XYZ in normalized metres, within ±1e9. Rotation is a normalized
XYZW quaternion with components in [-1,1] and squared-length residual at most
1e-6. Both arrays are required when alignment is supplied. The transform maps
donor component space into target component space; it is not a per-bone
position correction or a body-height scale.

## Supported conversion

Let `Gs_ref(i)` and `Gt_ref(i)` be a mapped node's global reference transforms,
and `H` the declared alignment. Poima uses column-vector transforms and forms:

```text
C(i) = inverse(H * Gs_ref(i)) * Gt_ref(i)
Gt(i,t) = H * Gs(i,t) * C(i)
```

Each `C(i)` must represent a proper rotation at the same joint origin, within
the policy's numerical checks. Parent and child bases both affect the local
curve: for a nonroot node, the local expression is
`inverse(C(parent)) * source_local(i,t) * C(i)`. The root expression also
includes `H`. Applying a correction to only the child's quaternion would miss
the parent frame and translation changes.

The bounded profile requires exactly equal XYZ scale components on mapped
nodes in imported defaults and references, and in scale keys and cubic scale
tangents. Nearly uniform values are not treated as equal. Nonuniform scale can create shear when axes
change, so it rejects rather than being projected onto an approximate TRS.
Euclidean reference global-origin differences must be at most 1e-5 metres, and frame
rigidity/unit-scale residuals at most 1e-6. These are admission tolerances;
they do not establish a universal error bound for arbitrary large transforms
or subsequent animated scaling.

Translation and rotation keys are expressed in the new frames while retaining
their times, interpolation and duration. Cubic tangents receive the linear
coordinate change; translation offsets apply only to values. Quaternion values
and tangents receive the same linear map, preserving authored near-unit key
norms. The converter does not normalize either curve component; doing so would
change the motion. Evaluated orientations and synthesized constant default
orientations are normalized. Constant channels may be added where transformed
source defaults differ from target defaults and a selected clip has no channel
for that property. A sampled reference is not substituted for the original
source defaults during motion playback.

The existing model budgets still apply, including 8,192 total channels,
1,000,000 total keys and the import's retained-data limit. Added constant
channels count toward these budgets. Unsupported source geometry, materials or
deformation can reject before conversion, even when only donor curves are used.

## Inspect and verify

Discover the current `asset.import` schema before using the policy. The import
result's diagnostic strings identify `reference-frame-v1`, both original
reference selections, normalized parsed-model fingerprints and conversion
counts/residuals. Those fingerprints identify the parsed model content; they
are not raw source-file hashes.

Page the cooked nodes, skins and animations through `asset.inspect`. Check the
selected name, duration, required mapping and retained target skin. Use
`asset.animation.sample` at reference, key and between-key times to inspect
joint transforms and deformed vertices. Visual observation remains necessary
for judging the actual character, material and source animation quality.

Converted root/hip translation stays in the visual animation. The importer
does not move a character controller or extract/consume a root-motion delta.
For controller-driven locomotion, choose an authored in-place clip and verify
it against actual actor movement. Conversion does not promise matched stride,
planted feet or contact-aware animation.

Malformed policy parameters return `-32602`. Invalid reference selections or
unsupported frame/scale/topology conversions return `-32050` with a bounded
diagnostic. Correct the named incompatibility before retrying. Failed imports
leave authored state and published model packages unchanged. Without the
optional policy, [structured exact-skeleton failures](FBX_IMPORT.md#inspect-a-composition-failure)
retain their existing `animation_composition` payload.

Instantiating the cooked result is a separate guarded edit. Follow the
[asset workflow](ASSETS.md) and [compiled character example](../examples/character-yard/README.md)
for ownership, playback and checkpoint continuation. The supplied character
example's stationary arm-opening source motion does not become a walk cycle
by enabling this policy.

## Reproduce the contract checks

These checks are qualified for 0.0.73 on Linux runtime, native Windows runtime
and simulation-disabled Linux authoring. Run from the repository root with
Python 3.10+ and a native authoring build.
The fixtures are original analytic FBX files created by the test; no downloads,
external character library, renderer or .NET installation are required.

```sh
cmake --preset headless
cmake --build --preset headless --target poima poima-animation-frame-transfer-test
build/headless/poima-animation-frame-transfer-test
python3 tests/animation_frame_transfer.py \
  --binary build/headless/poima --output build/frame-transfer-example
```

The output directory must be new. The native executable reports eight passed
check groups. The protocol verifier must exit with code 0 and record
`passed: true`, five tests, unchanged fixture hashes and clean owned-process
exits in `build/frame-transfer-example/evidence.json`. It checks independently
defined joint matrices and original skin weights, explicit references resolved
before clip filtering, rejected conversions and fresh-owner persistence.

Optional Windows Vulkan consumer checks use a matching renderer/simulation
build and a supported GPU. From WSL:

```sh
python3 tests/animation_frame_transfer_capture.py \
  --binary build/windows-runtime/poima.exe --windows-interop --gpu 0 \
  --output build/frame-transfer-capture-example
```

That destination must also be new. Success requires exit code 0 and
`passed: true` in its `evidence.json`. Inspect the retained images as well as
the report: GPU/CPU preview, independently derived geometry/normals and ordinary
runtime playback must agree. The verifier removes only the sources it created
before reopening cooked content. It does not delete caller assets. A hardware
readback is separate from an editor screenshot, physical input test or
game-scale performance measurement.
