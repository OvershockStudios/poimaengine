<!-- SPDX-License-Identifier: Apache-2.0 -->
# Height fog

Set `LightingEnvironment.height_fog` through a guarded world transaction.
The profile applies to the world while camera position, opaque depth and
live light poses determine what each view sees.

```json
{
  "density_per_m": 0.07,
  "base_height_m": 0.4,
  "height_falloff_per_m": 0.16,
  "start_distance_m": 0.8,
  "max_distance_m": 90,
  "ambient_radiance": [0.12, 0.16, 0.24],
  "anisotropy": 0.35,
  "spot_lights": [
    {"entity": "00000000000000000000000000000005", "scattering": 0.5}
  ]
}
```

Every field except `spot_lights` is required. Replace the example ID with
an authored spot Light. Up to two distinct spotlights can be selected.
Each scattering gain is in `[0, 1]`. An enabled light with positive gain and
intensity contributes at its current position and orientation. A disabled
light retains its authored reference so it can be enabled in a later
revision. In a spawn graph, use local spotlight IDs. Spawn remaps those
references along with the lights.

Density in `[0, 0.75]` is measured per metre at and below the base.
Above it, density falls exponentially with the height falloff in
`[0, 4]` per metre. Base height is a world Y coordinate in
`[-8192, 8192]`. Start distance accepts `[0, 300]` metres. The end cap
accepts `[0.1, 512]` metres and must exceed the start. The three ambient
radiance channels accept `[0, 32]` and use the same scene-linear space as
the opaque scene. Anisotropy in `[-0.8, 0.8]` controls the spotlight phase
function, with zero giving equal scattering in each direction.

The shader integrates extinction along the actual sightline to geometry
or the authored sky distance cap. It attenuates the input color by
transmittance and uses the remaining weight for ambient in-scattering.
Each contributing spotlight uses 12 midpoint samples along the part of
the sightline within its range. Each interval has analytically integrated
extinction. The spot cone, inverse-square falloff and nearest shadow-map
comparison determine the light available at the sample. [Analytic height fog
in Decima](https://www.guerrilla-games.com/media/News/Files/DecimaSiggraph2017.pdf)

## Rendering and inspection

Use forward or deferred color at one sample. [FSR reconstruction](RECONSTRUCTION.md)
can consume this color at the internal render extent.
Height fog reads opaque depth and scene-linear color after optional screen
reflections. It writes a separate world image before sprites, a separately
composited foreground, post-processing and the HUD. This route uses the
opaque depth surface. Sprites and composited foreground retain their own
blend and shading behavior.

When [aerial perspective](ATMOSPHERE_SKY.md#aerial-perspective) is active, it
uses the same compositor and height fog applies over the atmospheric haze.
The height-fog GPU timing then covers building the aerial tables and
compositing both effects.

`world.lighting`, `runtime.lighting` and the capture lighting receipt
report the selected profile. `render_diagnostics.height_fog` reports
`active`, `reason`, `buffer_bytes`, effective `spot_lights` count, and
available GPU timing. The extra target uses eight logical bytes per
render pixel with a 96 MiB cap. A selected density of zero has identity output.
Multisample, diagnostic views, a hidden scene, a non-rigid camera and
world-depth foreground use the original color route. The
inactive diagnostic gives the applicable reason.

Sparse probe `raw_hdr` is the original lit opaque scene. `resolved_hdr`
reports the world image after fog at the reconstructed output coordinate
when FSR is active. The native-resolution path includes sprites. Both
reports precede composited foreground and post-processing. The selected profile freezes with the
runtime and its authored dependency identity.

Project and standalone inspection report `needs_height_fog`. Use an
installed Vulkan runtime with `features.height_fog` to build and verify an
export containing the profile.

Run the native contract on a simulation build and the capture fixture on
an installed Windows Vulkan build.

```sh
python3 tests/height_fog_contract.py POIMA --output DIRECTORY
python3 tests/height_fog_capture.py POIMA --runtime-root RUNTIME \
    --output DIRECTORY --gpu GPU_INDEX --lighting-path forward --spot
```

Use `--lighting-path deferred` for the deferred path and `--windows-interop`
from WSL. `--msaa` exercises the multisample fallback. The
[composition check](RECONSTRUCTION.md#world-lighting-composition) exercises fog with FSR.
