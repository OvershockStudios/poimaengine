# Material height displacement

A rigid `StaticMesh` can subdivide its input triangles on the GPU and use
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

The profile follows the chosen rigid mesh LOD. Camera attachments and
foliage bending require separate drawables. Cooked static terrain meshes
can use the same geometry path. The mutable snow surface and skinned
meshes have separate geometry contracts. Match projection and height
across separately authored pieces when their edges must meet.

## Renderer and fallback

Each patch sizes its edges from the same unjittered camera projection for
every draw pass. Edge rates target 18 pixels with fractional-even spacing
between 2 and 16 segments. Equal endpoints use the same expression and
height coordinates at duplicated edges, including a UV seam within one
drawable. This preserves the shared edge under the [Vulkan tessellation
invariance rules](https://docs.vulkan.org/spec/latest/appendices/invariance.html#tessellation-invariance).

The renderer evaluates the same local height for present and prior rigid
transforms. Displacement changes invalidate prior correspondence. The
geometric bounds expand in the authored direction. Displaced casters
participate in full shadow-view culling and refresh affected shadow maps
as the camera can change their tessellation rate.

GPUs without Vulkan tessellation draw the base mesh. Inspect
`material_displacement_supported`, `last_draws.displacement_requested`,
`last_draws.displacement_draws` and `last_draws.displacement_patches` in
render diagnostics. Counts describe draw instances and input patches across
camera and shadow views. The per-frame ceiling is 8,192 input patch
submissions. `renderer.displacement.input_patches` records the same estimate
while profiling. Count time in the ordinary opaque and shadow passes.

`world.asset.references` finds the height texture at
`/displacement/texture`. Project export includes it and requires a runtime
with `features.material_displacement`.

## Qualification

Run `tests/material_displacement_capture.py` against a Windows Vulkan build.
It compares depth against an independent projection, tests a duplicated
edge, and checks a cast shadow on a separate receiver. Pass `--runtime` to
verify relocation of a standalone export.
[Qualification results](evidence/m2-material-displacement-464.json)
