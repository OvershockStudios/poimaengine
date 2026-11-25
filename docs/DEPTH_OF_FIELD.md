<!-- SPDX-License-Identifier: Apache-2.0 -->
# Depth of field

Set `LightingEnvironment.depth_of_field` with a guarded `world.transact`
to focus a color view at a known distance. For a close shot, measure the
subject's **view-space** depth along camera forward with a ray or committed
pose before choosing the focus distance.

```json
{
  "focus_distance_m": 1.05,
  "focus_band_m": 0.12,
  "radius_at_infinity_px": 10,
  "max_radius_px": 17
}
```

All four fields are required. The focus distance is measured in metres and
accepts 0.01–1,000,000. The focus band is a half-width in metres and accepts
0–1,000,000. Sky lies at infinite distance. The two radii accept 0–24
**internal render pixels**. They control the radius at infinity and the
near/far radius cap, respectively. Set either radius to zero to disable the
filter, or omit the object to use the original world image.

For finite view depth `z`, the signed circle-of-confusion radius scales with
`(abs(z - focus_distance) - focus_band) / z` outside the band. The focus plane
stays sharp. A fixed two-ring gather filters samples on the corresponding
side of focus. A separate foreground gather lets nearby blurred silhouettes
extend over more distant pixels. This uses one visible depth layer and a
bounded sampling pattern. It approximates out-of-focus highlights and
occluded background. [Thin-lens reference](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-28-practical-post-process-depth-field)
[Rendering and reconstruction constraints](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/depth-of-field/)

The renderer applies the filter after opaque lighting, screen reflections
and world fog. It applies particle sprites afterward. FSR consumes the
composed world image and its composition mask. Separate viewmodels and the
HUD are drawn later. Color at one sample is supported for forward and
deferred rendering. Multisampling, diagnostic views, a non-rigid camera,
a hidden scene, world-depth viewmodels and transparent water select the
original path. The lens parameters belong to the authored lighting
environment. The runtime freezes them with that environment revision.

`world.lighting` and `runtime.lighting` show the authored selection.
`render_diagnostics.depth_of_field` reports the active state, reason, and
RGBA16F target size. The target uses eight logical bytes per internal
pixel, with a 128 MiB cap. Under reconstruction, it also counts toward the
existing composition budget. Raw HDR scene probes retain the original
opaque values. Resolved probes include the filter on the selected path.

Project and game inspection report `needs_depth_of_field`. Export uses an
installed runtime that advertises `features.depth_of_field`. The selected
profile is still part of the project when either radius is zero.

Run the [fixture](../tests/depth_of_field_capture.py) against an installed
Windows Vulkan build. Add `--windows-interop` from WSL. When checking a relocated
export from WSL, set `--output` on a local Windows drive for its external save root.

```sh
python3 tests/depth_of_field_capture.py POIMA --output DIRECTORY \
  --gpu GPU_INDEX --lighting-path deferred --fsr --runtime-root RUNTIME
```

A forward build can use `--msaa` to check the four-sample fallback. Omit
rendering options to check the authoring contract on a simulation build.
[Qualification results](evidence/m2-depth-of-field-429.json)
