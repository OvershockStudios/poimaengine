# HDR bloom

Bloom softens bright scene highlights. Add this optional object to the scene's
`LightingEnvironment` component to enable it.

```json
{"bloom":{"intensity":0.35,"threshold":1.0,"radius":12.0}}
```

| Field | Range | Default | Meaning |
| --- | --- | --- | --- |
| `intensity` | 0–1 | 0 | Scales the bloom radiance. Zero disables every bloom pass. |
| `threshold` | 0–65,504 | 1 | Peak RGB in scene-linear units. Each source pixel contributes the portion above this threshold, preserving its color. |
| `radius` | 2–32 | 8 | Distance of the furthest filter tap, in pixels of the resolved color image. Prefiltering and upsampling add a small footprint. |

Use `world.describe` to discover the active `LightingEnvironment` schema. Use
`world.transact` with the current revision and the complete component value to
edit it. A scene without an authored `bloom` object uses the default output
pipeline. The component applies to scene capture and to game and editor views
that render that scene.

The Vulkan path runs after world MSAA resolve, temporal reconstruction when
selected, the foreground viewmodel composite when enabled, and any authored
[project post effect](PROJECT_POST_EFFECTS.md). It thresholds
the final HDR radiance and averages each nonoverlapping 4×4 source block,
including the valid texels of partial edge blocks. Two separable five-tap blur
passes use that reduced image. The output shader adds its filtered signal in
linear space, then applies the existing scene exposure, Reinhard tone map and
sRGB encoding. The renderer draws the HUD and overlays afterward.

Enabled bloom requires linear sampling of RGBA16_FLOAT. Two reusable
quarter-width, quarter-height RGBA16_FLOAT targets together are bounded to
32 MiB per renderer context. Debug views use their diagnostic display path.
Bloom does not enter reconstruction history or raw scene-radiance probes.

## Verify

The render schedule tests check the HDR producer, pass ordering, attachment
ownership and the default path with bloom omitted. The capture test below
checks a colored halo in real Vulkan output, a finite footprint, threshold
and zero-intensity behavior, and zero exposure. It requires a rebuilt native
renderer and graphics session. It records captures and exact RPC results in a
new directory under `--output`.

```sh
python3 tests/bloom_capture.py build/windows-runtime/poima.exe --output build/bloom --gpu 0 --windows-interop
```
