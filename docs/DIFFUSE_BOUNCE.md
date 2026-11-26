# Local diffuse bounce

Visible lit and emissive surfaces can carry color into nearby diffuse surfaces
in the Vulkan deferred renderer. For example, sunlight on a red wall can tint
the floor near that wall while the rest of the room retains its sky fill.

## Author the bounce

Merge `screen_diffuse_bounce` into the existing `LightingEnvironment` value.
Submit the full component with a revision-guarded `world.transact`.

```json
{
  "ambient": [0.04, 0.05, 0.07],
  "exposure": 1,
  "screen_diffuse_bounce": {
    "radius_m": 1.5,
    "thickness_m": 0.08,
    "strength": 0.7
  }
}
```

| Required field | Meaning |
| --- | --- |
| `radius_m` | Maximum ray distance in world metres, in [0.2, 4]. |
| `thickness_m` | Approximate surface thickness along camera depth, in [0.005, 0.25] metres and at most the radius. |
| `strength` | Linear gain in [0, 1]. Zero keeps the authored profile and produces identity output. |

## Choose a resolution

Set the optional `resolution` field inside `screen_diffuse_bounce` to
`"half"` to reduce the number of ray origins. The renderer gathers incident
light at one anchor per 2×2 block, then reconstructs full-resolution light
from up to four nearby anchors. Depth, geometric normals and the receiver
plane constrain interpolation across surfaces. The final resolve applies
each receiver's own diffuse color. An isolated receiver can fall back to
its direct and sky lighting when no compatible anchor contributes.

Explicit `"full"` uses the original per-pixel gather. Omission defaults
to full. Explicit half requires `features.screen_diffuse_bounce_half_resolution`
as well as `features.screen_diffuse_bounce` in an export runtime.

Use single-sample deferred color. Set `startup_render.lighting_path` to
`deferred` for standalone playback. The renderer reports the fallback reason
on forward, diagnostic, hidden and world-depth foreground views.
An explicit profile is frozen with the runtime scene and requires a Vulkan
export runtime with `features.screen_diffuse_bounce`.

## Light transport and bounds

Deferred shading records outgoing direct diffuse radiance and emission in a
separate image. Local and directional lights share the scene's shadow,
contact, cloud and caustic visibility paths. Sky, authored ambient fill,
specular highlights and ambient occlusion stay outside the donor image.

Each eligible gather point tests 12 fixed cosine-weighted directions with
up to 12 depth intervals each. Projection interpolates reciprocal depth through
the visible segment. The first sampled surface within the depth slab can
contribute if it faces the receiver. Rays that miss contribute zero.
The gather averages the donated radiance and applies the receiver's base
color, diffuse fraction and strength. The result fades near the authored
radius and viewport edges. [Perspective-correct screen-space traversal](https://jcgt.org/published/0003/04/04/)

The pass uses the current frame's opaque depth and geometry. Camera
visibility limits the available donors, and the fixed sampling budget can
miss thin or distant surfaces. Off-camera continuity needs world-space
lighting data. Diffuse bounce composes before screen reflections, atmospheric
effects, sprites and reconstruction. The existing sky and ambient terms
remain available across the frame.

The two full-size RGBA16F textures use 16 bytes per render pixel. The full
route has a 128 MiB combined payload cap per context. Half adds an RGBA16F
texture with each dimension rounded up after dividing by two and a combined
160 MiB cap. The renderer also checks its reconstruction budget when that
route is active. Texture-driver overhead is separate.

## Inspect and verify

Use `world.lighting` and `runtime.lighting` to inspect the profile. Capture
diagnostics expose its reason, allocation, configured ray budget and GPU
timing samples when profiling is enabled. The half-resolution diagnostics
include the actual gather extent and bytes, with GPU timings for gather
and resolve and their combined interval. In a capture without other
composition or reconstruction, compare `raw_hdr` with `resolved_hdr` at a
receiver. Both are scene-linear. The former samples the scene image
before the bounce and the latter samples the composed result.

Run `tests/screen_diffuse_bounce_capture.py` against a Windows Vulkan
executable. Use `--reference` with the retained 0.0.456 binary, `--direct`
to exercise a directly lit donor, `--reflections` for matte composition,
`--fsr` for Native AA and `--runtime` for relocated standalone playback.
Use `--half` on a build with half-resolution support to test the
reconstruction, including adjacent foreground and sky edges.
