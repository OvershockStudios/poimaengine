# Atmospheric sky

Add `atmosphere` to a world's `LightingEnvironment` to render an Earth-like
sky with Rayleigh scattering, aerosol scattering and ozone absorption. Enable
the existing procedural sky and link its `sun` to an authored directional
light. Use one atomic component edit to set the whole environment. The example
adds an atmosphere above world height zero.

```json
{
  "sea_level_m": 0,
  "aerosol_strength": 1,
  "radiance_scale": 4
}
```

| Field | Range and effect |
| --- | --- |
| `sea_level_m` | Finite world Y from −1,000,000 to 1,000,000 m. Observer height is camera Y minus sea level, clamped to 0–20,000 m. |
| `aerosol_strength` | Finite multiplier from 0 to 4 for aerosol scattering and extinction. |
| `radiance_scale` | Finite multiplier from 0 to 64 for the scattered background. Zero selects the gradient and disk. |

An omitted profile, a zero radiance scale or a disabled linked sun selects the
legacy gradient. With the profile active, the gradient gives artists an
additive base color. A dark palette leaves most of the daytime color to the
scattered light. The sun disk keeps its authored diameter and intensity and
receives colored atmospheric attenuation. The model uses a 6,371 km ground
sphere, a 100 km atmosphere shell, and the camera's local +Y as radial up.
A sun below the observer's horizon can still illuminate higher air and produce
twilight.

[Game-driven solar lighting](WEATHER_CYCLE.md#drive-the-sun-and-sky) supplies
the committed sun direction, tint and sky gain. The directional lux and disk
can fade to zero while the higher atmosphere still receives sunlight. Exposure
and post effects act on the combined scene through the normal HDR path.
The existing [environment lighting](ENVIRONMENT_IBL.md) and
[height fog](HEIGHT_FOG.md) provide the separate material reflection and
in-scene fog controls.
[Volumetric clouds](VOLUMETRIC_CLOUDS.md) can use the same atmosphere and
committed sunlight and world time.

The lookup passes store 160 KiB of texels. A 128 × 32 RGBA32F table holds
optical-density columns and builds once per renderer. A 128 × 96 RGBA16F
table integrates single scattering in 24 segments per view direction. It
updates when observer height, sun elevation or aerosol strength changes.
Camera rotation, horizontal travel, the sun's azimuth, tint and exposure
reuse the table. Forward 1× and 4× MSAA and deferred 1× use the same tables.
Both formats require linear sampling and storage-image writes.

Inspect `LightingEnvironment.atmosphere` through `world.describe`. Authoring
inspection and runtime lighting report the profile. On a completed Vulkan
frame, `render_diagnostics.atmosphere_sky` reports both generations, texel
bytes and the clamped observer height. The runtime descriptor advertises
`features.atmosphere_sky` for standalone export.

The [atmosphere fixture](../tests/atmosphere_capture.py) checks authored
profiles, save restoration and Vulkan captures against independent numerical
integration. Its Native AOT replay checks cache reuse as the sun moves. Run the
script with `--help` for headless, capture and relocated-export options.

## Planet-centered sky

For a curved planet, specify the center in world metres.

```json
{
  "planet_center_m": [0, -6771000, 0],
  "aerosol_strength": 0.82,
  "radiance_scale": 4
}
```

This form uses the same Earth-sized reference sphere and atmosphere shell.
At world origin in the example, the observer is 400 km above the surface.
The renderer measures altitude from the center and takes the outward
direction as local up. Cameras and the linked sun retain their world
transforms. Use the sea-level form for the current cloud and aerial
perspective paths. The authored center takes the place of `sea_level_m`.

Exterior rays contribute scattering over the portion of their path inside
the atmosphere. The sky-view table separates ground intersections from
grazing paths through air, with closer samples near the limb. The same table
provides a scalar transmission estimate for the sun disk. An authored mesh
can supply the visible planet surface. This sky pass renders behind opaque
geometry, including planetary surfaces.

The `planet_center_m` components must each be finite and within ±1 billion
metres. For a camera below the reference surface, the optical altitude is
zero. Render diagnostics report the selected center and optical altitude.
The installed runtime advertises `features.orbital_atmosphere`, and project
inspection reports `needs_orbital_atmosphere`. Use the
[orbital fixture](../tests/orbital_atmosphere_capture.py) to check admission,
save continuation and native Vulkan views at multiple altitudes.

## Aerial perspective

Set `aerial_perspective_m` in the same `atmosphere` object to add distance haze
to opaque geometry. For example, choose `12000` to integrate up to 12 km along
each camera ray. The optional field accepts finite values from 128 to 65,536
metres. At greater distances, surfaces use the optical result at the cap.
The haze follows the same observer height, aerosol strength and committed sun
as the atmospheric sky. Surfaces look lighter or darker depending on the
balance of in-scattered light and the attenuation of their own color.

Each active frame builds two 32 × 18 × 24 RGBA16F tables that store RGB
scattered light and transmittance. The tables use 216 KiB together. The
distance slices are denser near the camera. The compositor reads opaque
depth and shares an eight-byte-per-pixel world image with height fog. It
leaves the sky background intact and applies optional local height fog over
the resulting color. Forward and deferred color each support this stage at
one sample. World color is composed before reconstruction, sprites, the
separately composited viewmodel and the HUD.

`render_diagnostics.aerial_perspective` reports `active`, `reason`,
`table_bytes` and `max_distance_m` after a completed frame. The `reason` field
identifies fallback for multisampling, diagnostic views, a hidden scene, a
non-rigid camera, a world-depth foreground or an inactive atmospheric sky.
Project and standalone inspection report `needs_aerial_perspective` when
the distance is authored. Export uses an installed runtime with
`features.aerial_perspective` and `features.atmosphere_sky`.

Use the [aerial perspective fixture](../tests/aerial_perspective_capture.py)
to check authoring headlessly. With `--gpu GPU_INDEX`,
it samples Vulkan HDR at three distances, checks sky and height fog
composition, and can verify a relocated export. Use `--help` for deferred,
multisample and replay options.

## References

The table split follows the potential for real-time transmittance and sky-view
lookups described by Hillaire. The optical coefficients and sphere geometry
follow the Earth case in Bruneton's reference. The 2024 Mie-theory model
describes additional work required for other planets and aerosol
compositions. [Hillaire 2020](https://diglib.eg.org/server/api/core/bitstreams/f17c98cb-60c8-4a58-b459-181dd009ef4e/content)
[Bruneton's reference](https://ebruneton.github.io/precomputed_atmospheric_scattering/index.html)
[Schneegans et al. 2024](https://elib.dlr.de/204028/)
