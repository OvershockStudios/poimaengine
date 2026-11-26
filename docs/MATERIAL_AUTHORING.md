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

Use [cutout mip cooking](MATERIAL_ALPHA.md#preserve-cutout-coverage-at-a-distance)
to derive a coverage-adjusted image from an existing standalone or embedded
texture. Its own package and inspection record carry the reference cutoff.

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

Runtime creation freezes the resolved image references and factors. Ordinary authored edits leave the running scene unchanged. Explicit [live material editing](LIVE_MATERIALS.md) can adopt base color, emission, metallic, roughness and [alpha settings](MATERIAL_ALPHA.md) on existing authored drawables with `PbrMaterial`, preserving their live state and asset owners. [Live texture editing](LIVE_TEXTURES.md) separately adopts all five maps, samplers and strengths on supported existing PBR drawables. `double_sided`, geometry and general scene changes still require a new runtime. Recorded replay forbids intervening material or texture adoption. Graph compilation, texture transforms, compressed formats and streaming remain future work.

## Normal mapping and tangents

The glTF importer retains supplied TANGENT values after validating unit length, perpendicularity to the normal and handedness +/-1. When UV0 exists but tangents do not, it generates tangent frames using the pinned upstream MikkTSpace implementation, then reindexes complete attributes. Mirrored UV seams split vertices where tangent direction or handedness differ. Seam expansion counts against the existing vertex/package limits and can change cooked vertex counts. The algorithm uses additional temporary memory; it is not covered by the cgltf parser's allocation budget.

The shader transforms tangent vectors with the model matrix, normals with the inverse transpose, re-orthogonalizes the tangent, reconstructs the bitangent using handedness, applies normal scale to sampled X/Y and normalizes the result. Back faces reverse the resulting shading normal. Normal images use glTF's tangent-space convention; no automatic DirectX-style green-channel inversion is performed. Mips average encoded linear normal components and normalize during shading.

Default opaque static models cook as `POIMAM03`, with 48-byte vertices (position, normal, UV0, tangent XYZW), five texture slots, UV availability and normal scale. Animated opaque models use version 4; nondefault [alpha settings](MATERIAL_ALPHA.md) use version 5. Version 1/2 packages remain readable. They lack tangent data, so applying a normal override to them requires reimport; regular version-2 texture maps still render. New imports receive new hashes. The loader validates tangent frames, image uses and normal scale in addition to earlier checks. A missing normal/occlusion source map receives strength 1 for future overrides, including correction of the unused zero occlusion default in older packages.

## Reduce shimmer on curved glossy surfaces

Set `geometric_specular_aa` on the scene's `LightingEnvironment`. The strength
is a number from 0 to 1. Omission and zero select the original material
response. Start with 1 for shiny curved meshes and compare at the intended
render resolution. Keep the rest of the environment component when editing it.

```json
{
  "ambient": [0.03, 0.035, 0.045],
  "exposure": 1,
  "geometric_specular_aa": 1
}
```

The renderer measures normalized mesh normals across the pixel quad, after
object and vertex deformation. It uses their squared variation to widen the
GGX specular lobe, capped before applying the authored strength. Flat normals
retain the material's original roughness. Forward lighting and the deferred
G-buffer consume the same filtered roughness. The filter runs before
reconstruction. The variance estimate uses mesh normals. Normal-map mips
still average encoded samples. [Tokuyoshi and Kaplanyan, projected-space NDF filtering](https://jcgt.org/published/0010/02/02/)

Use the `LightingEnvironment` component from `world.describe` to discover the
setting. Positive strength appears in capture lighting receipts and
`world.dependencies` reports `needs_geometric_specular_aa`. Restart a runtime
session to adopt an authored change. Export requires a Vulkan runtime with
`features.geometric_specular_aa`. [Qualification results](evidence/m2-geometric-specular-aa-473.json)

## Reduce shimmer from fine normal-map detail

Call `asset.image.normal_variance` with the content hash of a linear normal
image. To use a model's embedded normal image, pass its model hash in `asset`
and its zero-based image index in `image`.

```json
{"jsonrpc":"2.0","id":1,"method":"asset.image.normal_variance","params":{"asset":"<linear image hash>"}}
```

Bind the returned hash to `PbrTextures.normal` or `StaticMesh.surface_layer.normal`
with a mipmapped sampler. Standard UV mapping, projected base normals and
surface-layer normals support the derived image. Inspect the image or the
resolved material to see `alpha_channel: "mean_normal_length"`. Cooking
preserves the authored scene revision and the source asset. Repeating the
cook with the same input returns the same hash.

The new `poima.image.v3` keeps the ordinary normal RGB mip chain. Each mip's
alpha stores the mean unit-normal length over the texel's full-resolution
footprint. Mip zero stores unit length. The shader widens the GGX lobe using
that statistic and the current normal strength, with a cap on the extra
roughness. It reads the value from the same normal texture sample. Base
normal strength zero and base-mip-only sampling retain their usual response.
The approximation is isotropic and uses the average sampled lengths when
interpolating between mips or projections. [Frostbite PBR course notes on normal-map prefiltering](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/course-notes-moving-frostbite-to-pbr-v2.pdf)

`world.dependencies` reports `needs_normal_variance_images` when a scene
references the derived image. A standalone export requires an image decoder
that advertises `features.normal_variance_images`. Packaged games can inspect
and sample these images. Authoring the derived image requires a writable
project. [Qualification results](evidence/m2-normal-variance-474.json)

## Example

From Windows PowerShell in the repository root:

```powershell
Get-Content examples/normal-grid.jsonl | .\build\windows-runtime\poima.exe world build/normal-demo.world.json
```

Use fresh world/capture paths. The [captured example](evidence/m2-normal-grid.png) compares normal scale 0 in the upper row with 1.5 below; metallic increases across the columns, with matching factor roughness. The source is original sample art generated offline by `python3 examples/assets/generate_textured_sphere.py --normal-map`. This offline script does not provide native procedural-material authoring.

## Blend a surface layer

Add `surface_layer` to an opaque world `StaticMesh` through a guarded
`world.transact` component set. Import the layer color as sRGB and the
other images as linear data. Replace the example IDs with cooked hashes.

```json
{
  "base_color": "<color image hash>",
  "orm": "<packed occlusion, roughness and metallic image hash>",
  "normal": "<optional normal image hash>",
  "mask": "<optional painted mask image hash>",
  "tile_mm": 3000,
  "base_tile_mm": 1200,
  "weight": 0.85,
  "slope_degrees": [20, 45]
}
```

The two repeat periods use world XZ coordinates. Keep matching periods on
neighboring terrain tiles to align their texture phase. `tile_mm` is required.
Omitting `base_tile_mm` keeps the base material on its original UVs and
samplers. Provide integer millimetres in 32–64,000. Layer normals use +X
and +Z as their texture directions on level ground.

The layer starts with `weight` in `[0, 1]`, with a default of 1. An optional
mask multiplies it by the linear red channel, sampled with clamped UV0.
An optional pair of strictly increasing angles in `[0, 90]` blends smoothly
from zero coverage to full coverage using the surface normal before normal
mapping. With no slope range, coverage follows the weight and mask.

The renderer blends linear base color, occlusion, roughness and metallic
independently. The layer normal blends with the underlying shading normal.
Covered areas suppress base emission in proportion to coverage. Wetness
scales both layers before blending. Surface marks apply after the blend.
Terrain detail levels and material displacement share the same shading path.
When using a mask, supply valid UV0 on each detail tier.

`world.asset.references` exposes the layer images under `/surface_layer/`.
Dependency and project inspection report `needs_surface_layer`. Standalone
export includes the selected images and requires `features.surface_layer`.
The layer belongs to the authored `StaticMesh`. Live `PbrTextures` edits
preserve the layer, and edits to the layer itself take effect in a new
runtime snapshot.

`tests/terrain_surface_layer_capture.py` checks native forward and deferred
shading against a color, normal and lighting oracle. It also checks source-free
export and the original material path. [Surface-layer qualification](evidence/m2-terrain-surface-layer-468.json)

### Texture steep slopes and cliffs

Set `"projection": "triplanar"` on `surface_layer` to project the textures
from three world axes. The shader blends the projections using the fourth
power of the unperturbed normal's absolute components. The three projections
sample ZY, XZ and XY coordinates. Adjacent tiles share texture phase when their
repeat periods match. The default `"xz"` keeps the original top-down mapping.

With `base_tile_mm` set, the projection applies to the base maps at that
repeat period and to the layer maps at `tile_mm`. The projected base maps
work on rigid meshes without UVs or tangents. Omit `base_tile_mm` to keep
the base maps on mesh UVs. A painted coverage mask always uses UV0.
Each projected normal map contributes a slope in its own axes. The shader
combines the slopes and applies the result within the surface's tangent plane.
[Surface-gradient bump mapping](https://jcgt.org/published/0009/03/04/)

Check `needs_surface_layer_triplanar` in the dependency or project report.
The export runtime must advertise `features.surface_layer_triplanar`. Relocate
the export and inspect the cliff and slope transitions from the intended
camera. This option can sample a map from three directions near a blend.
Measure it on the material and views you intend to ship.

## Evidence

[Evidence](evidence/m2-material-authoring.json) covers native import/authoring tests, mirrored-seam unit checks, scalar/GPU normal-map comparisons, independent image overrides, preview/retry behavior, effective inspection, disable/inherit semantics, source-independent reload and frozen runtime replay. These are bounded fixtures, not full glTF conformance or production graphics qualification.
