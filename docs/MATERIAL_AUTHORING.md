# Material image authoring and normal maps

Poima 0.0.9 adds normal mapping, independent PNG/JPEG image assets, sparse per-entity texture overrides and effective-material inspection. All operations are available in the headless world service; Vulkan capture/player consumes the same resolved data. This extends M2. It does not complete the production renderer, material graphs or live runtime structural editing.

## Import an image without a model

Within `poima world build/room.world.json`:

```json
{"jsonrpc":"2.0","id":1,"method":"asset.image.import","params":{"source":"../art/paint.png","color_space":"srgb"}}
```

Use `srgb` for base-color/emissive images and `linear` for metallic/roughness, occlusion or normal images. Paths resolve against the world document directory, as with model imports. The result includes a SHA-256 `asset`, `poima.image.v1` format, package size, color space and mip dimensions/bytes. `asset.image.inspect` accepts `{ "asset": "<hash>" }` and returns the same metadata. Importing does not change a scene revision; identical cooked bytes deduplicate.

Images are stored beside models in `<world>.assets/<hash>.pimage`. Packages contain `POIMAI01`, little-endian uint32 JSON and binary lengths, versioned JSON metadata and raw RGBA8 mip chains. Hashes are verified before decoding. The same limits, mip validation, staging/flush/publication and corruption refusal as model assets apply. Source images may be removed after cooking. Preserve the entire `.assets` directory when moving/backing up the world.

The image profile remains PNG/JPEG, 4096 per dimension, four million pixels, 32 MiB encoded source and 32 MiB cooked mip payload. The independent decoder allocation budget is 128 MiB; referenced model and image packages share a 256 MiB observation/runtime-construction budget. These stage bounds are not a hard process-memory ceiling.

## Override texture slots

`PbrTextures` is an optional component. Use ordinary `component.set` transactions, for example:

```json
{"op":"component.set","id":"<entity id>","type":"PbrTextures","value":{"base_color":{"asset":"<image hash>","wrap_s":10497,"wrap_t":10497,"min_filter":9987,"mag_filter":9729},"normal":{"asset":"<linear normal image hash>"},"normal_scale":0.6,"occlusion_strength":0.8}}
```

This operation belongs inside `world.transact`; revision guards, previews, atomic rollback and persisted retry receipts are unchanged. A preview validates authored structure without publishing it. There is still no general undo-history command.

Slots are `base_color`, `metallic_roughness`, `emissive`, `occlusion` and `normal`. Each slot has three distinct states:

| Authored state | Effective behavior |
| --- | --- |
| Field absent | Inherit that map from the imported primitive. |
| `null` | Disable that inherited map. Color/data factors use their neutral sample; normal mapping is disabled. |
| Reference object | Use the referenced image and sampler. |

A reference requires `asset`. Omit `image` for a standalone `.pimage`; include `image: <zero-based index>` to reference a cooked image in another `.pmodel`. Model image indices/color spaces come from `asset.inspect` with `section: "images"`; primitive map references come from `section: "primitives"`. Images must have the slot's correct color space. A new reference defaults to repeat addressing, linear magnification and trilinear minification; it does not inherit the old sampler.

Optional sampler enums follow glTF: wrap `10497` repeat, `33071` clamp-to-edge, `33648` mirrored repeat; magnification `9728` nearest or `9729` linear; minification `9728`, `9729`, or `9984`–`9987`. `occlusion_strength` is [0,1]; `normal_scale` is [0,16]. Omitted strengths inherit the model values (or 1 for a built-in box). `component.set` replaces the entire component; removing `PbrTextures` restores all imported maps/strengths. Factor values remain in the separate `PbrMaterial` component.

References can be authored before their packages are available, consistent with `StaticMesh`. Schema validation does not prove resource availability. Effective inspection, capture and runtime creation resolve references and reject missing/corrupt assets, bad image indices, incompatible color spaces, missing UVs or unusable tangent frames with `-32050`. Invalid sampler values/types fail ordinary transaction validation with `-32602` and roll back the transaction.

Built-in boxes have per-face UVs and tangent frames. Adding `PbrTextures` selects PBR shading on a box even if it previously used the legacy preview shader; without a `PbrMaterial`, the box albedo becomes its base factor and metallic defaults to zero. Imported meshes retain their geometry and imported factor defaults. Overrides own lightweight map references, not copies of the vertex/index arrays.

## Inspect the effective material

```json
{"jsonrpc":"2.0","id":2,"method":"entity.material","params":{"id":"<entity id>","revision":4}}
```

The result contains the revision, factor material (or null for a legacy box), effective map references/samplers, normal scale and occlusion strength. It resolves inherited and authored maps in one bounded response. `entity.get` still returns the authored component values; `entity.material` reports what rendering will use. Both are read-only and accept a revision guard.

Runtime creation freezes the resolved image references and factors. Subsequent authored edits do not change a running scene, including continuous replay. Restart the runtime to apply authored material changes. Live runtime material editing, graph compilation, texture transforms, compressed formats and streaming remain future work.

## Normal mapping and tangents

The glTF importer retains supplied TANGENT values after validating unit length, perpendicularity to the normal and handedness +/-1. When UV0 exists but tangents do not, it generates tangent frames using the pinned upstream MikkTSpace implementation, then reindexes complete attributes. Mirrored UV seams split vertices where tangent direction or handedness differ. Seam expansion counts against the existing vertex/package limits and can change cooked vertex counts. The algorithm uses additional temporary memory; it is not covered by the cgltf parser's allocation budget.

The shader transforms tangent vectors with the model matrix, normals with the inverse transpose, re-orthogonalizes the tangent, reconstructs the bitangent using handedness, applies normal scale to sampled X/Y and normalizes the result. Back faces reverse the resulting shading normal. Normal images use glTF's tangent-space convention; no automatic DirectX-style green-channel inversion is performed. Mips average encoded linear normal components and normalize during shading. Variance-aware specular antialiasing is not implemented.

Models now cook as `POIMAM03`, with 48-byte vertices (position, normal, UV0, tangent XYZW), five texture slots, UV availability and normal scale. Version 1/2 packages remain readable. They lack tangent data, so applying a normal override to them requires reimport; regular version-2 texture maps still render. New imports receive new hashes. The loader validates tangent frames, image uses and normal scale in addition to earlier checks. A missing normal/occlusion source map receives strength 1 for future overrides, including correction of the unused zero occlusion default in older packages.

## Example

From Windows PowerShell in the repository root:

```powershell
Get-Content examples/normal-grid.jsonl | .\build\windows-runtime\poima.exe world build/normal-demo.world.json
```

Use fresh world/capture paths. The [captured example](evidence/m2-normal-grid.png) compares normal scale 0 in the upper row with 1.5 below; metallic increases across the columns, with matching factor roughness. The source is original sample art generated offline by `python3 examples/assets/generate_textured_sphere.py --normal-map`. This offline script does not provide native procedural-material authoring.

## Evidence

[Evidence](evidence/m2-material-authoring.json) covers native import/authoring tests, mirrored-seam unit checks, scalar/GPU normal-map comparisons, independent image overrides, preview/retry behavior, effective inspection, disable/inherit semantics, source-independent reload and frozen runtime replay. These are bounded fixtures, not full glTF conformance or production graphics qualification.
