# Scene HDR composition (development)

The development renderer stores scene-linear RGB in an RGBA16_FLOAT attachment. When MSAA is enabled, it resolves that attachment before applying exposure and the display transform. Geometry, procedural sky and the flat scene background use this common path. Native game UI and editor overlays are composed afterward, so scene exposure does not change their authored colors.

This is an internal HDR rendering stage with SDR output. HDR monitors, automatic exposure, bloom, color grading, GI and temporal upscaling are not implemented by this change.

## Output contract

The output pass multiplies resolved scene radiance by `LightingEnvironment.exposure`, applies component-wise Reinhard mapping `x / (1 + x)`, then encodes sRGB exactly once. An sRGB attachment performs that encoding; an UNORM attachment uses explicit shader encoding. The pass uses an integer texel load at the output pixel, avoiding a resampling filter or different UV convention between capture, player and editor.

Exposure zero makes scene content black, including the flat background. UI remains visible. Legacy preview boxes retain their simple preview shading but now receive the same output transform as other geometry; their previous exposure bypass is intentionally removed. The standalone diagnostic triangle remains outside the scene pipeline.

## Bounds and ownership

Half-float radiance storage is bounded to 65,504 per channel before writing. Values beyond that saturate; negative values are clamped and NaNs become zero. This prevents half-float overflow from contaminating the resolve/output pass, but loses extreme highlight ratios. Pre-exposure or wider storage is future work. Authored light limits are unchanged and do not imply lossless storage across that entire range.

The renderer checks format/image capabilities for its color, sampled and transfer usage, extent and requested sample count. Unsupported combinations fail explicitly. HDR color attachments have a 512 MiB combined budget per renderer context, separate from depth, swapchain, UI and staging allocations. Two editor viewports own separate contexts and allocations.

Output bindings and framebuffers follow swapchain recreation. The current serialized submission model permits one resolved scene texture per context. Future overlapping frames must revise this resource ownership before removing the GPU wait.

## Qualification

This change is under development. All seven affected shader stages compile with DXC. An isolated Windows C++ syntax check passes, with an existing Vulkan-header deprecated-copy warning. Full native linking, GPU capture and resize/player/editor qualification are pending; older lighting/sky screenshots do not qualify the new pass order. No new desktop package or performance claim accompanies this document.

The independent capture fixture covers shared exposure, finite saturation, zero exposure, and partially covered MSAA pixels that distinguish resolving radiance from resolving already tone-mapped colors. Its analytic self-test validates the reference oracle only; it does not execute the renderer. Existing material, sky, lighting, UI and player checks remain relevant.

```sh
python3 tests/hdr_composition_capture.py --self-test
python3 tests/hdr_composition_capture.py build/windows-runtime/poima.exe --output build/hdr-gpu --gpu 0 --windows-interop
```

The second command requires a freshly rebuilt renderer and a graphics session. [Development validation record](evidence/m2-hdr-composition-development.json).

The output encoding follows the [Vulkan color-space contract](https://docs.vulkan.org/refpages/latest/refpages/source/VkColorSpaceKHR.html). See [authored lighting](LIGHTING.md) for component units and [implementation status](IMPLEMENTATION_STATUS.md) for qualified engine capabilities.
