# Static glTF assets and textured materials

Poima 0.0.9 supports native glTF/GLB import, immutable cooked model packages, editable hierarchy instances and an initial textured metallic/roughness rendering path. Import and inspection work in headless builds. Windows Vulkan capture/player builds render the imported indexed geometry. This implements another part of M2; it does not establish full glTF support, Unity conversion or the planned advanced renderer.

## Import, inspect and instantiate

To render the original eight-sphere material example from the repository root in Windows PowerShell:

```powershell
Get-Content examples/material-grid.jsonl | .\build\windows-runtime\poima.exe world build/materials.world.json
```

For a textured version, substitute `examples/textured-grid.jsonl` and a fresh `build/textured.world.json` path; its capture is `build/textured-grid.bmp`. The bundled tiled surface is original generated sample art. `examples/assets/generate_textured_sphere.py` can regenerate the GLB offline, but is not an implemented native procedural-material framework.

This uses the bundled original GLB and writes `build/material-grid.bmp`. Use fresh world/capture paths when repeating the example. The top row uses roughness 0.18, the bottom 0.65; metallic increases from 0 to 1 across each row.

Open `poima world build/model.world.json`. Paths passed to `asset.import` are absolute, or relative to the world document's directory. For example:

```json
{"jsonrpc":"2.0","id":1,"method":"asset.import","params":{"source":"../examples/assets/material-sphere.glb"}}
```

The result returns `asset` (a 64-character SHA-256 identity), package bytes, node/primitive/image/vertex/triangle counts, selected scene roots and diagnostics. Import publishes a package into `build/model.world.json.assets/<asset>.pmodel`; it does not increment the scene revision. Repeating an identical import deduplicates the cooked output. It still parses/cooks the input; an incremental dependency/build cache remains future work.

Inspect metadata without dumping vertex arrays:

```json
{"jsonrpc":"2.0","id":2,"method":"asset.inspect","params":{"asset":"<returned hash>","section":"primitives","offset":0,"limit":16}}
```

`section` is `summary` (default), `nodes`, `primitives`, `images`, `skins`, or `animations`. Pages contain at most 64 items and return `next_offset` or null. Node pages expose source indices, names, parents, local TRS and primitive references; primitive pages expose counts, material factors, map-to-image references, glTF sampler enums and occlusion strength. Image pages expose color space, dimensions and bytes of each mip level. Raw geometry remains in the binary artifact.

Instantiate through an ordinary atomic transaction, replacing `<returned hash>` with the actual asset ID:

```json
{"jsonrpc":"2.0","id":3,"method":"world.transact","params":{"request_id":"00000000000000000000000000000001","base_revision":0,"ops":[{"op":"asset.instantiate","id":"00000000000000000000000000000064","asset":"<returned hash>","name":"Imported model","parent":null}]}}
```

The caller supplies the new outer root ID. The operation creates that root, the selected glTF scene's ordinary hierarchy nodes, and a child entity per mesh primitive. Each primitive has editable `StaticMesh` and `PbrMaterial` components. Node transforms, visibility, parenting, names and material factors use the existing core operations; there is no opaque second scene graph hidden inside a GUI. Preview, revision guards, transaction rollback, persisted retry receipts and retired-ID checks apply to the whole instantiation. It can be combined with other authoring operations.

Generated IDs are the first 32 lowercase hex digits of SHA-256 over UTF-8 `poima.instance.v1/<root-id>/node/<source-index>`. Primitive children append `/primitive/<slot>` to the node suffix. The source index and slot are zero-based. Collisions or retired IDs fail the transaction. Different outer roots produce distinct editable instances sharing the same geometry package. Source transforms remain local; the wrapper root starts at identity and can place the entire model.

Only the selected glTF scene is instantiated. If there is no default scene, all parentless nodes are used. The importer currently validates/cooks all meshes in the source file, including unused ones. Import does not synthesize collision bodies; add explicit collider components where appropriate. Static mesh collision and automatic collider cooking remain future work.

Animated models now have a separate [reference import, inspection and pose-capture path](ANIMATION_ASSETS.md). They cannot yet be instantiated into the ordinary runtime.

## Components

| Component | Required fields |
| --- | --- |
| `StaticMesh` | `asset`: immutable package hash; `primitive`: zero-based cooked primitive index; `visible`: boolean. |
| `PbrMaterial` | `base_color`: three linear RGB factors; `emissive`: three linear RGB factors; `metallic`, `roughness`: scalars; `double_sided`: boolean. All numeric material factors are in [0,1]. |

An entity cannot combine `StaticMesh` and the older `MeshRenderer`. `PbrMaterial` can override an imported primitive or shade the built-in box. Without an explicit override, imported geometry uses its cooked material. A legacy box without PbrMaterial retains the earlier Lambert/ambient preview appearance.

`world.describe` supplies schema revision 16. Import/asset-file errors use `-32050`; ordinary malformed component/transaction requests retain their existing codes. A reference can be authored before its package is available, but observation/runtime creation validates and resolves the package and primitive. Runtime creation freezes geometry and material values with the authored revision. Later authored edits do not alter that running state.

## Initial supported profile

- glTF 2.0 JSON and GLB, with embedded GLB buffers, base64 data buffers, or external buffers beneath the source directory.
- Static indexed or non-indexed triangle lists, POSITION, normals and optional first UV set; strided accessors are supported. cgltf decodes supported accessor representations before cooking. Missing normals generate flat triangle normals and a diagnostic. Authored tangents are validated/preserved; missing tangents are generated with MikkTSpace when UV0 is present.
- Positive-scale local TRS hierarchies and normalized rotations, mesh instances and multiple primitives/materials.
- Opaque core metallic/roughness base-color, metallic, roughness and emissive factors, with double-sided rendering.
- PNG (8-bit output, including palette/grayscale inputs) and JPEG texture images from GLB buffer views, base64 image data URIs or relative files beneath the source directory. Referenced primitives must supply TEXCOORD_0. Normal maps use linear tangent-space RGB with a validated/generated tangent frame.
- Base-color and emissive RGB sampled as sRGB; metallic/roughness channels B/G and occlusion channel R sampled as linear data. Factors multiply the samples. Occlusion strength interpolates between unoccluded and sampled values, and affects the ambient diffuse term only.
- Repeat, clamp and mirrored-repeat addressing, nearest/linear magnification, all six glTF minification modes. Missing samplers default to repeat and linear/trilinear filtering. Non-mip samplers restrict the texture view to level zero.
- Complete RGBA8 mip chains, area-box filtered during cooking. sRGB RGB channels are decoded before filtering and re-encoded afterward; alpha stays linear. Odd image edges are included. No vertical image flip: glTF UV (0,0) addresses the upper-left texel.

16-bit PNG, HDR images, KTX/Basis/WebP, texture transforms, morph targets, compression, required extensions, extended material models, alpha masking/blending, vertex colors, additional UV sets, sparse indices, non-triangle-list modes, matrix-authored nodes and mirrored/zero scales are rejected explicitly. Cameras/lights retain their transform nodes but are not converted to engine camera/light components; this produces a diagnostic. Sparse attribute unpacking is delegated to cgltf but has not yet received a dedicated Poima fixture. Do not infer general glTF conformance from the current fixture set.

This strict profile is intended to grow. Broader transforms/materials, compressed GPU formats and streaming remain required work. [Material authoring](MATERIAL_AUTHORING.md) adds independent image imports, texture-slot overrides, sampler/strength editing and effective inspection. Purchased-asset rights and Unity-specific material/prefab/script conversion are separate work.

## Storage, limits and recovery

The model package is versioned: eight-byte ASCII `POIMAM03`, a little-endian uint32 metadata length, a little-endian uint32 binary length, UTF-8 JSON metadata, then packed geometry followed by RGBA8 mip chains. Each primitive stores interleaved little-endian float32 position/normal/UV/tangent XYZW (48 bytes per vertex), followed by uint32 triangle indices. Metadata contains counts, material factors, nodes, roots, diagnostics, five texture references/samplers, normal scale, UV availability, image color spaces/mip sizes, the geometry byte boundary and importer identity. Decoding checks the version, lengths, counts, finite values, normalized normals/quaternions, tangent frames, indices and acyclic hierarchy and mip lengths/dimensions, sampler enums and map color spaces before exposing the model. Existing `POIMAM01`/`POIMAM02` packages remain readable; new imports use version 3 and receive new content hashes even when the source is unchanged.

The filename hash covers the exact package bytes. Loads verify it before decoding. Imports stage and flush a package before atomic publication, and refuse to overwrite an existing corrupt package. A leftover `.pending` is not an asset. As with the world store, power-loss directory durability is not yet qualified. No automatic asset garbage collection is implemented. Keep the world's `.assets` directory with the world when moving/backing up a project; removing the original glTF source does not prevent loading an already-cooked model. Preserve sources separately for future reimport/upgrades.

Initial limits are 128 MiB cumulative source/external-file reads, a separate 128 MiB cgltf allocation budget, 10,000 source nodes/meshes/primitives, 50,000 accessors, one million cooked vertices, three million indices, 8 MiB cooked metadata and 64 MiB per package. Observation/runtime construction allows 256 MiB of referenced package bytes, loading each package once for that construction; independent image packages share that budget. The existing world cap remains 10,000 entities/16 MiB. These bound individual stages; they are not a hard total process-memory cap or a qualified production content budget. Decoding and geometry copies consume additional memory.

Texture limits are 32 MiB encoded bytes per image, 4096 per dimension, four million pixels per image, 256 cooked image/color-space variants, and 32 MiB combined cooked mip bytes per model. The decoder uses a separate 128 MiB allocation budget. One source image used for both color and data maps cooks into separate sRGB and linear mip chains; uses with the same source/color space share an image. GPU uploads have a separate initial 256 MiB texture-data cap per renderer context. GPU allocation overhead is additional; these limits are temporary and do not establish production scalability.

External buffers and image files must remain under the source directory; network resources are not fetched. Geometry and metadata stay separate from authored JSON. Sources are read only. The immutable store is reserved against capture-output writes.

## Initial PBR renderer

The Vulkan path uses indexed GPU vertex/index buffers and shares uploaded geometry, textures and binding sets across instances within a renderer context. Per-frame camera/view data uses a constant buffer; model/normal transforms and material factors fit in 128-byte draw push constants. The shader uses a GGX normal distribution, height-correlated Smith visibility, Schlick Fresnel and Lambert diffuse, followed by simple Reinhard display mapping and sRGB output. Roughness is clamped to 0.045 for finite highlights.

Lighting supports [authored directional/point/spot sources, ambient fill and exposure](LIGHTING.md), with the old fixed source retained for scenes without authored lighting. Optional [shadow maps](SHADOWS.md) now provide direct-light visibility. There is no sky/IBL, reflection probe, GI or HDR presentation. Metallic surfaces can therefore be dark outside the direct highlight. Draws are serial and individually submitted; this is geometry reuse, not hardware-instanced batching. The player still waits for GPU completion every frame. No scene performance target or next-generation visual-quality claim is established by this baseline.

## Verification

[Evidence](evidence/m2-static-assets.json) records native Linux/Windows import results, cooked IDs, failure cases and GPU observations. Tests cover GLB/external/base64 equivalence, source-independent reload, preview/retry/rollback, hierarchy/material edits, corruption rejection, flat-normal generation and SHA-256 known answers. The Vulkan test compares actual material pixels with an independent scalar reference, checks front/back faces, preserves a frozen runtime material during authored edits, and reopens the scene after deleting its import sources. A [material grid](evidence/m2-material-grid.png) shows the initial direct-light response; it is not a target for the finished engine's graphics.

Native Linux graphics, production-size content, arbitrary third-party glTF assets and the excluded profile features remain unqualified.


[Texture evidence](evidence/m2-textures.json) extends the original checks with native Linux/Windows source-form equivalence, JPEG decoding, mip/color-space validation, v1 package compatibility, rejection cases and native NVIDIA/AMD captures. `tests/texture_native.cpp` checks linear/sRGB filtering and odd-dimension edge contributions; `tests/texture_capture.py` compares material pixels to scalar expectations and checks UV orientation, sampling modes, mip use, source-independent reload, frozen runtime ownership and a 120-tick textured player replay. These tests use synthetic original fixtures rather than claiming arbitrary third-party content coverage.

[Normal mapping and per-object texture overrides](MATERIAL_AUTHORING.md) extend this contract, with separate [evidence](evidence/m2-material-authoring.json).
