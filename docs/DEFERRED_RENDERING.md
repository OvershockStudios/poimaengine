# Deferred rendering

Poima has an optional, single-sample deferred opaque path. Forward rendering remains the default and supports MSAA. Both paths use the same material evaluation and direct-lighting functions; deferred rendering is not a separate material system.

Bounded integration checks pass on the AMD integrated GPU and NVIDIA RTX 4070 Laptop GPU. [Recorded evidence](evidence/m2-deferred-rendering.json) describes each cohort. These checks do not establish a performance advantage, introduce global illumination, or resolve the experimental FSR path's documented image-quality limitations.

## Select the path

World, runtime and animation capture requests accept:

```json
{"lighting_path":"deferred","samples":1}
```

Add these fields to the normal capture parameters, including camera, revision or runtime tick, and output path. `runtime.play` accepts the same selection. Invalid combinations reject without advancing authoritative state. Omitting `lighting_path` selects `forward`; deferred rendering requires explicit single-sample selection when the request's default would be MSAA.

Native clients set `RenderOptions::deferred` and `samples = 1`. There is no desktop preference for this path yet. See the [world service contract](WORLD_SERVICE.md) for complete request schemas.

## Screen-space reflections

Set `LightingEnvironment.screen_reflections` in a guarded `component.set`
operation. All three settings are required when selected.

```json
{"screen_reflections":{"strength":0.65,"max_distance_m":12,"thickness_m":0.125}}
```

Strength is a blend factor from 0 to 1. The trace distance is 0.25 to 32
metres and the depth hit tolerance is 0.01 to 0.5 metres. Authored strength
zero keeps the selected settings and disables the effect.

Use single-sample deferred color with reconstruction disabled. Reflections
sample visible opaque scene radiance. Each ray uses up to 32 coarse samples
with four refinements per candidate depth crossing. Rougher surfaces fade
out of the effect between roughness 0.20 and 0.45. Rays that miss retain
the existing lighting.

Deferred lighting writes the reflector roughness and the original opaque HDR.
The reflection pass reads both and writes a separate HDR image. Sprites and
separately composited viewmodels draw afterward, followed by post-processing
and UI. The two added images use 10 logical bytes per pixel, capped at
128 MiB. Capture probes report the opaque input as `raw_hdr` and the
reflected world image as `resolved_hdr`.

`world.lighting`, `runtime.lighting` and capture lighting report the authored
selection. When selected, `render_diagnostics.screen_reflections` reports
`active`, `reason`, `buffer_bytes` and available GPU timing. Forward
rendering, FSR reconstruction, diagnostic views and world-depth foregrounds
use their existing lighting.

Headless authoring can validate the settings and resolve the export closure.
World dependencies and project and bundle inspection report
`needs_screen_reflections` for a selected profile. Build and bundle verification
require an installed Vulkan runtime with `features.screen_reflections`.

Use `tests/screen_reflections_capture.py POIMA --runtime-root RUNTIME
--output DIRECTORY --gpu GPU_INDEX` to check relocated exports, an independent
geometric and radiance witness, and disabled and fallback comparisons. Add
`--fsr` for an installed build with FSR and `--windows-interop` from WSL.

## Storage and execution

Opaque rasterization writes base color/metallic, octahedrally encoded shading normal/occlusion/flags, emission/roughness, and octahedrally encoded geometric normals. A compute pass reconstructs world position from depth, evaluates shared lighting, and writes HDR radiance. Background pixels retain the clear or procedural sky. GTAO and shadow bias use geometric normals at the same precision in both storage layouts.

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
