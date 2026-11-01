# Procedural-sky environment lighting

`LightingEnvironment.ibl` enables diffuse sky illumination and roughness-dependent
environment reflections in the shared Vulkan renderer. Forward, deferred opaque
and first-person foreground materials use the same environment response.

This profile convolves the procedural sky's color gradient. Local geometry does
not appear in reflections or contribute bounced light. Local probes, screen-space
reflections and geometry-aware global illumination remain separate work.

## Authoring

Set the environment component through an ordinary `world.transact` operation:

```json
{
  "op": "component.set",
  "id": "00000000000000000000000000000001",
  "type": "LightingEnvironment",
  "value": {
    "ambient": [0, 0, 0],
    "exposure": 1,
    "sky": {
      "enabled": true,
      "zenith": [0.06, 0.22, 0.55],
      "horizon": [0.55, 0.70, 0.85],
      "ground": [0.12, 0.10, 0.08],
      "horizon_falloff": 0.35,
      "sun": null,
      "sun_size_degrees": 0.53,
      "sun_intensity": 20
    },
    "ibl": {
      "enabled": true,
      "diffuse_intensity": 1,
      "specular_intensity": 1
    }
  }
}
```

The entity must already exist. Discover its complete mutation contract with
`world.describe`, selecting `component.set` and `LightingEnvironment`.

| Field | Meaning |
| --- | --- |
| `enabled` | Required Boolean when `ibl` is present. Enabling requires an enabled procedural sky. |
| `diffuse_intensity` | Required finite gain in `[0,64]` for the convolved diffuse contribution. |
| `specular_intensity` | Required finite gain in `[0,64]` for the reflected contribution. |

Unknown fields and invalid types/ranges are rejected even when IBL is disabled.
Omitting `ibl` preserves the existing lighting behavior and omitted inspection
shape. Explicit disabled and zero-gain values retain their authored presence.
History, frozen runtime lighting and complete game saves preserve that distinction.

Sky colors are linear RGB. Environment exposure is applied once to the combined
lighting. Existing ambient fill is additive; the example sets it to zero to show
the environment contribution independently. The Sun disk and halo are excluded
from convolution. Use an ordinary directional light for direct sunlight.

## Shading and resources

Diffuse convolution stores irradiance divided by pi. The material applies its
diffuse color, metallic exclusion and Fresnel weighting. Specular lighting uses a
GGX roughness-prefiltered cube and correlated-Smith split-sum BRDF lookup.
Material and screen-space ambient visibility attenuate indirect lighting once.
Emission and direct lights retain their existing paths.

The fixed resource profile is:

| Resource | Dimensions | Format |
| --- | --- | --- |
| Diffuse cube | Six 16 × 16 faces | RGBA32F |
| Specular cube | Six 64 × 64 faces, seven mip levels | RGBA32F |
| BRDF lookup | 64 × 64 | RG32F |

Its texel payload is **581,600 bytes**, excluding driver allocations and staging.
Enabled IBL requires linear sampling support for those floating-point formats.
Disabled rendering uses three tiny normalized-format dummy images with a
52-byte texel payload, without imposing the floating-point sampling requirement.

The renderer retains one completed profile. A changed zenith, horizon, ground or
falloff synchronously rebakes and uploads it before the next affected frame.
The old consumers retire before resources or bindings are replaced. Staging is
bounded to two full profiles; disabling releases the full profile.

Camera movement, exposure, gains and Sun metadata reuse the current convolution.
Re-enabling after release creates a new generation. Profile rebuilding currently
also recomputes the sky-independent BRDF lookup. Continuous animated sky colors
need further preparation/caching work; this is not an asynchronous day/night GI
system.

## Observation and checks

Render diagnostics include `environment_ibl` when the authored option is present:
`enabled`, texture dimensions, `texture_bytes`, `generation` and `prepare_cpu_ms`.
Preparation time covers CPU convolution; it excludes upload and retirement waits.
A disabled profile reports zero full-profile bytes/dimensions and retains the
generation counter. Omitted authoring omits this diagnostic.

The native bake test independently checks diffuse integration, GGX/Smith
integration, constant environments, face orientation, roughness, complete
serial/worker equality and bounded executor recovery. The CLI contract checks
validation, focused discovery, atomic rejection, optional presence, history,
frozen settings and complete save restoration. Run the registered
`environment_ibl_native` and `environment_ibl_contract` tests in a configured
headless build.

The Windows capture fixture additionally tests independent radiance oracles,
whole-image controls, profile replacement/release and foreground MSAA. Its
hardware execution is recorded separately from headless checks. Consult
[implementation status](IMPLEMENTATION_STATUS.md) for the qualified checkpoint.

[Checkpoint 0.0.217](evidence/m2-environment-ibl.json) qualifies 162 actual Vulkan
captures across forward 1×/4× and deferred 1× on both laptop GPUs. These include
102 independent radiance oracles and 54 complete-image controls. A separate
read-only review compares 64 retained images and their complete capture records
with the original 0.0.211 results. The original monitoring workflow remains
failed after an optional-field lookup error; both native producers passed.
The earlier 18-capture identity failure also remains recorded.

These controlled surfaces establish bounded correctness, not representative
game performance or general scene-reflection quality. Concurrent Unity work
was present during qualification; timings support no controlled performance
comparison. Dedicated IBL Inspector controls remain additional editor work.
