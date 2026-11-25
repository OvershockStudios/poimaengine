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

## Keep a moving subject in focus

Set `focus_target` when authoring a `Camera` component. The entity may be a
subject, a rig node or a child marker on the subject. The offset uses the
target's full transform and defaults to zero. Each axis accepts –1,000 to
1,000 metres.

```json
{
  "vertical_fov": 45,
  "near": 0.10,
  "far": 150,
  "focus_target": {
    "entity": "00000000000000000000000000000027",
    "local_offset_m": [0, 0.35, 0]
  }
}
```

Tracking uses the point's depth along that camera's forward direction. An
off-centre point can set focus. A point before the near plane, at or beyond the
far plane, or outside the supported distance range uses the environment's
`focus_distance_m`. Other named cameras use their own target or that fixed
distance. Free editor views use the fixed distance. The environment still owns
whether the filter is enabled and its blur settings.

Call `runtime.camera_focus` with `session_id`, `tick` and the camera `entity`.
The response reports the effective `distance_m` at the committed pose. Tracking
includes `target`, `world_position_m`, `distance_m` and `in_range`. An existing
noncamera or a camera in a scene without the profile returns `state: null`.
A missing entity or a stale session or tick rejects. In a Vulkan capture,
`render_diagnostics.depth_of_field.tracking` reports the point and distance used
with the capture's camera pose. Eligibility for the filter is reported separately.

Targets are authored references. Remove or replace the binding in the same
transaction when deleting its target. A template's camera can track a target
within the template. Spawning resolves that reference to the new instance's
entity. Save restoration recovers the transforms used to calculate focus. A
moving target can drive a focus pull without an additional saved focus state.

Project and game inspection report `needs_depth_of_field`. Export uses an
installed runtime that advertises `features.depth_of_field`. An authored camera
binding or profile requires this feature, including a profile with a zero radius.

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

Run [the tracking fixture](../tests/camera_focus_capture.py) with the same
options to check movement, target assignment and restore through a standalone
build. The tracking fixture has no `--msaa` option.
[Tracking results](evidence/m2-camera-focus-430.json)
