# Scene HDR composition (development)

The development renderer stores scene-linear RGB in an RGBA16_FLOAT attachment. When MSAA is enabled, it resolves that attachment before applying exposure and the display transform. Geometry, procedural sky and the flat scene background use this common path. Native game UI and editor overlays are composed afterward, so scene exposure does not change their authored colors.

This is an internal HDR rendering stage with SDR output. Optional [bloom](BLOOM.md)
uses the same radiance before exposure and tone mapping. An authored
[project post effect](PROJECT_POST_EFFECTS.md) processes resolved scene-linear
radiance before bloom, exposure and tone mapping. HDR monitor output and GI
have separate implementation and qualification scopes.

## Output contract

The output pass adds enabled bloom to the selected scene radiance, multiplies
by `LightingEnvironment.exposure`, and evaluates the optional
[output grade](PROJECT_POST_EFFECTS.md#grade-exposed-scene-color). It applies
the selected SDR tone mapper and encodes sRGB exactly once.
An sRGB attachment performs that encoding. An UNORM attachment uses explicit
shader encoding. The pass uses an integer texel load for scene radiance to
keep sampling consistent between capture, player and editor.

Exposure zero makes scene content black, including the flat background. UI remains visible. Legacy preview boxes retain their simple preview shading but now receive the same output transform as other geometry; their previous exposure bypass is intentionally removed. The standalone diagnostic triangle remains outside the scene pipeline.

## Choose a tone mapper

Author `LightingEnvironment.tone_mapper` on the world environment. Use
`world.describe` to discover the enum and `world.lighting` or `runtime.lighting`
to inspect the selected value. For example, a scene can opt into AgX with
this environment component.

```json
{
  "ambient": [0.04, 0.05, 0.07],
  "exposure": 0.8,
  "tone_mapper": "agx"
}
```

| Value | SDR response |
| --- | --- |
| `reinhard` | Legacy component-wise `x / (1 + x)` mapping. Omission uses the same output. |
| `agx` | Filmic contrast with a soft highlight shoulder and saturation roll-off, using the [three.js r180 AgX approximation](https://github.com/mrdoob/three.js/blob/r180/src/renderers/shaders/ShaderChunk/tonemapping_pars_fragment.glsl.js). |
| `pbr_neutral` | Preserves product and material colors through most of the range, with strong highlights rolling toward white. [Khronos PBR Neutral specification](https://github.com/KhronosGroup/ToneMapping/tree/main/PBR_Neutral) |
| `aces2_sdr` | ACES 2 SDR 100-nit Rec.709 output with D65 white, using the pinned OpenColorIO CG config. [ACES output presets](https://docs.acescentral.com/system-components/output-transforms/parameters/) |

AgX here approximates an SDR display transform in linear Rec.709. Exact
Blender/OpenColorIO color matching and HDR display output have separate
qualification scopes. The transform follows reconstruction, bloom, exposure
and the optional output grade. Native game UI and diagnostic views bypass it.

The selector is case sensitive. Runtime sessions freeze the environment with
the authored world revision. An explicitly selected value requires
`features.tone_mapping` in the installed export runtime descriptor.
`aces2_sdr` also requires `features.tone_mapping_aces2_sdr`. Remove the
field for legacy content compatibility. A runtime that lacks a required
feature refuses export before publishing a bundle.

ACES 2 maps Poima's exposed linear Rec.709 through the ACES rendering
transform into linear display Rec.709. The output pass then applies the usual
sRGB encoding. The config and its hue and gamut tables are pinned to
OpenColorIO 2.5.2, CG v4.0. The Vulkan renderer uploads the 11,616-byte lookup
on the first accepted ACES 2 frame of each renderer context. The shader and
tables are embedded in the runtime. Regenerate them using
`scripts/generate_aces2_sdr.py` with the OpenColorIO 2.5.2 Python package.
[OpenColorIO release](https://github.com/AcademySoftwareFoundation/OpenColorIO/releases/tag/v2.5.2)

## Adapt exposure between dark and bright areas

Set `LightingEnvironment.auto_exposure` to enable the GPU meter. For example,
this profile lets a night scene gain up to three quarters of a stop while
letting it recover faster when the camera faces a bright source.

```json
{
  "ambient": [0.04, 0.05, 0.07],
  "exposure": 0.8,
  "auto_exposure": {
    "min_stops": -1.5,
    "max_stops": 0.75,
    "brighten_stops_per_second": 0.75,
    "darken_stops_per_second": 3
  }
}
```

Use `{}` for the defaults of −4 to +4 stops, 2 stops/s brightening, and
4 stops/s darkening. Each limit must be within [−8, 8] with the minimum at
or below the maximum. The rates accept [0.125, 32] stops/s. Remove the optional
object to use manual exposure alone. An export needs a runtime with
`features.auto_exposure`.

The meter samples up to 64 × 36 positions across the rendered world rectangle
after MSAA resolve and world fog and reflections. It bins Rec. 709 luminance
into 64 log2 buckets spanning [−12, 12], then trims the lowest and highest 10%
of the samples. It estimates the mean in stops from the remaining bin centers.
The target moves that mean toward linear 0.18, subject to the authored bounds.
The output multiplies manual exposure by `2^adapted_stops` after adding bloom
and before applying the output grade. The meter uses world radiance before FSR,
foreground viewmodel, project post effects, bloom, and UI composition.

Each view owns its GPU history. A new source, camera cut, viewport change, or
resize snaps to the measured target. Later accepted frames move at the selected
rate. Live playback uses elapsed time between accepted frames. Replays and
capture fixtures use a fixed 60 Hz interval. Saves retain the authored settings.

`world.lighting` and `runtime.lighting` report the resolved settings.
Vulkan diagnostics report whether the meter reset, its interval, and the sample
count. Capture diagnostics also report the adapted stops, target stops,
metered log2 luminance, and multiplier read back from the GPU.

The design uses established [luminance histogram metering](https://alextardif.com/HistogramLuminance.html)
and [separate adaptation rates in stops](https://dev.epicgames.com/documentation/unreal-engine/auto-exposure-in-unreal-engine).

## Bounds and ownership

Half-float radiance storage is bounded to 65,504 per channel before writing. Values beyond that saturate; negative values are clamped and NaNs become zero. This prevents half-float overflow from contaminating the resolve/output pass, but loses extreme highlight ratios. Pre-exposure or wider storage is future work. Authored light limits are unchanged and do not imply lossless storage across that entire range.

The renderer checks format/image capabilities for its color, sampled and transfer usage, extent and requested sample count. Unsupported combinations fail explicitly. HDR color attachments have a 512 MiB combined budget per renderer context, separate from depth, swapchain, UI and staging allocations. Two editor viewports own separate contexts and allocations.

Output bindings and framebuffers follow swapchain recreation. Each renderer
context shares resolved HDR and exposure state across ordered graphics queue
submissions. Capture readback belongs to the submitted frame slot. Resize
retires frames before replacing their image bindings.

## Qualification

Build `poima-auto-exposure-test` from the Windows Vulkan configuration.
Pass a fresh output prefix, a GPU index, a lighting path, and a reconstruction
mode. For example, from WSL run the following after creating the output directory.

```sh
build/windows-runtime/bin/poima-auto-exposure-test.exe \
  "$(wslpath -aw build/exposure-check/frame)" 1 deferred fsr3_quality
```

The fixture compares the captured GPU state and pixel values against an
independent meter, checks camera cuts and resize, and verifies the manual
capture after disabling the effect. [Exposure results](evidence/m2-auto-exposure-400.json).

Run `tests/tone_mapping_capture.py` with `--aces2` and the pinned
OpenColorIO Python package to compare Windows Vulkan captures with
the CPU display transform. The test covers color patches, bloom,
grading and fixed-stop auto exposure. Pass `--runtime` for the relocated
export check, `--reconstruction fsr3_native` for reconstruction, and
`--check-ui` for the composed HUD. [ACES 2 results](evidence/m2-aces2-sdr-462.json)

The Windows native engine, UI capture executable and desktop bridge build and link. Both the RTX 4070 Laptop GPU and AMD integrated GPU pass 174 scene captures across the HDR, lighting, sky and material suites. Eight additional integration groups cover UI at 1×/4× MSAA, standalone player parity and independent native Scene/Game viewport lifetimes on both GPUs. [Qualification evidence](evidence/m2-hdr-composition.json).

The HDR edge fixture finds 248 partially covered pixels on each GPU. Captures match the resolve-before-tone-map reference within the permitted tolerance and differ from the old ordering by at least 27 display levels. The saturation fixture returns the expected value of 169 rather than the unbounded reference of 245. Zero-exposure scene captures are entirely black. UI comparisons preserve every pixel at exposures 0, 1 and 64 while proving the scene background changes.

These are small correctness fixtures. They do not qualify game-scale frame times, Linux rendering, HDR displays, physical input or the Avalonia editor shell. Hosted tests exercise native windows, resize, hide/restore and device teardown. GPU behavior at 2×/8× MSAA remains separately unqualified.

The independent capture fixture covers shared exposure, finite saturation, zero exposure, and partially covered MSAA pixels that distinguish resolving radiance from resolving already tone-mapped colors. Its analytic self-test validates the reference oracle only; it does not execute the renderer. Existing material, sky, lighting, UI and player checks remain relevant.

```sh
python3 tests/hdr_composition_capture.py --self-test
python3 tests/hdr_composition_capture.py build/windows-runtime/poima.exe --output build/hdr-gpu --gpu 0 --windows-interop
```

The second command requires a freshly rebuilt renderer and a graphics session. [Earlier shader and syntax validation record](evidence/m2-hdr-composition-development.json).

`tests/tone_mapping_capture.py` compares both optional transforms against a
CPU color reference. Run it against a Windows Vulkan executable with a new
`--output` directory and the intended `--gpu` index. Pass `--reference` with
the retained 0.0.454 executable for exact legacy comparison, or `--runtime`
with a matching installed runtime to exercise standalone export and replay.
[Tone-mapping results](evidence/m2-tone-mapping-455.json)

The output encoding follows the [Vulkan color-space contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkColorSpaceKHR.html). See [authored lighting](LIGHTING.md) for component units and [implementation status](IMPLEMENTATION_STATUS.md) for qualified engine capabilities.
