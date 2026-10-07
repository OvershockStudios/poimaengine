# Ambient occlusion (development)

Poima's experimental spatial GTAO path estimates local ambient visibility from the current view's depth and geometric normals. It affects diffuse ambient lighting. Direct lighting and material emission retain their existing values.

**Bounded integration checks pass on both development-laptop GPUs.** Linux API and render-schedule contracts pass. Windows hardware capture, reconstruction lifecycle and UI tests pass on AMD Radeon integrated graphics and the NVIDIA RTX 4070 Laptop GPU with Vulkan synchronization validation. General visual quality, temporal behavior and performance remain unqualified. The default forward renderer retains exact pixel matches across 16 saved reference captures per device. This feature is disabled by default and is not activated in the installed desktop package. [Evidence](evidence/m2-ambient-occlusion.json).

## Requesting AO

The following options apply to `world.capture`, `runtime.capture`, `asset.animation.capture` and `runtime.play`. Add them to the operation's required parameters:

```json
{
  "lighting_path": "deferred",
  "samples": 1,
  "ambient_occlusion": {
    "mode": "gtao",
    "quality": "medium",
    "radius": 1.0
  }
}
```

Use `world.describe` for the complete schemas and [World service](WORLD_SERVICE.md) for revision, session and capture requirements.

| Property | Values | Default |
| --- | --- | --- |
| `mode` | `none`, `gtao` | `none` |
| `quality` | `low`, `medium`, `high` | `medium` |
| `radius` | Finite number from `0.01` through `100`, in world units | `1` |

Omitting the object or passing `{}` leaves AO disabled. Unknown keys, incorrect types and out-of-range values are rejected even when disabled. Enabled AO requires deferred lighting and one sample per pixel. Invalid API requests preserve authoritative state and do not write capture output.

Quality controls spatial sampling: low uses two slices with four steps on each side; medium uses four slices with four steps; high uses eight slices with eight steps. These are implementation settings, not measured quality or performance guarantees. The radius sets the local sampling distance; larger values do not provide off-screen geometry or global illumination.

## Inspecting the result

Set `scene_debug_view` to `ambient_occlusion` to display filtered visibility. This requires enabled AO and bypasses exposure and tone mapping. Surface pixels range from black (occluded) to white (unoccluded); background is black, consistent with other surface debug views. Non-color debug views cannot be combined with reconstruction.

Requested `scene_product_probes` expose:

- `raw_ambient_visibility`: the spatial estimator's output.
- `ambient_visibility`: its filtered output, used by ambient lighting.
- `surface_valid`: distinguishes covered geometry from background.

Visibility is in `[0,1]`, with `1` meaning unoccluded. Disabled AO and background probes return `1`; use `surface_valid` to interpret background rather than comparing it directly with the black debug display.

Render diagnostics include the effective `ambient_occlusion` options, `ambient_occlusion_buffer_bytes`, and separate `ambient_occlusion_gpu` and `ambient_occlusion_filter_gpu` timing summaries. Those GPU intervals are contained in the existing post interval; adding them to that interval double-counts work.

## Rendering and cost

Two compute passes run between opaque surface rendering and deferred lighting. The first reconstructs positions and integrates visible horizon intervals. The second uses a 3×3 filter with geometric-normal and plane-distance rejection. The estimator uses geometric normals, rather than normal-map detail, to avoid treating shading detail as occluding geometry.

The path allocates two R16F images: four logical bytes per internal render pixel, in addition to deferred and inspection storage. With reconstruction, this follows the internal render extent. Logical bytes exclude allocation alignment and driver overhead. The images are not allocated when AO is disabled.

Sampling is deterministic and spatial, with no AO history or frame-varying noise. Off-screen samples provide no occlusion evidence. Smooth distance falloff fades each sampled horizon toward the geometric plane hemisphere; a radius-relative bias reduces self-occlusion. Rear-facing projected normals retain the full angular interval as a conservative fallback. Screen-space visibility cannot recover hidden geometry, and thin surfaces, screen boundaries, motion and large radii need further visual evaluation. AO does not supply indirect light or replace GI.

## Verification fixtures

The current fixtures separate API correctness from GPU behavior:

| Fixture | Intended coverage |
| --- | --- |
| `world_contract`, `runtime_contract` | Schema discovery, invalid option rejection and authoritative-state preservation. |
| `render_schedule_native` | Resource roles, initialization, ordering, formats, extents and invalid schedules. |
| `ambient_occlusion_native.cpp` | Plane/corner visibility and separation of ambient, direct and emissive radiance. |
| `ambient_occlusion_capture.py` | Defaults, quality/radius bounds, sloped geometry, normal-map independence, background and exposure-independent debug output. |
| `reconstruction_native.cpp … deferred gtao` | Planar visibility during reconstruction, resize, resets and interleaved view lifetimes. |
| `reconstruction_ui_capture.py --lighting-path deferred --ambient-occlusion gtao` | Occluded geometry beneath a colored UI patch, with probes requiring nontrivial visibility reduction; full-image UI comparisons against matching AO-disabled reconstruction modes and exposures. |

The GPU fixtures pass under Vulkan synchronization validation on both devices: 12 basic captures, 21 API captures, 100 reconstruction captures and 50 UI captures per device. They do not establish general visual quality, temporal stability or game-scale performance. An initial hardware failure exposed incorrect distance-falloff reference horizons; the corrected estimator passes the original corner and sloped-plane assertions without relaxed tolerances.
