# Optional FSR reconstruction

**Experimental; bounded integration checks passed.** Bounded capture, API, lifecycle and UI checks have passed on the two recorded laptop GPUs. Overall temporal image quality and performance remain unqualified. See the [qualification record](evidence/m2-reconstruction.json) for the exact scope. The existing renderer's qualification records do not qualify this optional path.

Poima integrates the FSR 3.1.4 upscaler from FidelityFX SDK 1.1.4, pinned to commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`. The initial adapter targets the Windows Vulkan renderer. It is disabled in default builds and in default rendering options. This feature does not provide frame generation or DLSS.

## Build preparation

Follow the existing [Windows renderer build instructions](BUILD.md) first. FSR adds explicit source and shader preparation; CMake does not download the SDK automatically.

Run the preparation script with **Windows Python and Git**, from the checkout. The script fetches the pinned source if the selected source directory does not exist, uses the SDK's official Windows shader tools, and writes a hash manifest. An existing source directory must match the pin and remain unmodified.

```powershell
python scripts/prepare_fsr3.py --source .cache/sources/FidelityFX-SDK-v1.1.4 --output .cache/fsr3-shaders
```

The documented renderer build is hosted on Linux/WSL and targets Windows. Back in that environment, set `FSR_VULKAN_LIBRARY` to an absolute path to the **target Windows Vulkan loader import library**, compatible with the selected toolchain. A Linux Vulkan shared library is not a substitute. Configure the existing renderer preset with the optional inputs:

```sh
cmake --preset windows-render \
  -DPOIMA_ENABLE_FSR3_UPSCALER=ON \
  -DPOIMA_FSR3_SOURCE_DIR="$PWD/.cache/sources/FidelityFX-SDK-v1.1.4" \
  -DPOIMA_FSR3_SHADER_DIR="$PWD/.cache/fsr3-shaders" \
  -DPOIMA_FSR3_VULKAN_LIBRARY="$FSR_VULKAN_LIBRARY"
cmake --build --preset windows-render
```

Use the runtime preset and its existing prerequisites when runtime services are needed. Enabling FSR also requires `POIMA_BUILD_RENDER_SMOKE=ON`. Configuration verifies the source revision and shader manifest. The SDK license is included in installation notices. The compiled copy includes narrowly scoped CPU alignment and GLSL format corrections; the original checkout remains pristine. Generated patch records and the shader manifest bind the source and patch hashes. See [third-party notices](../THIRD_PARTY_NOTICES.md). Shader preparation and adapter compilation are separate from GPU qualification; this guide does not qualify native Windows-host MSVC builds.

Without `POIMA_ENABLE_FSR3_UPSCALER`, an explicit reconstruction request fails. Unsupported device capabilities also fail explicitly; an enabled request does not silently become ordinary rendering.

## Rendering options

The capture services and `runtime.play` accept `reconstruction`:

| Value | Scene render size relative to reconstruction output |
| --- | --- |
| `none` | Existing rendering path; default |
| `fsr3_native` | Native AA, equal render and output dimensions |
| `fsr3_quality` | Each output dimension divided by 1.5 |
| `fsr3_balanced` | Each output dimension divided by 1.7 |
| `fsr3_performance` | Each output dimension divided by 2 |

Render dimensions truncate to positive integer pixels. Reported extents are authoritative. These are fixed ratios; dynamic resolution is not implemented.

Enabled reconstruction requires `samples: 1`, `scene_debug_view: "color"`, and a scene with presentation continuity identity. Non-color debug views retain their separate, unjittered inspection path and cannot be combined with reconstruction. Sharpening is disabled in this initial adapter. UI renders at the output/window resolution after scene reconstruction and display mapping.

For example, these are additional options for a capture request; the selected service still requires its usual revision/session, camera and path fields:

```json
{
  "samples": 1,
  "scene_debug_view": "color",
  "reconstruction": "fsr3_quality",
  "capture_frames": 32,
  "scene_product_probes": [{"x": 40, "y": 40}],
  "profile": true
}
```

`capture_frames` accepts integers from 1 through 128 and defaults to 2. Capture repeatedly renders one frozen scene snapshot; these frames do not advance simulation or animation time. They do advance accepted renderer history and jitter. Explicitly request sufficient frames for the observation being made; the default two frames are not a convergence guarantee. `runtime.play` does not accept `capture_frames`.

Frozen captures and recorded-input replays use a fixed 60 Hz SDK frame interval, independent of disk writes and readback time. Live players, hosted viewports and the legacy editor use elapsed steady-clock time between accepted reconstruction dispatch timestamps, clamped to 1–100 ms. The first dispatch and history resets use the fixed interval. Preparing a dispatch does not advance the clock until submission is accepted; hidden views do not advance it. Capture/readback exclusivity controls synchronization, not timing policy. This interval is an SDK input, not a measured frame-rate claim. Live temporal image quality remains unqualified.

## Coordinates, history and composition

The temporal scene renders into compact images whose origin is `(0,0)`. The reconstructed image has its own output extent. In the legacy editor, that output is composed into the scene's display rectangle; surrounding UI and the swapchain retain their full window extent. An SDK input cannot be represented by merely supplying a smaller size for an arbitrary subrectangle of a larger texture.

Jitter comes from the pinned SDK sequence and advances only with accepted submissions for that view. Projection rasterization is jittered; motion remains `previous unjittered UV - current unjittered UV`, with a top-left origin. History belongs to the view, independently of swapchain image, CPU frame slot and simulation tick. Separate views retain separate SDK contexts and histories.

First use, resize and changes to source identity/generation, camera cut, camera identity, projection or relevant view configuration reset history. Missing object correspondence produces a reactive input rather than being presented as reliable static motion. Previous skin deformation comes from the immediately preceding accepted submission. Hidden/skipped views do not consume temporal submissions. A recording/submission fault invalidates the renderer session; callers must recreate it. Accepted submission does not mean monitor presentation.

A separate input pass supplies background motion for the procedural sky using camera rotation, with translation ignored. Raw surface normals and their validity remain surface products; sky is not assigned a geometric normal. Reactive input marks unavailable correspondence. These inputs need temporal quality testing at silhouettes, newly revealed regions, animated shading and the sun edge. Continuously changing sky or lighting is not treated as a camera cut every frame.

FSR consumes scene-linear HDR before exposure and display mapping. Output exposure and the existing display transform follow reconstruction; game UI, overlays and editor composition follow those operations. Exposure changes therefore do not rescale stored scene radiance. This path does not establish reconstruction coverage for future transparent scene effects or arbitrary texture animation.

## Numeric observations and diagnostics

`scene_product_probes` remains bounded to 64 positions. With reconstruction enabled, `x` and `y` are **render-input pixels**, not window or reconstruction-output pixels. The renderer rejects positions outside the actual render extent. Each capture result includes:

- Existing depth, shading normal, raw geometric motion and validity values, sampled from that frame's jittered raster products.
- `raw_hdr`: scene-linear RGB before reconstruction, exposure and display mapping.
- `resolved_hdr`: reconstructed scene-linear RGB before exposure and display mapping.
- `resolved_x` and `resolved_y`: the output pixel selected by mapping the input pixel center to the output grid.

For one axis, the mapping is `floor((input + 0.5) * output_size / render_size)`. Without reconstruction, raw and resolved HDR reference the same scene radiance. RGB values originate in RGBA16F attachments; JSON conversion does not recover precision absent from those attachments. Sparse probes do not replace full-image quality measurements. BMP captures contain the final display image and UI, not lossless HDR data.

`diagnostics.reconstruction` reports `mode`, `active`, render/output dimensions, `jitter_pixels`, `history_reset`, `history_sequence`, `reset_reason`, `sdk_version`, `logical_bytes` and `gpu`. Completed-frame diagnostics retain the metadata of the accepted submission they describe. `active` means that frame dispatched reconstruction.

`logical_bytes` counts SDK-reported GPU allocation usage plus the engine's additional reconstruction output, dense-motion/reactive inputs and required shared SDK images. It excludes preexisting scene products, swapchain/UI resources and unreported driver overhead; it is not total process VRAM. Allocations and SDK context lifetime are bounded per view. Resize and teardown retire outstanding work before releasing those resources.

With profiling enabled and timestamps supported, `gpu` summarizes the temporal input preparation plus SDK reconstruction interval. It is a GPU queue duration, not presentation latency or total game frame time. This interval is contained within the existing `gpu.post` interval; adding both would double-count reconstruction. This document supplies no timing results or performance targets.

## Qualification boundary

Recorded checks cover strict Vulkan validation, constant HDR radiance, fixed render ratios, cuts, resize, interleaved view histories, public capture APIs and output-resolution UI isolation on AMD Radeon(TM) Graphics and NVIDIA GeForce RTX 4070 Laptop GPU. These bounded fixtures do not establish broad hardware compatibility or game-scale performance. Static convergence alone is insufficient: moving silhouettes, deformation, disocclusion and detail retention still require temporal quality qualification. Keep reconstruction opt-in.

Current diagnostic captures expose persistent color residue near a stationary silhouette after a foreground object moves away. Static edge improvement does not resolve that temporal artifact. Near-edge contamination remains part of the quality assessment even where revealed interiors recover cleanly; it must not be hidden by excluding boundary pixels from the measurements.

The qualification tools separate API behavior, GPU execution and image measurements:

- `tests/reconstruction_contract_capture.py` checks the enabled public capture APIs, probe coordinates, frozen simulation state and rejected requests.
- `poima-reconstruction-test` checks constant HDR radiance, fixed render ratios, cuts, resize and independent view histories across the supported modes.
- `poima-reconstruction-quality-test` records static and moving polygon sequences. A successful run establishes capture completion, not image quality.
- `tests/reconstruction_ui_capture.py` checks all reconstruction modes and exposure settings against every UI-owned output pixel, with blending, clipping and transform checks.
- `tests/reconstruction_quality.py --evaluate MANIFEST --output METRICS` compares captures with an independent analytic coverage reference. Its static-edge target does not qualify temporal behavior.
- `tests/reconstruction_history_quality.py --evaluate MOVING_MANIFEST --static-manifest STATIC_MANIFEST --output METRICS` measures revealed surfaces by age and distance from edges, and records differences from the corresponding static sequence.

GPU tools require a native Windows graphics session. Configure the Khronos validation layer and synchronization validation externally when reproducing strict GPU checks. The CPU evaluators operate on the resulting manifests and BMP files without a GPU.

For native SDK investigation, `poima-reconstruction-quality-test OUTPUT_PREFIX GPU --history-probes` captures bounded internal samples around the known boundary case. This uses the pinned SDK's private resource layout and is not a portable authoring API. Samples distinguish previous and current history, reactive/disocclusion/shading-change/accumulation channels, and luma instability. Previous history is unavailable on the first dispatch. Diagnostic reads restore the SDK's tracked image layouts and use the capture completion drain before CPU mapping; image parity must be checked separately to establish that observation did not change rendering.

Two control variants retain the moving occluder's trajectory and sampling sequence: `--uniform-background-history` removes the stationary diagonal, while `--flat-diagonal-history` retains its geometry but matches its radiance to the background. The latter allows depth and motion observations to be compared with the original case. Color-derived SDK masks can still differ; these controls do not isolate a single internal shader operation.
