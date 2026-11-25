# Volumetric clouds

Set `LightingEnvironment.atmosphere.clouds` to render a cloud layer above the
world. It uses the [atmospheric sky](ATMOSPHERE_SKY.md), including the linked
directional sun. Update the whole `LightingEnvironment` component in one
guarded transaction to keep its sky and atmosphere settings together.

```json
{
  "base_height_m": 2000,
  "thickness_m": 1600,
  "coverage": 0.55,
  "extinction_per_km": 3.5,
  "wind_m_per_s": [8, 2]
}
```

| Field | Effect |
| --- | --- |
| `base_height_m` | Cloud base from 250 to 12,000 metres above `sea_level_m`. |
| `thickness_m` | Layer depth from 100 to 6,000 metres. |
| `coverage` | Fractional control from 0 to 1 over which parts of the density field become visible. |
| `extinction_per_km` | Optical density multiplier from 0 to 32 per kilometre. |
| `wind_m_per_s` | Horizontal cloud motion along world X and Z, from −100 to 100 metres per second on each axis. |

Omit the profile to use the atmospheric sky by itself. Coverage or extinction
of zero also clears the layer. When the atmospheric sky is inactive, clouds
fall back with it. All five fields are required when the profile is present.

## Motion and light

Drive the scene with [gameplay solar lighting](WEATHER_CYCLE.md#drive-the-sun-and-sky)
to move the layer. The committed `world_milliseconds` sample controls motion
through the authored wind. Capturing the same saved world time and camera
reuses the same position in the cloud density field. Authored previews
without a solar sample show time zero. The committed sun supplies direction,
tint, and atmospheric light gain. The cloud profile controls its own coverage
and wind.

The renderer bakes a periodic density field once into a 64 × 64 × 64 RGBA8
texture array. The horizontal noise repeats every 64 km. Each active frame
integrates light and transmittance along
192 × 108 unjittered view rays through the layer. The ray march takes up to
24 view samples and three sunlight samples per contributing view sample.
Density fades between 36 and 48 km along the ray. The RGBA16F view stores
premultiplied cloud light and transmittance. The two textures use 1,214,464
logical bytes in total, including the retained density field.

Clouds appear in the sky draw. Opaque scene geometry can occlude the sky
normally. The layer's internal illumination includes self-shadowing. Its
scope is background composition, with full depth clipping for flying into
clouds and moving cloud shadows on terrain requiring additional passes.
Forward color supports 1× and 4× MSAA. Deferred color uses 1×. With FSR active,
background pixels receive a reactive signal to reduce accumulation of moving
cloud edges. Opaque-surface history keeps its regular behavior.

## Inspect and verify

Use `world.describe` for field bounds. Inspect `world.lighting` or
`runtime.lighting` for the authored profile. On a retired frame,
`render_diagnostics.volumetric_clouds` reports `active`, `reason`,
`noise_bytes`, `view_bytes` and `world_milliseconds`. `profile: true`
reports available GPU timing for the cloud view integration.
`world.dependencies`, project inspection and game inspection report
`needs_volumetric_clouds`. Export with an installed Vulkan runtime that
advertises `features.volumetric_clouds`.

Run the [cloud fixture](../tests/volumetric_cloud_capture.py) to check authoring
and save restoration on a simulation build. Add `--gpu GPU_INDEX` for Windows
Vulkan captures. The script's `--help` lists options for deferred rendering,
MSAA, FSR and a relocated Native AOT replay.

The bounded ray-march design draws on published work on grouping clouds
into a height layer, using procedural 3D noise and sampling light within the
volume. Guerrilla's later voxel work shows the extra acceleration and data
needed for detailed cloud interiors. [Loboda et al. 2025](https://erk.fe.uni-lj.si/2025/papers/loboda%28real_time_volumetric%29.pdf)
[Guerrilla's Nubis³](https://www.guerrilla-games.com/read/nubis-cubed)
