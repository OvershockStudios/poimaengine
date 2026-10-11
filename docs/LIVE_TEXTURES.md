# Live texture editing

An agent can import or cook an image, author a `PbrTextures` change and explicitly
apply it to an existing drawable in a running game. Texture and scalar edits
share the same `material_revision`. Applying textures preserves the runtime,
compiled gameplay, simulation tick and original mesh/rig owners. Ordinary
authoring changes remain separate until applied.

This experimental API is outside the stable authoring-core v1 contract.
It supports all five existing maps—base color, metallic/roughness, emission,
occlusion and normal—plus wrap/filter settings, normal scale and occlusion
strength. Select 1–32 existing authored drawables, using IDs in ascending order.
Imported meshes may retain their inherited PBR material; primitives require
explicit `PbrMaterial`. Every selected entity must have changed texture
intent. It cannot select spawned objects or change geometry/scalar materials.

## Author and apply

1. Import PNG/JPEG with `asset.image.import`, or reference an embedded cooked
   model image. Use sRGB for base color/emission and linear for the other maps.
2. Author the complete desired `PbrTextures` component with `world.transact`.
   Use `component.remove` to restore model defaults.
3. Inspect the runtime for its exact session, tick, structure/material revisions,
   frozen authored revision and content SHA. The transaction revision identifies
   the authored target.
4. Call `runtime.texture.apply` with the same guard fields as
   [scalar adoption](LIVE_MATERIALS.md#author-inspect-apply), and selected IDs.
5. Inspect accepted texture values with `runtime.texture.inspect`; use player
   or capture receipts to identify the frame actually submitted/presented.

The apply response contains `session_id`, `tick`, incremented
`material_revision`, adopted `authored_revision`, `content_sha256`, `ids` and
`replayed`. Exact retained retries resolve before current clocks and files are
checked, without applying twice. Changed request-ID reuse rejects; texture,
scalar and other runtime mutation receipts share the request namespace.

An interactive player may accept an edit while unpaused. Its exact tick guard
can conflict; inspect again and submit a fresh request. Recorded replay forbids
intervening edits. Acceptance is not a renderer-readiness acknowledgement.

## Inheritance and inspection

The existing [material authoring rules](MATERIAL_AUTHORING.md) apply unchanged:

| Intent | Effective map |
| --- | --- |
| Slot omitted | Original imported mesh map, or no map on a primitive. |
| Slot set to `null` | Disabled, including an inherited model map. |
| Slot set to an asset reference | Referenced image with explicit/default sampler settings. |
| Component removed | All original model maps and strengths, or primitive defaults. |

Omitted strengths inherit model values, otherwise one. Explicit normal scale
is 0–16; occlusion strength is 0–1. Missing UVs, invalid normal-map tangent
frames, wrong color spaces and unsupported samplers reject before commit.

`runtime.texture.inspect` takes `session_id`, exact `tick`, sorted `ids` and
optional exact `material_revision`. Each `textures` row contains `id`, the
accepted authored component (or `null` when absent) and `effective`. Effective
maps expose image SHA, color space, dimensions, mip count and sampler enums;
strengths are returned separately. The image SHA identifies canonical cooked
image bytes, including embedded images; it is not the source model's asset ID.
Inspection reads immutable native owners without rendering or advancing ticks.

## Content and state boundaries

Admission checks the complete authored delta and freshly validates both frozen
source and target asset closures. Only selected texture intent and precisely
associated dependency/provenance changes are permitted. Existing active asset
bytes and provenance remain exact; unrelated authoring must be reverted first.
A new provenance binding must identify a newly introduced selected texture
dependency. Existing bindings cannot be rewritten. Removing a binding requires
its selected dependency to be fully absent from the target active inventory.
A new model used as an image source does not authorize replacing geometry.

New immutable map owners, definitions, baseline document and receipt are staged
before a no-throw commit. Existing snapshots retain old maps. The shared material
epoch increments once and invalidates temporal scene history. Original geometry,
rigs, gameplay state and unrelated save payload remain unchanged.

Adopted intent and cooked content persist in saves. As with scalar changes,
nonzero material epochs require exact material guards on saves and captures.
Fresh restore reconstructs the adopted baseline and resets the session-local
epoch to zero. Pending gameplay saves must finish before adoption.

## Performance limits

Import, closure validation, decoding and GPU uploads are currently synchronous
and bounded by existing image/package/cache limits. Texture upload can wait for
the GPU device to become idle. This implementation does not promise hitch-free
updates or the representative one-second Alpha iteration target. Renderer
failure after native acceptance is reported separately; a GPU failure does not
silently undo gameplay or the accepted baseline. Presentation receipts do not
prove physical scanout or GPU completion.

## Qualification

The [0.0.114 results](evidence/m2-live-textures.json) record nine Windows native
checks, schema discovery and 106 hardware captures on one AMD GPU. Standalone
forward/deferred comparisons cover all five maps, sampler controls, zero strengths,
inheritance and a weighted bind-pose mesh. Two continuous player windows cover
paused/running adoption and twelve replacement cycles each. A separate retained
CoreCLR game preserves its full save payload except the adopted baseline,
continues to completion and restores exactly in a fresh process.

These are functional checks on small fixtures. They do not establish animated
character-game rendering, Native AOT texture adoption, GPU completion, peak VRAM,
frame performance or the one-second iteration target. The retained failed initial
CPU fixture and its corrected successor are identified separately.
