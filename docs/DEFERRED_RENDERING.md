# Deferred rendering

Poima has an optional, single-sample deferred opaque path. Forward rendering remains the default and supports MSAA. Both paths use the same material evaluation and direct-lighting functions; deferred rendering is not a separate material system.

Bounded integration checks pass on the AMD integrated GPU and NVIDIA RTX 4070 Laptop GPU. [Recorded evidence](evidence/m2-deferred-rendering.json) describes each cohort. These checks do not establish a performance advantage, introduce global illumination, or resolve the experimental FSR path's documented image-quality limitations.

## Select the path

World, runtime and animation capture requests accept:

```json
{"lighting_path":"deferred","samples":1}
```

Add these fields to the normal capture parameters, including camera, revision or runtime tick, and output path. `runtime.play` accepts the same selection. Invalid combinations reject without advancing authoritative state. Omitting `lighting_path` selects `forward`; deferred rendering requires explicit single-sample selection when the request's default would be MSAA.

For a standalone game, pass the lighting path to the bundle's launcher.

```text
MyRoom-game\launch.cmd --lighting-path deferred
```

Both interactive launch and `--replay` accept the option. The standalone
launch supplies one world sample for deferred rendering, overriding stored
and session sample preferences. A conflicting `--samples 4` is rejected
before opening the bundle. Read `poima schema game` for the CLI options.

Native rendering callers set `RenderOptions::deferred` and `samples = 1`.
For a standalone launch through `run_game`, set
`GameLaunchOptions::render.deferred`. The same single-sample policy applies,
and an explicit sample count must be 1. See the
[world service contract](WORLD_SERVICE.md) for the capture and play request schemas.

## Single-sample antialiasing

Use `--post-aa fxaa` to smooth visible edges in a standalone game or replay.

```text
MyRoom-game\launch.cmd --lighting-path deferred --post-aa fxaa
```

Standalone FXAA supplies one world sample, overriding stored or session
sample preferences. An explicit `--samples 4` conflicts with the selection.
For world, runtime and animation captures or live player start, request:

```json
{"post_aa":"fxaa","samples":1,"lighting_path":"deferred","reconstruction":"none"}
```

The default `post_aa` is `none`. FXAA accepts forward or deferred color,
one world sample and native resolution with reconstruction disabled.
Foreground samples are selected independently. The filter uses luminance
contrast and neighboring pixels in the current frame. It runs after tone
mapping and world/viewmodel composition, then the engine draws UI and editor
overlays. [FXAA method](https://developer.download.nvidia.com/assets/gamedev/files/sdk/11/FXAA_WhitePaper.pdf)

`render_diagnostics.post_aa` reports active FXAA and its intermediate
texture payload of four bytes per output pixel, up to 64 MiB. Depth,
normals, motion and raw/resolved HDR probes describe the scene before
the display filter. A color capture includes the filtered scene and UI.

## Local diffuse bounce

Single-sample deferred color can gather bounded first-bounce light from
visible lit and emissive surfaces. Its output feeds reflections and the world
image used by media and reconstruction. [Author and verify the bounce](DIFFUSE_BOUNCE.md).

## Screen-space reflections

Set `LightingEnvironment.screen_reflections` in a guarded `component.set`
operation. All three settings are required when selected.

```json
{"screen_reflections":{"strength":0.65,"max_distance_m":12,"thickness_m":0.125}}
```

Strength is a blend factor from 0 to 1. The trace distance is 0.25 to 32
metres and the depth hit tolerance is 0.01 to 0.5 metres. Authored strength
zero keeps the selected settings and disables the effect.

Use single-sample deferred color, with optional [FSR reconstruction](RECONSTRUCTION.md).
Reflections sample visible opaque scene radiance. Each ray uses up to 32 coarse samples
with four refinements per candidate depth crossing. Rougher surfaces fade
out of the effect between roughness 0.20 and 0.45. Rays that miss retain
the existing lighting.

Deferred lighting writes the reflector roughness and the original opaque HDR.
The reflection pass reads both and writes a separate HDR image. Optional
[height fog](HEIGHT_FOG.md) then scatters light over the reflected color.
With FSR enabled, the engine reconstructs that HDR result before drawing
composited viewmodels and UI. Without FSR, sprites can draw over the HDR
world before viewmodels. The two reflection images use 10 logical bytes
per render pixel, capped at 128 MiB. Capture probes report the opaque input
as `raw_hdr` and the resulting world color as `resolved_hdr`, at the
reconstruction output coordinate when FSR is active.

`world.lighting`, `runtime.lighting` and capture lighting report the authored
selection. When selected, `render_diagnostics.screen_reflections` reports
`active`, `reason`, `buffer_bytes` and available GPU timing. Forward
rendering, diagnostic views and world-depth foregrounds use their existing
lighting.

Headless authoring can validate the settings and resolve the export closure.
World dependencies and project and bundle inspection report
`needs_screen_reflections` for a selected profile. Build and bundle verification
require an installed Vulkan runtime with `features.screen_reflections`.

Use `tests/screen_reflections_capture.py POIMA --runtime-root RUNTIME
--output DIRECTORY --gpu GPU_INDEX` to check relocated exports, an independent
geometric and radiance witness, and disabled and fallback comparisons. Add
`--windows-interop` from WSL. Use the [composition check](RECONSTRUCTION.md#world-lighting-composition)
to test the FSR path.

## Storage and execution

Opaque rasterization writes base color/metallic, octahedrally encoded shading normal/occlusion/flags, emission/roughness, and octahedrally encoded geometric normals. A compute pass reconstructs world position from depth, evaluates shared lighting, and writes HDR radiance. Background pixels retain the clear or procedural sky. GTAO and shadow bias use geometric normals at the same precision in both storage layouts.

The base-color attachment's fourth channel holds metallic in `[0, 1]` or
negative [diffuse transmission](FOLIAGE_SCATTER.md#light-leaves-from-behind)
for a thin dielectric. Material consumers decode a negative value as
metallic zero. The screen-space diffuse bounce receiver reduces its reflected
diffuse budget by the transmitted fraction. Attachment formats and counts
stay the same.

Ordinary color playback without numeric scene-product probes or FSR skips previous-position raster inputs and the expanded normal, motion and validity attachments. Color screenshots are still available during that playback. Standalone captures, diagnostic playback, playback with probes and FSR use the full layout. In the full layout, opaque rasterization also writes motion and the compute pass expands the normal, motion and validity products before inspection or reconstruction. Exposure, output conversion and game UI composition follow lighting.

Additional material buffers cost **24 bytes per render pixel** for ordinary color playback and **32 bytes** for the full layout. The expanded products of the full layout use another **17 bytes per pixel**. At 1920×1080 ordinary color playback uses **51,840,000 fewer logical image bytes** (49.4 MiB). These counts exclude reused HDR and depth, and measure texel payload. Reconstruction uses its internal render extent for the full layout.

## Diagnostics and limits

`render_diagnostics.lighting_path` identifies the actual path. `deferred_buffer_bytes` reports the additional material storage. Check `scene_products.available` and `scene_products.motion_available` for expanded outputs. In ordinary deferred color playback both flags are false, their byte counts are zero and `history_reset_reason` is `products_unavailable`. `deferred_lighting_gpu` measures the lighting dispatch where GPU timestamps are available. It is contained within the existing post interval.

The path requires a rigid camera transform and supported storage-image formats. Scene rendering enables Vulkan's `shaderDemoteToHelperInvocation` for fragment alpha cutouts and selects a device that exposes it. Authored color and emission values must fit the half-float storage range. Deferred rendering uses one sample per pixel. Forward rendering supports MSAA.

Tests cover separate concerns: analytic radiance and forward comparisons; render-graph resource contracts; reconstruction lifecycle; UI composition; lighting/shadows; skinning and motion. Passing one concern does not qualify the others. Published implementation status and evidence, rather than the existence of a fixture, determine which workflows have been qualified.

## Reproduce the checks

Use the Windows renderer build described in [BUILD.md](BUILD.md). Build the `poima-deferred-test`, `poima-reconstruction-test`, `poima-ui-capture-test` and `poima-scene-motion-test` targets alongside `poima`. Reconstruction tests require the optional SDK build. Run native fixtures with an existing output directory and the desired GPU index:

```text
poima-deferred-test.exe OUTPUT_PREFIX GPU_INDEX
poima-reconstruction-test.exe OUTPUT_PREFIX GPU_INDEX deferred
poima-scene-motion-test.exe OUTPUT_PREFIX GPU_INDEX deferred
```

The Python suites `lighting_capture.py`, `shadow_capture.py`, `clustered_lighting_capture.py`, `sky_capture.py` and `gpu_skinning_capture.py` accept the engine executable followed by `--output DIRECTORY --gpu GPU_INDEX --lighting-path deferred`. `reconstruction_ui_capture.py` takes the UI fixture executable instead. Use Windows Python for native Windows paths, or the suites' `--windows-interop` option from WSL.

For standalone launch, publish the [item contact fixture](../tests/fixtures/item_attachment_contact/README.md)
as Windows x64 Native AOT, then run this check with a matching installed
Windows runtime. The fixture writes an external checkpoint on the first
committed tick. The check compares its bytes across forward, deferred and
headless runs from a relocated export.

```sh
python3 tests/game_launch_lighting.py "$runtime/bin/poima.exe" \
  --runtime-root "$runtime" --artifact "$contact/native-gameplay.json" \
  --output build/standalone-lighting-check --gpu 0 --windows-interop
```

Add `--fxaa` and use a new output directory to check the filter with a
diagonal edge, UI controls, fog and reflections in the same scene.

Enable the installed Khronos validation layer and synchronization validation when reproducing GPU qualification. Preserve stdout, stderr and generated evidence; a successful image comparison does not excuse a validation error. Run each suite separately on each device and verify its reported GPU name. Headless contract tests and `poima-render-schedule-test` provide separate CPU-side checks.

Run `tests/scene_products_capture.py POIMA --output DIRECTORY --gpu GPU_INDEX --deferred-player-color` to compare replayed color pixels with and without numerical probes, including both GTAO settings. It also checks the available products and bytes, valid surface and motion samples, and the shading-normal debug view.

## Color playback qualification — 0.0.281

The [qualification record](evidence/m2-deferred-color-281.json) covers Windows
Vulkan on the AMD integrated GPU and RTX 4070 Laptop GPU. It combines standalone
inspection captures, replays with and without numeric probes, native motion
and FSR lifecycle checks, and the Performance Yard workload. All 60 paired
inspection captures match byte-for-byte, including partial workgroups and
rotated cameras. Each adapter also matches its unprobed and probed color
replays with GTAO enabled and disabled. Separate
323×243 replay checks use Khronos synchronization validation with GTAO enabled
and disabled. Both reference adapters report the missing shader-demotion
feature and the candidate enables it. The validation replays match the
reference images exactly.

The performance comparison uses sequential nonfocusable 1080p game runs with
medium GTAO, 30 seconds of warm-up and 60 measured seconds. GPU durations
measure submitted work on the graphics queue. The focused Alpha workload and
GPU residency have separate acceptance checks.

| Adapter | Opaque p95, 0.0.280 → 0.0.281 | Total GPU p95, 0.0.280 → 0.0.281 |
| --- | --- | --- |
| AMD Radeon integrated | 7.322 → 4.935 ms | 25.224 → 21.748 ms |
| RTX 4070 Laptop | 1.566 → 1.162 ms | 6.876 → 6.760 ms |

Both builds complete three laps at about 60 simulation ticks per second
on each adapter. The initial paused screenshots in the background runs
match their references exactly.
