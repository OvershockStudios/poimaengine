# FBX models and separate animation clips

`asset.import` accepts `.fbx` sources through the bundled, pinned MIT-licensed
ufbx parser. Import, inspection and animation sampling work through the same
headless service as glTF. Cooked models use the existing content-addressed
packages and editable hierarchy/rig instances; no source application is needed
at runtime.

## Import a character

Open `poima world build/character.world.json`, then send:

```json
{"jsonrpc":"2.0","id":1,"method":"asset.import","params":{"source":"../characters/character.fbx"}}
```

Paths are absolute or relative to the world document. The result reports the
asset hash, cooked bytes, geometry/hierarchy/skin/animation counts and import
diagnostics. Import does not change the authored revision. Use
[`asset.inspect` and `asset.instantiate`](ASSETS.md) to inspect and create an
editable instance, then the [runtime animation API](RUNTIME_ANIMATION.md) for
playback.

## Compose separate clips

Add up to 32 animation sources to the base import:

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "asset.import",
  "params": {
    "source": "../characters/character.fbx",
    "animations": [
      {"source": "../characters/idle.fbx", "clip": 0, "name": "Idle"},
      {"source": "../characters/walk.fbx", "clip": 0, "name": "Walk"}
    ]
  }
}
```

A string path imports all takes from that source. An object can select a
zero-based `clip` and rename the selected take. A name override without `clip`
requires a single-take source. Base and appended clip names must be distinct;
explicit names handle exports whose takes all have the same default name.

Composition matches unambiguous node-name paths and normalized local rest
frames, including required joint ancestors. Equivalent meter/centimeter and
axis exports are normalized before comparison. Different bone bases,
proportions, hierarchy or missing joints require retargeting and reject; this
operation does not perform retargeting. Diagnostics report node mappings and
take selections. Geometryless animation donors are supported in this operation;
standalone animation-only packages are not yet supported.

Donor geometry and materials are validated even though only their matched
animation channels are appended. Unavailable donor textures can therefore
prevent import. The base and donor data remain immutable, and a rejected import
does not publish a partial model or change authored state.

## Supported import profile

- ASCII and binary FBX; triangulated static or linear/rigid-skinned geometry.
- Right-handed Y-up coordinates with geometry and local translations in meters.
  Geometry/inheritance helper nodes preserve supported transforms; shear,
  mirrored and zero scale reject.
- At most four positive joint influences per vertex and 256 joints per skin.
  Additional influences and dual-quaternion skinning reject rather than being
  silently truncated or converted.
- Opaque metallic/roughness materials from the supported shader models and
  PNG/JPEG textures embedded in the FBX or contained below its source directory.
  Lambert/Phong materials receive an explicit matte-conversion diagnostic.
- One UV set; generated normals when absent, normalized normals and generated
  tangents. FBX UVs convert to the image convention before tangent generation.
  On a nondegenerate triangle with exactly collapsed UVs and no normal map,
  an invalid generated tangent receives a deterministic orthogonal frame and
  an explicit repaired-corner count. Valid generated frames and geometry remain
  intact. This frame supplies no normal-map fidelity: repair the source UVs
  before adding a normal map. Mapped invalid frames still reject.
- TRS animation takes baked to linear channels with 60 Hz nonlinear resampling
  and retained dense source keys. Take starts normalize to zero. Discontinuous
  and non-transform animation properties reject.

The normal-map convention defaults to `opengl`. Set `fbx_normal_map` to
`directx` for maps with the opposite green-channel convention. This option is
for an FBX base source and applies to FBX donors in the same import; glTF keeps
its own convention. The chosen profile is part of cooked identity.

Transparent/advanced materials, layered/procedural textures, texture transforms,
multiple UV sets, nonwhite or multiple vertex-color layers, morph/cache
deformation, subdivision, NURBS and constraints are outside this profile. Cameras and lights retain hierarchy
nodes with a diagnostic but do not become engine camera/light components.
Absolute or escaping texture references reject; automatic basename search or
external path remapping is not provided.

A single vertex-color layer with finite, exactly white RGBA values is an
appearance identity and can be omitted with a diagnostic. Other colors and
alpha changes reject. Invalid attribute indices abort loading; selected parser
warnings for repaired/truncated arrays and duplicate object IDs also reject.

Selected top-level inputs total at most 128 MiB. Each parser separately bounds
its allocations and dependency reads; combined retained imported models are
bounded to 128 MiB before composition. Cooked packages retain the 64 MiB model
limit. These are input/data limits, not a total process-memory guarantee.

## Qualification boundaries

The original analytic fixtures cover both encodings, metric/axis conversion,
geometry transforms, skin bind cancellation, multiple takes, normal-map/UV
orientation, separate clips, cooked reopening and unsupported-input rejection.
Specific real-asset checks are separate evidence. Neither the parser's format
coverage nor a synthetic fixture establishes compatibility with every DCC
exporter or a general character retargeting workflow.

[Checkpoint evidence](evidence/m2-fbx-import.json) records the native Windows/Linux
checks and original-fixture Windows Vulkan captures with their source hashes.

## Check and recover

Inspect the returned diagnostics, then page the `nodes`, `skins`, `primitives`
and `animations` sections of `asset.inspect`. Confirm the expected hierarchy,
joint counts, take names, durations and material maps before instantiation.
Use `asset.animation.sample` to inspect representative poses; a Vulkan build can
also request an isolated pose capture. Check rest, intermediate and end poses,
especially feet, hands, bind frames and normal-map orientation.

Import failures return `-32050` with a bounded diagnostic. Correct the source or
unsupported feature and retry; import does not edit the world. An existing model
staging entry is rejected without truncation. Remove only a verified abandoned
regular staging file; do not follow or overwrite links. Successful imports can
be retried to recover the same cooked identity.

Instantiating is a separate revision-guarded edit. On a stale revision, inspect
the current world and reconcile the proposed edit before submitting a new
transaction. If the result of a sent edit is unknown, retain its original
receipt and payload and follow [client recovery](PYTHON_CLIENT.md#failure-and-deliberate-recovery).
An import diagnostic or pose preview does not establish that a gameplay
controller, collision body or retargeted rig has been created.

## Reproduce the original fixtures

After a [headless build](BUILD.md), run the registered import tests:

```sh
ctest --test-dir build/headless -R '^fbx_' --output-on-failure
```

The fixtures are original analytic geometry and takes, generated locally in
both ASCII and binary encodings. The checks cover independent pose/material
expectations, composition, rejected inputs and owner recovery. They do not
download character art.

For the Windows Vulkan path from WSL, use a fresh output directory:

```sh
python3 tests/fbx_capture.py build/windows-runtime/poima.exe \
  --windows-interop --gpu 0 --output build/fbx-capture-example
```

This compares GPU skinning, CPU deformation and separately authored expected
geometry after source deletion, then advances an instantiated rig by 30 ticks.
It records still captures and native state; it does not execute C# gameplay or
qualify frame-rate performance.
