# Material height displacement

A `StaticMesh` can subdivide its input triangles on the GPU and use
a linear height texture to move the new vertices. The Vulkan camera and
shadow passes share the patch rates and evaluate the same displaced
surface. The ordinary frame, material, and shadow diagnostics remain
available.

## Author a surface

Import a height image with `asset.image.import` using `color_space: "linear"`.
Read the current `StaticMesh` component and add a `displacement` field. Submit
the complete component through a guarded `world.transact`.

```json
{
  "texture": "<cooked image content hash>",
  "axis": "y",
  "tile_mm": 1600,
  "height_m": 0.08
}
```

The red channel supplies the height in [0, 1]. It repeats on the two local
axes perpendicular to `axis` in tiles of `tile_mm` (32–64,000 integer
millimetres). The height scales by `height_m` (0–4 local metres) and moves the
surface along the positive local axis. Zero height selects the base mesh.
Material maps still use the mesh's UVs. Use a separate authored collider for
the gameplay surface.

The profile follows the chosen rigid mesh LOD. Use it on cooked static
terrain or on both tiles of a [mutable snow region](TERRAIN.md#add-material-height-to-snow).
Match projection and height across independently authored pieces when
their edges must meet. Camera attachments, foliage bending and skinned
meshes use other geometry paths.

## Renderer and fallback

Each patch sizes its edges from the same unjittered camera projection for
every draw pass. Edge rates target 18 pixels with fractional-even spacing
between 2 and 16 segments. Equal endpoints use the same expression and
height coordinates at duplicated edges, including a UV seam within one
drawable. This preserves the shared edge under the [Vulkan tessellation
invariance rules](https://docs.vulkan.org/spec/latest/appendices/invariance.html#tessellation-invariance).

The renderer evaluates the same height profile on the current and previous
local positions. On mutable snow those positions include the corresponding
compacted tile geometry. Profile changes invalidate prior correspondence.
The geometric bounds expand in the authored direction. Displaced casters
participate in full shadow-view culling and refresh affected shadow maps
as the camera can change their tessellation rate.

GPUs without Vulkan tessellation draw the base mesh. Inspect
`material_displacement_supported`, `last_draws.displacement_requested`,
`last_draws.displacement_draws` and `last_draws.displacement_patches` in
render diagnostics. Counts describe active draw instances and input patches
across camera and shadow views. Above 8,192 submitted input patches, the
renderer lowers the maximum segment count for the whole frame. The cap is
the even floor of `sqrt(8192 × 16² / patches)`, bounded from 2 through 16.
Above 32,768 submitted input patches, every pass uses base geometry for the
frame. `last_draws.displacement_segment_cap` and
`last_draws.displacement_budget_fallback` describe that decision. The profiler
records `renderer.displacement.input_patches`,
`renderer.displacement.segment_cap` and `renderer.displacement.budget_fallback`.
Measure the elapsed time in the opaque and shadow passes.

`world.asset.references` finds the height texture at
`/displacement/texture`. Project export includes it and requires a runtime
with `features.material_displacement`.

## Qualification

Run `tests/material_displacement_capture.py` against a Windows Vulkan build.
It compares depth against an independent projection, tests a duplicated
edge, and checks a cast shadow on a separate receiver. Pass `--runtime` to
verify relocation of a standalone export.
[Qualification results](evidence/m2-material-displacement-464.json)

Run `tests/snow_material_displacement_capture.py` with `--runtime-root` to
check the displaced seam, movement after a stamp, shadow occlusion and
save continuation with the source project unavailable. `--headless` checks
profile admission, rollback, collision and snow save continuation.
`tests/material_displacement_budget_capture.py` compares base geometry,
active displacement, whole-frame fallback and recovery after a dense draw.
[Snow qualification](evidence/m2-snow-material-displacement-465.json)
