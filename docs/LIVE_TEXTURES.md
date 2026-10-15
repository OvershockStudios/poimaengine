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
updates or a general one-second latency bound. The selected representative
cohort below meets its declared one-second Alpha target. Renderer
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

The 0.0.114 checks use small fixtures. They do not establish animated
character-game rendering, Native AOT texture adoption, GPU completion, peak VRAM,
frame performance or the one-second iteration target. The retained failed initial
CPU fixture and its corrected successor are identified separately.


## Representative encoded-texture iteration

The [0.0.159 cohort](evidence/m2-live-texture-iteration.json) passes all twenty
PNG import-to-verified-visible intervals at **0.854 s mean, 0.909 s p95 and
0.911 s maximum**. Nearest-rank p95 is sorted sample 19; every sample is retained.
This meets the selected one-second target for this workload and device.

The paused Windows player uses native 0.0.158 with the retained 0.0.94 Native AOT
Performance Yard game. It keeps all 2,394 authored entities, 24 animated skins,
768 box props, 64 lights, 32 dynamic bodies, both cameras and all 50 gameplay
values. Rendering uses the existing overview camera at 1920×1080, FOV 60,
deferred lighting, medium GTAO, one sample and two frames in flight. Reconstruction
and frame generation are disabled. Audio is enabled and drains on shutdown;
this does not qualify audible output.

Four original 1024×1024 RGBA8 PNGs from Kenney's CC0 Animated Characters
Protagonists pack supply the states. All 24 selected skins receive only an
explicit `PbrTextures.base_color` override. Scalar materials and every omitted
map/strength retain their original values. Actual import receipts identify four
distinct 5,592,876-byte cooked images, each with eleven sRGB mip levels.
Independent PNG decoding verifies complete base-mip RGBA equality; lower mip
contents are retained by digest rather than independently quality-qualified.

An untimed prerequisite imports and adopts all four states twice. Its nine
captures, ten complete native saves and fourteen independently recomputed masks
verify recurrence and visible changes outside the HUD. Temporarily moving only
a new, inactive package causes adoption to reject with `-32050`. The entire
prior snapshot remains exact, including both content identities. Restoring the
same bytes lets the identical apply request succeed.

The measured run starts from a fresh full-world copy. Four untimed imports and
adoptions establish the fourth state in that owner. A fresh baseline capture
must match its external reference. Twenty timed edits then cycle the four states
five times. Each interval starts before the timed complete authored-byte verification
read and native PNG import, after the separately recorded oracle preparation. It includes PNG decoding, mip generation, encoding,
existing-package validation, guarded authoring/adoption, a matching presented
receipt, a strictly later fresh capture and full image/effect-mask verification.
Cached package existence never substitutes for the actual import operation.
The presentation-observation p95 is separately **0.728 s**; it does not measure
physical scanout.

Expected complete authoring bytes and reference comparisons prepare before the
primary interval. Authoring preparation has **0.464 s mean and 0.580 s p95**.
The separate preparation-start-through-verification interval has **1.318 s mean
and 1.426 s p95**. Complete texture/scalar inspections and native save comparisons
follow each primary interval. These exclusions are part of the declared
measurement boundary, not a claim about total task wall time.

All 21 fresh captures match their actual state-reference bytes exactly. Every
timed transition changes at least 156,460 non-HUD pixels by a channel delta of
eight; the threshold is 100 pixels. Exact-reference admission permits reuse of
independently verified pair masks; unknown bytes take the full actual RGB path
inside the timer. All forty retained masks are independently recomputed.

Each complete saved document matches the precise adopted texture delta and
native frozen-ledger policy. All 14 version-7 native payload keys remain exact
across twenty pairs except `authored_revision` and `content_sha256`, which
identify the adopted baseline. Both cameras, physics, animation, components,
sound, UI and all 50 gameplay values remain present. The same player/runtime
owns every sample. All four Job processes close naturally.

To reproduce the sequence, use [Performance Yard preparation](../examples/performance-yard/README.md#prepare-the-world)
and the original licensed source pack. Begin through the compiled New Game
control, settle 60 ordinary ticks after the initial tick, then pause tick 61.
Use existing camera `00000000000000000000000000000066` and the settings above.
Freeze four actual state references and the native UI exclusion rectangles
before timing. On a fresh copy, establish the fourth state, capture its baseline,
then cycle twenty imports of `criminalMaleA.png`, `cyborgFemaleA.png`,
`skaterMaleA.png` and `skaterFemaleA.png`. Use fresh author/apply/capture IDs,
exact current guards and complete post-interval checkpoints. Keep every receipt,
image, mask, timing sample and failure. The recorded cohort uses specifically
pinned retained artifacts; rebuilding other versions is a separate cohort.

This qualifies one paused Windows/NVIDIA animated Native AOT workload. It does
not establish arbitrary image-size latency, cold-start cost, hitch-free uploads,
advancing-game edits, other GPUs, frame performance, physical input or the broader
Alpha release gates.
