# Visibility and render diagnostics

Poima 0.0.12 adds conservative CPU frustum culling for the camera and each shadow view, submitted-draw counters, and optional CPU/GPU timing. These are exposed through native capture/play operations so an agent can relate an image to the work that produced it. They do not establish a production frame-rate target.

The later [native profiler](PROFILER.md) retains scoped CPU events and separate GPU duration observations for shared agent/editor inspection. The aggregate report below remains supported.

## Request controls

`world.describe` schema revision 44 exposes these optional parameters on `world.capture`, `runtime.capture`, `asset.animation.capture` and `runtime.play`:

| Parameter | Default | Meaning |
| --- | --- | --- |
| `culling` | true | Reject object bounds entirely outside each render view. False submits every renderable to every view for comparison. |
| `clustered_lighting` | true | Conservatively assign finite lights to view-space cells. False evaluates the complete light table as a reference. |
| `frames_in_flight` | 2 | Integer from 1 to 2. Two bounds queued submissions; one provides a serialized reference. |
| `scene_debug_view` | `color` | Select `depth`, `shading_normal`, `motion` or `motion_validity` with `samples: 1`; see [scene products](SCENE_PRODUCTS.md) for encoding and qualification. |
| `scene_product_probes` | `[]` | Up to 64 ordered `{x,y}` pixel requests for raw depth, shading normal, motion and validity; requires a capture path and `samples: 1`. |
| `reconstruction` | `none` | Optional FSR NativeAA or fixed-ratio upscaling; requires an enabled build, `samples: 1` and the color view. See the [reconstruction contract and qualification boundary](RECONSTRUCTION.md). |
| `profile` | false | Collect CPU intervals and, when supported, graphics-queue GPU timestamps. False records no timing samples or query commands. Draw counters remain available. |

```json
{"jsonrpc":"2.0","id":1,"method":"world.capture","params":{"revision":1,"camera":"00000000000000000000000000000001","path":"build/profile.bmp","culling":true,"profile":true}}
```

These controls are observation/player options, not persistent world components. Existing revision/session/tick guards and capture destination rules still apply. Wrong types and values outside the declared bounds fail with `-32602`. Play retry receipts retain the original diagnostics; an identical retry does not rerun the player or resample timings.

The three capture operations also accept `capture_frames` (1–128, default 2). They repeatedly render a frozen snapshot without advancing simulation. `runtime.play` does not accept this option. Sparse probes now include raw and resolved scene-linear HDR; when reconstruction is enabled, their requested coordinates address the compact render input. See [probe mapping](RECONSTRUCTION.md#numeric-observations-and-diagnostics).

## Visibility decisions

Bounds are derived from all stored mesh vertex positions, or the unit box for built-in geometry. Local bounds are cached per immutable mesh for the renderer lifetime. Their transformed world-space axis-aligned bounds account for rotation, nonuniform scale and shear. Bounds are widened for float rounding, and plane tests include a conservative error margin. Touching or numerically uncertain bounds are retained.

The six planes use the actual float-rounded GPU view-projection matrix and Vulkan's `[0,w]` clip-depth convention. The camera and each directional cascade, point face or spotlight test their own frustum. Camera rejection therefore does not remove a potentially visible shadow caster. Light/caster movement and viewport resize rebuild the relevant decisions.

This is object-level frustum culling, not occlusion culling. Bounds can produce false positives. There is no hierarchical scene index, meshlet/triangle culling, GPU-driven submission or instancing yet. CPU work is linear in objects times views. All renderables still resolve/upload geometry and material resources; this change does not implement asset streaming or defer invalid-content errors until an object appears. Hidden render components are excluded by snapshot construction and are not counted as frustum rejects.

## Response

Successful capture and all returned player reports include `render_diagnostics`:

- `culling`, `profile_requested`: selected options.
- `completed_submissions`: command submissions whose graphics completion and diagnostic readbacks have retired. An out-of-date presentation may still have completed GPU work, so this is distinct from presented frames.
- `frame_execution`: configured limit, submitted/outstanding/peak counts, slot/drain/device-idle wait counts and the presentation retirement policy.
- `last_draws`: counts for the last completed submission, not lifetime totals.
- `light_assignment`: selected path, grid and buffer bounds, and actual GPU candidate counts when available. Unsampled candidate statistics are null.
- `cpu`: timing summaries for scene preparation, command recording, the render call and blocking completion waits.
- `gpu`: timestamp availability, valid bits, nanoseconds per tick, dropped samples, interpretation detail, and pass summaries.

`last_draws` fields:

| Field | Meaning |
| --- | --- |
| `objects` | Renderable snapshot objects. |
| `camera_draws`, `camera_culled` | Submitted camera draws and rejected object bounds; their sum equals `objects`. |
| `camera_triangles` | Triangles submitted to the camera. Back-face/depth rejection can reduce rasterized work further. |
| `shadow_views` | Active depth views for this camera/light configuration. |
| `shadow_candidates` | `objects * shadow_views`. |
| `shadow_draws`, `shadow_culled` | Submitted and rejected object/view pairs; their sum equals `shadow_candidates`. |
| `shadow_triangles` | Triangles submitted across all active shadow views. |

Draw counters describe scene objects; the optional fullscreen sky triangle is excluded. Its GPU work is included in `opaque`.

Each timing summary contains `samples`, `min_ms`, `mean_ms`, `max_ms` and `last_ms`. An unsampled timing has zero samples and null values, not a fabricated zero duration. Summaries are bounded aggregates; the response does not emit a per-frame trace.

## Timing boundaries and limitations

CPU measurements use the native monotonic clock:

- `prepare`: building draw data, bounds/visibility and any initial or newly required geometry/texture upload waits. Captures prepare once; a player prepares at initialization, during play and for final capture.
- `record`: command recording and closing, excluding submission/presentation/wait and preceding scene preparation.
- `render_call`: slot reuse, acquire, recording, submit/present and any required completion wait. It excludes simulation, snapshot extraction before `update_scene`, event handling and image file encoding. It is not an end-to-end game frame time.

- `completion_wait`: actual blocking waits for an occupied slot or an explicit graphics drain. Already completed submissions add no wait sample.

GPU measurements use six 64-bit Vulkan timestamps on the same graphics queue. Availability is checked using the device period and selected queue's valid bits. Durations use the valid-bit mask; a CPU interval long enough to make wrap ambiguity possible is discarded. Each slot owns its query/readback storage. Readback occurs after that submission completes; ordinary retirement polls, while slot reuse and explicit drains may wait. Unsupported queues report unavailable timing while rendering and CPU profiling remain usable. `samples_dropped` reports unavailable/ambiguous completed samples rather than reusing prior values.

GPU intervals:

| Summary | Approximate included work |
| --- | --- |
| `skinning` | Frame-uniform upload and skinning dispatch. |
| `light_assignment` | Light-table upload, candidate assignment and the GPU copy of counts for diagnostics. CPU mapping/scanning is outside this interval. |
| `shadows` | Shadow clears/draws and transition to sampling. |
| `opaque` | Main target/depth clears, optional procedural sky, and opaque draws. |
| `post` | Linear MSAA resolve, scene output mapping, UI/overlays, optional image readback copy, and transition for presentation. Capture frames include a copy that ordinary player frames lack. |
| `total` | The complete timestamp span, excluding swapchain acquire, presentation completion, CPU simulation and file writing. |

The start stamp uses top-of-pipe and later boundaries use bottom-of-pipe. These are approximate intervals on an overlapping GPU pipeline, not isolated shader costs. Timestamps themselves add synchronization/measurement overhead. Neither totals nor their reciprocals establish playable FPS. A two-frame capture includes cold/warm effects and the final readback; use controlled longer workloads for performance comparisons. The CPU can retain two submissions, but a cross-submission dependency still orders shared GPU scratch. This does not establish concurrent GPU frame execution or production frame pacing.

Optional reconstruction adds two timestamps for temporal input preparation and SDK dispatch. Its `diagnostics.reconstruction.gpu` duration is contained within `gpu.post`; do not add both together. The default rendering path retains its existing timestamp layout.

The pinned NVRHI timer implementation reads 32-bit results. Poima uses its supported native-command-buffer access for these 64-bit query operations without changing NVRHI source or graphics binding state.

## Submission and presentation lifetime

Completed counters and readbacks belong to their original submission. A hosted viewport report polls completion; a capture or final standalone/player report drains pending graphics work. Submitted work is not necessarily presented or scanned out.

Acquire semaphores, completion tracking, timestamps and diagnostic readbacks belong to bounded frame slots. Present-finished semaphores belong to swapchain images. Where supported and enabled, maintenance1 present fences guard presentation teardown. Other devices use bounded idle compatibility cleanup during resize/shutdown; the reported policy explicitly does not claim formal presentation-completion proof. No retired swapchain generations accumulate.

Normal warmed frames avoid device-wide idle waits. Existing geometry, texture and font uploads can still wait, as can lifecycle cleanup. `device_idle_waits` includes those calls; two slots alone do not eliminate streaming stalls or guarantee higher frame rates.

## Evidence and reproduction

The [frame-retirement evidence](evidence/m2-frame-retirement.json) records one/two-slot parity, stable-resource idle counts, changing-frame attribution and strict Vulkan validation on both laptop GPUs. Reproduce the native owned-window fixture after building `poima-frame-execution-test`:

```sh
python3 tests/frame_execution_capture.py build/windows-runtime/poima-frame-execution-test.exe --windows-interop --output build/frame-execution --gpu 0
```

This command does not install or enable the Khronos layer automatically. Qualification enabled it explicitly, including synchronization validation. The fixture checks 49 changing submissions per mode through original-frame profiler attribution, alongside resize, hide/show, recreation and readback parity. Its short interleaved-view timing distributions are observations, not a sustained game benchmark.

```sh
python3 tests/culling_capture.py build/windows-runtime/poima.exe --windows-interop --output build/culling-capture --gpu 0
python3 tests/culling_capture.py build/windows-runtime/poima.exe --windows-interop --output build/culling-capture --gpu 1
python3 tests/player_contract.py build/windows-runtime/poima.exe --windows-interop --authored-lights --shadows --profile --output build/culling-player --gpu 0
```

The correctness fixture has 245 renderables, including 240 distant boxes, an offscreen shadow caster, a near-plane intersection, a sheared hierarchy and an imported receiver. Culled/unculled and profiled/unprofiled images must agree exactly. A moving runtime caster must also preserve parity. Draw-counter identities and actual GPU timing availability are checked. The player test compares a culled 371-tick replay against an unculled independent headless replay/capture. Native tests check all clip planes, boundary/enclosing cases, perspective depth and 10,000 affine transforms against float-rounded corners.

See [recorded evidence](evidence/m2-render-diagnostics.json). Draw reductions in this constructed offscreen fixture are not a representative shipped-game speedup. Representative game workloads and broader hardware tiers remain unqualified.

The [typed pass schedule](RENDER_SCHEDULE.md) validates the resources used by these operations before recording and exposes per-pass CPU scopes through the profiler. Its [qualification evidence](evidence/m2-render-schedule.json) compares the executable schedule against the preceding renderer on both laptop GPUs.
