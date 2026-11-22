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

The table split follows the potential for real-time transmittance and sky-view
lookups described by Hillaire. The optical coefficients and sphere geometry
follow the Earth case in Bruneton's reference. The 2024 Mie-theory model
describes additional work required for other planets and aerosol
compositions. [Hillaire 2020](https://diglib.eg.org/server/api/core/bitstreams/f17c98cb-60c8-4a58-b459-181dd009ef4e/content)
[Bruneton's reference](https://ebruneton.github.io/precomputed_atmospheric_scattering/index.html)
[Schneegans et al. 2024](https://elib.dlr.de/204028/)
