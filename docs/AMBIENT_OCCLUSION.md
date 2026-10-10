# Ambient occlusion (development)

Poima's experimental spatial GTAO path estimates local ambient visibility from the current view's depth and geometric normals. It affects diffuse ambient lighting. Direct lighting and material emission retain their existing values.

**Bounded integration checks pass on both development-laptop GPUs.** Linux API and render-schedule contracts pass. Windows hardware capture, reconstruction lifecycle and UI tests pass on AMD Radeon integrated graphics and the NVIDIA RTX 4070 Laptop GPU with Vulkan synchronization validation. General visual quality, temporal behavior and performance remain unqualified. The default forward renderer retains exact pixel matches across 16 saved reference captures per device. This feature is disabled by default. [Evidence](evidence/m2-ambient-occlusion.json).

## Bounded optimization checkpoint — 0.0.95

The optimized estimator retains full internal resolution, every quality tier's
slice/step counts, radius, bias, falloff and the existing filter. Front-facing
projected normals use an algebraically equivalent direct integral; rear-facing
and tiny projections keep the general cosine-lobe integral.

Neighbor samples need positions rather than normals. Their unused normal
texture reads are omitted because the renderer-owned RGBA32 correspondence
image is cleared to zero and its normal coordinates are written only by
`encode_normal`. That producer guarantees finite octahedral coordinates in
`[-1,1]`, whose decoded vector has nonzero bounded length. Center normal reads
and validation remain complete, as do neighbor coordinate, viewport, surface,
depth and reconstructed-position checks. Future G-buffer writers must preserve
this producer invariant or restore neighbor normal validation.

Paired captures use identical authored geometry, cameras, content, quality and
radius on each device: 25 captures per binary, including planes, contact corners,
sloped/grazing geometry, rear imported normals, background edges and two 1080p
Performance Yard views. All seven AO-disabled images match exactly. Enabled
images differ by at most one RGB channel level; raw and filtered visibility
probes differ by at most 0.00048828125, within the fixed 0.002 budget. This is
bounded visual equivalence, not bit-exact enabled output or general physical AO
accuracy. The original AO integration evidence remains historical.

Matched Performance Yard runs use 30 seconds of warm-up and 60 seconds of
measurement at 1920×1080, deferred lighting and medium GTAO without reconstruction
or frame generation:

| Device | Mean AO, reference → candidate | Mean total GPU, reference → candidate | Present-return p95, reference → candidate |
| --- | --- | --- | --- |
| RTX 4070 Laptop | 1.079 → 0.902 ms | 2.747 → 2.692 ms | 12.478 → 12.695 ms |
| AMD integrated | 10.777 → 4.001 ms | 19.584 → 13.416 ms | 27.067 → 18.625 ms |

AMD candidate p99 is 19.486 ms. It meets the 25 ms p99 budget and still misses
the 16.7 ms p95 budget. NVIDIA candidate p99 is 13.721 ms; these measurements
do not show a NVIDIA cadence improvement. The earlier integral-only and guarded
decode candidates did not establish a meaningful AMD gain and remain recorded.
Short runs on one laptop do not qualify production performance, a soak or GPU
residency. [Results and compressed raw observations](evidence/m2-ao-optimization.json).

### Reproduce the paired captures

Retain the reference executable and its matching runtime dependencies before
building the candidate. Use native Windows Python, new output directories and
the same prepared [Performance Yard](../examples/performance-yard/README.md)
world for both calls:

```powershell
python tests/ambient_occlusion_equivalence.py --binary build/ao-reference/poima.exe --output build/ao-reference-gpu0 --gpu 0 --workload-world build/performance-world/world.json
python tests/ambient_occlusion_equivalence.py --binary build/windows-runtime/poima.exe --output build/ao-candidate-gpu0 --gpu 0 --reference build/ao-reference-gpu0 --workload-world build/performance-world/world.json
```

Replace executable paths with your retained reference and candidate builds;
repeat with GPU 1 and separate outputs. The verifier pins its own sources,
executable dependencies and content, checks clean native owner exits, and keeps
failure records. Omitting `--workload-world` runs the 19 geometric captures;
including it runs all 25. For timed comparisons, run the
[Performance Yard benchmark](../examples/performance-yard/README.md#measure)
separately against each executable with the same world and compiled artifact.

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
| `ambient_occlusion_equivalence.py` | Paired reference/candidate geometry and Performance Yard captures with fixed image/probe budgets and frozen inputs. |
| `reconstruction_native.cpp … deferred gtao` | Planar visibility during reconstruction, resize, resets and interleaved view lifetimes. |
| `reconstruction_ui_capture.py --lighting-path deferred --ambient-occlusion gtao` | Occluded geometry beneath a colored UI patch, with probes requiring nontrivial visibility reduction; full-image UI comparisons against matching AO-disabled reconstruction modes and exposures. |

The GPU fixtures pass under Vulkan synchronization validation on both devices: 12 basic captures, 21 API captures, 100 reconstruction captures and 50 UI captures per device. They do not establish general visual quality, temporal stability or game-scale performance. An initial hardware failure exposed incorrect distance-falloff reference horizons; the corrected estimator passes the original corner and sloped-plane assertions without relaxed tolerances.
