# Animation asset reference pipeline

The animation asset pipeline provides glTF skin/curve import, immutable packages, numerical inspection and isolated pose capture. [Compute skinning](GPU_SKINNING.md) and [editable runtime rigs](RUNTIME_ANIMATION.md) support fixed-tick playback, crossfades, inertial transitions and masked layers. [Explicit rotation retargeting](ANIMATION_RETARGETING.md) supports matching named ancestry with chosen reference poses and position/scale policies. Root-motion extraction, animation events, IK and automatic anatomical/contact retargeting remain unfinished. [FBX import and separate clip composition](FBX_IMPORT.md) feed the same cooked model and runtime paths, with explicit import limits.

Run the checked-in original ribbon example from the repository root:

```sh
build/headless/poima world build/animation-example.world.json < examples/animation-assets.jsonl
```

It imports [the fixture](../examples/assets/animated-ribbon.glb), inspects its rig and keys, and samples a halfway pose. The script performs no world mutations and works without a graphics device. Windows builds support the same protocol.

## Import and inspect

Use `asset.import` as described in [Assets](ASSETS.md). Animated or weighted models use `poima.model.v4`; ordinary static output remains `poima.static-model.v3`. Import does not alter the world. glTF key times and values survive cooking without fixed-rate resampling or compression; FBX transform takes use the separately documented baked profile. Sources can be removed after import.

`asset.inspect` accepts the additional `skins` and `animations` sections. Skin summaries expose index, name, skeleton node and joint count. Clip summaries expose index, name, duration and channel count. Node records now include `skin` (-1 when unbound); primitive records include `skinned`. All sections retain pagination with `offset` and `limit` (1–64).

The following read-only commands accept the immutable model hash as `asset` and return `items`, `total` and `next_offset` (null at the end). Numeric indices are zero-based; names need not be unique. `world.describe` advertises their parameters for the current build.

| Command | Required parameters beyond `asset` | Optional parameters | Observation |
| --- | --- | --- | --- |
| `asset.animation.channel` | `clip`, `channel` | `offset`, `limit` | Channel target node/path/interpolation and original keys. Each key has time/value; cubic keys additionally have in/out tangents. |
| `asset.animation.skin` | `skin` | `offset`, `limit` | Palette index, joint node and inverse bind matrix. Also reports skin name and skeleton index. |
| `asset.animation.sample` | `time` | `clip`, `loop`, `section`, `node`, `primitive`, `offset`, `limit` | Evaluated node poses, or a page of deformed vertices. |

Channel vectors always contain four components; the fourth translation/scale component is zero. Matrices are column-major. Times are seconds. Samples accept finite nonnegative time up to 1e9 seconds. With a clip selected, time clamps to its duration or wraps when `loop:true`; zero-duration clips remain at time zero. Channels hold their first/last value outside their own key interval. Omitting `clip` samples the rest hierarchy, preserving the requested time in the response.

Node sampling is the default: each item supplies local position/rotation/scale, parent index and evaluated world matrix. `section:"vertices"` requires both a source node index and a cooked primitive index bound to that node. Each result contains mesh-local position, model-world position, mesh-local normal/tangent, UV and cooked joint indices/weights when skinned. `mesh_world` identifies the transform connecting these spaces. Only the requested vertex page is deformed, although model validation and hierarchy evaluation still process the model. This API is a reference observation path, not a production crowd benchmark.

```json
{"jsonrpc":"2.0","id":1,"method":"asset.animation.channel","params":{"asset":"<model hash>","clip":0,"channel":0,"limit":8}}
{"jsonrpc":"2.0","id":2,"method":"asset.animation.sample","params":{"asset":"<model hash>","clip":0,"time":0.5,"loop":true,"limit":16}}
{"jsonrpc":"2.0","id":3,"method":"asset.animation.sample","params":{"asset":"<model hash>","clip":0,"time":0.5,"section":"vertices","node":0,"primitive":0,"limit":8}}
```

Invalid selectors/parameters return `-32602`; invalid packages or unevaluable poses return `-32050`. The sampler rejects a cubic quaternion that evaluates to zero, nonpositive evaluated scale, singular blended skin transforms or out-of-range coordinates. These are reported failures, not silently substituted poses.

## Measure a posed model

With native 0.0.178 / world schema revision 106, `asset.animation.bounds`
measures a cooked model at one CPU-reference pose without opening a player or
changing the authored world. Use it to inspect imported dimensions and placement
before choosing gameplay geometry. This evolving method is outside the frozen
authoring-core and runtime game-session contracts.

```json
{"jsonrpc":"2.0","id":1,"method":"asset.animation.bounds","params":{"asset":"<cooked model hash>","time":0}}
{"jsonrpc":"2.0","id":2,"method":"asset.animation.bounds","params":{"asset":"<cooked model hash>","clip":0,"time":0.5,"loop":true}}
```

Replace the asset placeholder with the actual 64-character lowercase model
hash returned by import. The required parameters are `asset` and `time`.
Optional `clip` is a zero-based integer in `0..255` which must identify an
existing take; `loop` is Boolean and defaults to false. No node, primitive,
section, offset or pagination selector is accepted. Time must be finite and
within `0..1e9` seconds.

The result has exactly these members:

| Member | Meaning |
| --- | --- |
| `asset` | The verified cooked model identity. |
| `clip` | The chosen take index, or null for the authored rest pose. |
| `requested_time` | The supplied time in seconds. |
| `sample_time` | The effective time after the existing clip clamp/wrap policy. |
| `space` | `"model"`: imported model-world coordinates, before an authored/runtime instance transform. |
| `empty` | True when the selected scene has no stored vertices. |
| `bounds` | `{ "min": [x,y,z], "max": [x,y,z] }`, or null when empty. |
| `primitive_instances` | Primitive bindings beneath selected scene roots. Shared primitives count for each binding. |
| `vertex_instances` | Stored cooked vertices summed across those bindings. |

Omitting `clip` samples the rest hierarchy and retains the requested time as
`sample_time`; it does not select the first take. Selected clips clamp to their
duration or wrap when `loop:true`; a zero-duration clip stays at time zero.

Only selected scene roots and their descendants contribute geometry. Rigid
accessories, weighted meshes and repeated mesh bindings all contribute. Every
stored cooked vertex counts, including vertices absent from the triangle index
list. Full model/animation validation still applies before evaluation; geometry
outside the selected scene is not an exemption from model validity.

Bounds are the extrema of the existing reference vertex sampler's model-world
positions. Weighted geometry uses its existing float deformation before the
same double-precision model-world transform. The result is tight for these
reference points at this pose, without the margins used by conservative render
bounds. It is not a swept-animation bound, a GPU-rounding guarantee or a
collision shape.

The full selected workload is counted before any mesh deformation allocation:
at most 10,000 primitive instances and 1,000,000 vertex instances. A shared mesh
can exceed these limits through repeated bindings even when its unique stored
geometry is small. Normal model/package validation and storage limits also
apply; the work caps are not a process-memory guarantee.

Malformed parameters, unknown fields and invalid clip selectors return
`-32602`. Missing/corrupt packages, exceeded instance budgets and unevaluable
poses return `-32050`. Preserve the actual error; the operation does not repair
content, publish a replacement package or retry automatically. Reads and
rejections leave authored state and cooked assets unchanged.

### Verify posed bounds

With Python 3.11+ and a built native Linux headless executable:

```sh
python3 tests/model_bounds_contract.py --binary build/headless/poima
```

The last JSON line identifies the retained evidence path. Native POSIX output defaults to Python's temporary directory, respecting `TMPDIR`;
`--output` must name a fresh directory. The fixture retains original RPC streams,
analytic geometry, complete authored-state/store observations and source-absent
cold reopening. It compares full vertex pages with the new result and checks
exact and over-budget shared instances. Optional `--knight-source` and
`--kenney-source` accept the original files pinned by
[Locomotion Yard](../examples/locomotion-yard/README.md#prerequisites-and-original-content).

Bounds are 180 seconds, 1,000 requests and 128 MiB/4,096 retained files; ten
seconds of the total are reserved for cleanup. Windows-native Python requires
an actual caller-owned process-tree Job. Use the
[supervisor pattern](CHARACTER_LIBRARY.md#run-the-independent-checks) with these
arguments and a 210-second outer limit. CTest registers `model_bounds_native`
and `model_bounds_contract`. The native unit checks six analytic groups; the
protocol test verifies the real service. Neither checks a displayed frame.

## Bind imports to inspected original models

With world schema revision **105**, `asset.import` accepts optional
`expected_model_sha256` on the base request and on each object in `animations`.
Obtain each value from that file's `asset.source.inspect.model_sha256` using the
same FBX normal-map convention. The fingerprint identifies the **complete
normalized parsed model**, before selecting a take, renaming it, frame transfer
or rotation retargeting. It is not the raw file's SHA-256, the selected donor's
identity or the final composed package's asset ID. Do not substitute those
identities even when two happen to coincide for a particular import.

For example, after separately inspecting `body.fbx` and `motion.fbx`, substitute
the returned fingerprints into this request:

```json
{"source":"body.fbx","expected_model_sha256":"<body model_sha256>","animations":[{"source":"motion.fbx","clip":1,"name":"Run","expected_model_sha256":"<motion model_sha256>"}]}
```

Both placeholders must be replaced with the corresponding 64 lowercase
hexadecimal characters. A donor string still appends takes without a new
admission guard; use an object to guard that donor. A donor object can combine
this guard with either its `frame_transfer` or `retarget` policy. The mandatory
`expected_source_model_sha256` and
`expected_target_model_sha256` inside rotation retargeting remain separate
checks; the new outer guard does not replace them.

Omitted guards retain the existing request behavior. Matching guards do not
become cooked metadata or change a successful package's identity. Whole-source
import budgets still apply before take selection. File-size preflight can
inspect donor metadata before the base fingerprint is compared; the API does
not promise that a stale base prevents all donor filesystem access.

A well-formed guard that differs from the parsed model rejects with `-32050`,
message `Model source changed since inspection; inspect it again before importing.`,
and this complete `error.data` shape:

| Member | Value |
| --- | --- |
| `type` | `"model_source_changed"` |
| `version` | integer `1` |
| `source_role` | `"base"` or `"animation_donor"` |
| `donor_index` | `null` for base; integer `0..31` for the original `animations` array index |
| `expected_model_sha256` | The submitted original-model fingerprint |
| `actual_model_sha256` | The newly parsed original-model fingerprint |

This rejection publishes no partial package and leaves the authored world
unchanged. Malformed guard values reject with `-32602` before importer I/O;
other import errors retain their existing behavior. Parsing and format
validation can fail before a meaningful model fingerprint exists. Keep the
original error and data, inspect the source deliberately, then choose whether
to accept a new fingerprint or restore the intended source. The engine does
not repair files, replace the submitted guard or retry the import automatically.

### Verify import guards

With Python 3.11+ and a built Linux headless executable:

```sh
python3 tests/asset_import_guards.py --binary build/headless/poima
```

The final JSON line gives the retained output and evidence paths. Native POSIX
runs default to Python's temporary directory (respecting `TMPDIR`); Windows and
Windows-interop runs keep the project `build/asset-import-guards` default.
`--output` selects a fresh path explicitly. Under WSL, use a native Linux
filesystem for repeated full-tree observations. Copy retained temporary output
before OS cleanup if it must survive a restart. The fixture does not delete it.

The three protocol groups retain original requests/replies, source versions,
complete authored/store snapshots and `evidence.json`. They cover malformed
values, matching guards, changed base/donor models and deliberate recovery.
Add `--baseline <retained-older-native-executable>` for an independent legacy
reply/package comparison; without it, compatibility checks use the current
native executable. Bounds are 180 seconds, 1,600 requests and 128 MiB of retained
output, with ten seconds inside the total deadline reserved for owner cleanup.
The complete output census still runs before every request. Windows-native
Python requires a caller-owned process-tree Job; use the
[supervisor pattern](CHARACTER_LIBRARY.md#run-the-independent-checks) with this
fixture's arguments and a 210-second outer limit.

CTest also registers `model_source_fingerprint_native`,
`model_source_fingerprint_protocol` and `locomotion_source_guards`. The first
checks native helper policy/byte parity; the last uses a fake client to check
launcher request shapes and error retention. The launcher unit checks are not
native execution evidence. `python3 tests/asset_import_guards_harness.py` checks
default paths and cleanup budget with a fake owner. See the [checkpoint evidence](evidence/m2-model-source-guards.json)
for the actual platform cohorts and their different baseline scopes.

## Visual observation

`asset.animation.capture` accepts `asset`, `time`, optional `clip`/`loop`, and the existing `world.capture` parameters: required `revision`, `camera`, `path`, plus optional width, height, GPU, samples, culling and profiling. The authored camera and lighting illuminate an isolated model in its source coordinate system. Other authored meshes are excluded. Only the model's selected scene roots and descendants are drawn. Capture leaves the scene and running simulation unchanged.

The command samples curves on the CPU and defaults to GPU compute deformation (`skinning:"gpu"`). Select `skinning:"cpu"` for reference deformation. Both use the existing Vulkan material, shadow and culling path. The response labels `source:"asset_animation"` and reports asset, clip, effective time and the chosen deformation path in `animation`. See [GPU skinning](GPU_SKINNING.md). This is a still pose preview, not continuous runtime animation. Image output retains the new-path and immutable-store protections of ordinary capture. Renderer absence returns `-32003`; render failures return `-32020`. Capture validation/evaluation failures use the existing scene capture `-32602` path; package reading errors use `-32050`.

```json
{"jsonrpc":"2.0","id":4,"method":"asset.animation.capture","params":{"asset":"<model hash>","clip":0,"time":0.5,"revision":1,"camera":"<camera entity ID>","path":"<new BMP path>","width":640,"height":480,"samples":4}}
```

`asset.instantiate` now creates ordinary editable rig/node entities for models containing skins or clips. Weighted primitive children use `SkinnedMesh`; `StaticMesh` continues to reject weighted primitives. See [runtime animation](RUNTIME_ANIMATION.md) for the authoring components, initial state and fixed-tick controls. Static import and instantiation remain available.

## Supported profile and mathematics

- Local TRS animation: STEP, LINEAR and CUBICSPLINE. Translation/scale interpolate componentwise. Linear rotations use shortest-path quaternion interpolation. Cubic curves use Hermite interpolation with time-scaled endpoint derivatives; cubic rotations are normalized without changing key signs.
- Four joint influences per vertex via JOINTS_0/WEIGHTS_0. Joint attributes accept unsigned byte/short; weights accept float or normalized unsigned byte/short. Import normalizes accepted near-unit sums. Additional influence sets are rejected, not truncated.
- Joint order follows the skin array, independently of node order. Child-before-parent storage is supported. Joints must share a hierarchy root; an optional skeleton node must be their ancestor. A referenced skin's joints must belong to the selected scene. Missing inverse bind matrices become identity.
- Mesh-local palettes use `inverse(mesh_world) * joint_world * inverse_bind`. Rendering applies `mesh_world` afterward, cancelling the skinned mesh node transform as required by glTF. CPU deformation blends matrices, transforms positions and inverse-transpose normals, then orthogonalizes tangents and preserves reflection handedness. It fails for singular blends.
- Flat-normal expansion and MikkTSpace reindexing preserve weights, including distinct influences at otherwise identical vertices. Static vertices remain 48 bytes; skin influences are a separate array.

These rules follow the [Khronos glTF specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#skins) and its [animation interpolation appendix](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#appendix-c-interpolation). The reference sampler preserves source curves for import diagnostics. These asset queries evaluate one take; crossfades, inertial transitions and masked layers use the separate [runtime animation](RUNTIME_ANIMATION.md) interface. Compressed curve storage is not implemented.

Limits: 64 skins, 256 joints per skin, 4096 total joint references, 256 clips, 16,384 total channels, one million total keys and key times in 0–3600 seconds. Inverse-bind accessors may contain extra elements but are limited to 4096 matrices; only the referenced joint prefix is retained. Node/geometry/package limits from [Assets](ASSETS.md) also apply. Preview limits are 10,000 drawable instances, one million deformed vertices and three million deformed indices in aggregate. They are bounded bootstrap limits, not shipping content targets.

The channel limit applies across all takes, including before donor take selection.
Version 0.0.174 increases that limit from 8,192 without changing the cooked v4
layout or the independent key, joint, metadata and package budgets. Older readers
still reject packages exceeding their 8,192-channel profile. Import and compiled
sampling can hold separate curve copies; these limits are not a process-memory
guarantee.

Morph targets, mirrored/nonpositive node scales and the broader excluded glTF profile remain unsupported. Static matrix-authored nodes use the [documented decomposition policy](ASSETS.md#matrix-authored-transforms); animation-targeted nodes must supply TRS. Inverse bind matrices must be affine and invertible. Weighted primitives instantiated without a skin are rejected. Unused weighted primitives are retained in v4 packages.

## Cooked format v4

The package starts with eight ASCII bytes `POIMAM04`, a little-endian uint32 metadata length, a little-endian uint32 binary length, then UTF-8 JSON metadata and binary payload. Lengths must account for the entire file. Static v1/v2/v3 packages remain readable; static cooking still emits byte-compatible v3 output.

Metadata retains v3 materials/images/nodes/roots and adds `skin` to each node, a `skinned` boolean per primitive, `skins`, `animations`, and `animation_bytes`. Each skin records name, skeleton node, joint node indices and column-major inverse bind matrices. Clip metadata records name, duration and ordered channels; each channel records target node, numeric path (translation 0, rotation 1, scale 2), numeric interpolation (step 0, linear 1, cubic 2), and key count.

Payload order is geometry, curves, then existing image mip bytes. Within each primitive, all 48-byte vertices come first, followed by influences if skinned (four little-endian uint32 palette indices and four float32 weights per vertex), then uint32 triangle indices. `geometry_bytes` bounds this section. Within each channel, all key times are float32, followed by float4 values. Cubic values are ordered in-tangent/value/out-tangent for each key. `animation_bytes` bounds the combined channels in clip/channel order. Image metadata and decoding are unchanged.

The content hash covers the entire package. A successful hash check is followed by structural and numerical validation; it does not waive bounds checks. Imports encode and decode before publication. Geometry and curve payloads must end exactly at their declared boundaries. Unused weighted geometry also forces v4, even when the file has no active skins or clips.

## Verification

- `tests/animation_native.cpp`: independent expected STEP/linear/Hermite/SLERP results, loop/clamp/rest behavior, non-parent-ordered rigs, inverse bind/mesh-transform cancellation, normals and tangents under nonuniform scale, singular-pose rejection, package round trips and malformed-state rejection.
- `tests/animation_contract.py`: real glTF import through CLI, quantized weights, flat-normal expansion, key/skin metadata pagination, source-independent sampling, failed-import isolation, corrupt-package rejection and parameter errors.
- `tests/animation_capture.py`: native Vulkan still captures compared with independently baked analytic geometry, culling equivalence, visible motion and unchanged authored state after source deletion.

The [captured analytic ribbon](evidence/m2-animation-reference.png) is a numerical graphics fixture, not a character-art or finished-quality demonstration.

These synthetic fixtures establish the supported asset reference path. They do not qualify arbitrary third-party characters or production animation performance. Evidence and actual executed platforms are recorded in [the animation asset checkpoint](evidence/m2-animation-assets.json); runtime playback has its own [contract and verification](RUNTIME_ANIMATION.md).

The [multi-take character guide](CHARACTER_LIBRARY.md) extends intake checks to an
unchanged, separately supplied KayKit character, with original curves, mixed
rigid/skinned attachments and relocated cooked reopening. Its CPU observations
remain separate from rendering and runtime locomotion qualification.
