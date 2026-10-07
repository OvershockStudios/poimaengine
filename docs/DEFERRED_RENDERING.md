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

## Storage and execution

Opaque rasterization writes base color/metallic, octahedrally encoded shading normal/occlusion/flags, emission/roughness, and motion/geometric normal. A compute pass reconstructs world position from depth, evaluates shared lighting, and writes HDR radiance. Background pixels retain the clear or procedural sky. Geometric normals remain distinct from shading normals for shadow bias.

The compute pass also writes the existing expanded normal, motion and validity products used by inspection and reconstruction. Exposure, output conversion and game UI composition follow lighting; optional FSR reconstruction consumes HDR and the expanded products before output composition.

Additional material buffers cost **32 bytes per render pixel**, excluding the reused HDR image, depth and existing expanded products. At 1920×1080 that is 66,355,200 bytes, about 63.3 MiB. Reconstruction uses its internal render extent for these buffers. This is logical image payload, not measured total VRAM consumption. Expanded products are currently allocated even when no inspection or reconstruction requests them.

## Diagnostics and limits

`render_diagnostics.lighting_path` identifies the actual path. `deferred_buffer_bytes` reports the additional material storage. `deferred_lighting_gpu` measures the lighting dispatch where GPU timestamps are available; it is contained within the existing post interval and must not be added to that interval a second time.

The path requires a rigid camera transform and supported storage-image formats. Authored color/emission values that exceed half-float storage range are rejected. It supports neither deferred MSAA nor a new transparency pipeline. Forward rendering remains available for comparison and supported MSAA workflows.

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
