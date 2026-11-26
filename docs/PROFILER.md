# Native profiler

Poima can record bounded native CPU work, renderer GPU durations and selected resource counters. The editor and command service inspect the same session-owned capture. Profiling is opt-in and independent of authored documents, game state, saves and rollback.

## Editor

Open **Window → Profiler** and press **Record** (16,384 events). Run the workload, then press **Stop** to load the capture. Recording does not start Play or otherwise change simulation. Closing this tool does not stop a capture owned by the shared session.

The selectable CPU timeline shows individual scoped operations. Filter by source to distinguish editor polling, Scene, Game and standalone player work. Select a span to inspect its parent, runtime session, tick, elapsed time and failure flag. Use the duration summary to sort aggregate, mean, p95 or maximum cost. Timeline display is paged; the native capture and export retain all recorded events.

CPU scopes measure elapsed wall time, including waits and scheduling delays; they are not sampled on-CPU utilization. Scopes are inclusive: a parent includes its children, so summing rows double-counts nested work. The editor records native polling and viewport work, not total Avalonia layout/rendering, managed UI allocations or operating-system scheduling. A simulation tick is not a presented frame.

GPU rows report timestamp-derived durations collected when the submitted frame completes. Completion may be observed by polling or after a required wait; collecting a GPU sample does not imply that the CPU blocked. GPU durations are **not aligned to the CPU timeline**. Resource counters show last/min/max/mean values rather than sums. Texture payload and skin-buffer bytes are partial tracked resources, not total GPU residency or VRAM usage.

**Export** writes a new Chrome trace-event JSON file. Existing files are preserved. The native export can also be opened in viewers supporting that format, including [Perfetto](https://perfetto.dev/docs/getting-started/other-formats). CPU events export as complete spans; GPU durations export as counter observations. CPU lanes use actual OS thread IDs. Logical sources remain event metadata; [native job spans](JOBS.md#profiling) preserve their submission attribution and execution intervals.

## Command service

Discover schemas from the current build with `world.describe`. All commands work through a persistent `world`, shared headless host or editor session; capture is lost when its owning session closes.

```json
{"jsonrpc":"2.0","id":1,"method":"profiler.start","params":{"capture_id":"0123456789abcdef0123456789abcdef","expected_capture_id":null,"capacity":16384}}
{"jsonrpc":"2.0","id":2,"method":"profiler.status","params":{}}
{"jsonrpc":"2.0","id":3,"method":"profiler.stop","params":{"capture_id":"0123456789abcdef0123456789abcdef"}}
{"jsonrpc":"2.0","id":4,"method":"profiler.summary","params":{"capture_id":"0123456789abcdef0123456789abcdef"}}
{"jsonrpc":"2.0","id":5,"method":"profiler.events","params":{"capture_id":"0123456789abcdef0123456789abcdef","offset":0,"limit":256}}
{"jsonrpc":"2.0","id":6,"method":"profiler.export","params":{"capture_id":"0123456789abcdef0123456789abcdef"}}
```

Run the workload between start and stop. Profiler control and observation requests do not record themselves. Export returns an object containing `trace`; the CLI caller can save that JSON without giving the service a filesystem destination.

| Operation | Contract |
| --- | --- |
| `profiler.start` | New 32-character lowercase hexadecimal ID, observed `expected_capture_id` (null initially), capacity 64–65,536 with default 16,384, and optional `gpu_passes` Boolean with default false. Allocates before replacing a stopped capture. |
| `profiler.stop` | Requires current capture ID; repeat is harmless. Stops accepting records while existing scopes finish. |
| `profiler.status` | Current identity/state, recording flag, capacity, count, open scopes, loss indicator, elapsed ns and recorder-array bytes. Does not advance simulation. |
| `profiler.events` | Sealed capture only; offset 0–count, limit 1–1,024, default 256. Returns total and nullable next offset. |
| `profiler.summary` | Sealed capture only; groups by name, kind and source. Reports sample count, failed count, last/min/mean/max and nearest-rank p50/p95/p99. |
| `profiler.export` | Sealed capture only; complete trace-event document plus timing and scope metadata. Export allocates outside measured work. |

Starting again with the current ID and identical original parameters returns the current capture without resetting it. Changed parameters reject. Replacing a capture requires stopping it and supplying its observed ID. IDs cannot be reused during the owner's lifetime; the session supports up to 4,096 distinct captures. A stale read/stop cannot act on a replacement capture.

When the next event would exceed capacity, recording automatically stops. The retained prefix and already-open scopes remain valid. `dropped` includes the first rejected reservation; subsequent work uses the disabled path and is not counted individually. `full` therefore means a truncated workload, not a complete count of every missing event. Reading waits until recording is false and open scopes are zero. Captures never overwrite their oldest entries.

## Recorded data

Each event has a capture-local ID, parent ID, bounded name, kind (`cpu`, `gpu`, `counter`), source, optional runtime session and tick, relative `start_ns`, `duration_ns`, value, actual `thread` ID and failure/truncation flags. Native job events also expose group/task identity, lane and queue delay. Children inherit tick/session context unless explicitly changed. Source and session are recorded at entry; a parent spanning a load describes its original context. Later work uses the restored session, so rewound ticks remain distinguishable.

Delayed renderer GPU durations and completion counters retain the submitting frame's source, runtime session, tick and CPU submission timestamp. Collection under a later tick or another bound session does not change that attribution. These events have parent ID zero: their submitting CPU scope may already have ended. Their `start_ns` identifies CPU submission, not GPU execution start; GPU duration remains in `value`, with `duration_ns` zero. Records are appended when collected, so record order need not match submission timestamps.

### Correlate a frame

Record the native profiler and [frame performance](FRAME_PERFORMANCE.md)
on the same running player. Join `profiler.events` to `performance.frames`
using both fields of `render_submission`, for example
`{"renderer_id":2,"submission_id":73}`. The same pair appears in Chrome
trace event arguments. The renderer ID identifies one renderer lifetime in
the host process. The submission ID is the accepted graphics queue serial.
Pause, replay and resize can draw the same tick many times with distinct pairs.
Keep observations from different host processes separate.

The `render.frame` and `render.submit` CPU scopes carry the pair once the
queue accepts their work. Find recording and pass-level CPU scopes through
their event parentage. The frame scope can include CPU work that retires
older submissions. GPU durations and per-submission readbacks such as
barrier and shadow counters carry the pair from their original frame.
Compare `gpu.total.ns` to `gpu_ns.total` on a matched completed performance
row. Use the runtime tick and row sequence to order the matched observations.

Frames without accepted work omit the pair. Overlapping recording windows,
available device timestamps and successful retirement determine which pairs
have GPU samples in both captures. Diagnostic readback and a successful
present can follow the same accepted submission at different times.

Deferred observations are accepted only on the original owner thread while the same recording is still active. Stopping, restarting or destroying that recorder discards outstanding observations rather than attaching them to a later capture. Thus **Stop does not flush pending GPU samples**. Existing CPU scopes still finish normally. Deferred tickets allocate no storage during capture or emission; recording start allocates their lifetime identity along with the event array.

Owner CPU failure flags indicate exception unwinding. Native job flags mark non-success task outcomes, including cooperative cancellation. Neither is a complete semantic classification of every operation error. Work performed before a rolled-back batch remains in the diagnostic trace. It does not mean that simulation state committed. Scope parentage identifies the enclosing batch; there is no separate permanent gameplay event log.

Native instrumentation covers runtime batch validation/checkpointing, fixed ticks, C# calls and command application, physics, animation sampling, hierarchy synchronization, rollback, owner advancement, snapshot/save preparation, renderer preparation/acquire/record/submit/present/retirement/wait/readback, player presentation and native audio queue/DSP boundaries. Only paths actually executed produce samples. Native audio device output and the editor DSP worker have separate [output contracts](AUDIO_EVENTS.md) and [editor qualification](EDITOR_AUDIO.md).

An active [diffuse-bounce](DIFFUSE_BOUNCE.md) pass records
`gpu.screen_diffuse_bounce.ns`. At half resolution the capture also records
`gpu.screen_diffuse_gather.ns` and `gpu.screen_diffuse_resolve.ns`. These
partition the enclosing bounce interval. Use the two child measurements to
attribute the cost of tracing and reconstruction.

### Measure each render pass

Set `gpu_passes: true` in `profiler.start`. On each accepted and retired
Vulkan submission, the profiler records `gpu.pass.<pass>.ns` in the
[executed schedule's](RENDER_SCHEDULE.md) order. Match each sample to the
corresponding `render.pass.<pass>` CPU scope through the
[submission pair](#correlate-a-frame). Scheduled passes with little or no
GPU work can measure zero. Optional passes appear only on the submissions
that execute them. Sealed capture status and trace metadata retain the
requested detail level.

Each duration is an approximate graphics-queue interval between successive
bottom-of-pipe timestamp writes. Adjacent intervals partition
`gpu.render_schedule.total.ns` within rounding to integer nanoseconds.
They include execution dependencies and any waits within those boundaries.
`gpu.pass.present.ns` measures the recorded transition to presentation
state. The CPU present call has its own scope. The existing coarse GPU
intervals overlap the detailed intervals. Use one set of intervals when
attributing cost. [Vulkan timestamp semantics](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdWriteTimestamp.html)

A requested frame on a queue without usable timestamp support reports
`gpu.render_schedule.unavailable`. A failed or inconsistent readback
reports `gpu.render_schedule.samples_dropped`. Both are keyed to the
accepted submission. Each active pass adds a timestamp and profiler record,
plus a schedule boundary at the start and one total record. Use a short
capture or larger capacity for detailed inspection, and use ordinary
profiling to estimate the cost of the detailed mode.

### Vulkan barriers

Start `profiler.start` before capturing or playing a scene. Each retired
profiled frame emits `renderer.barriers.calls` for the recorded pipeline
barrier commands and `renderer.barriers.memory_entries`, `.buffer_entries`
and `.image_entries` for the barrier structures carried by those commands.
One command can contain multiple structures. For a pass that recorded work,
`renderer.barriers.<pass>` counts its commands. For example,
`renderer.barriers.opaque` counts barriers recorded inside
`render.pass.opaque`. Passes with zero calls omit the counter.
`before_passes` includes the explicit dependency for shared frame scratch.
`after_passes`, when present, covers transitions during command-list close.

The counts come from Poima's native Vulkan pipeline-barrier calls and
NVRHI's Vulkan synchronization2 dispatch for the frame command buffer.
Asset upload lists, queue semaphores, and barriers internal to a vendor SDK
are outside this measurement. Match the submission stamp and session when
reconciling counts on older builds. Use `render_submission` when present.
Use GPU timings to assess cost because the number of
recorded barriers does not measure stalls.

The profiler observes the calls with a scoped dispatcher on the render
owner thread and restores the original dispatch entries when the list
closes or recording fails. A frame outside an active capture takes the
unobserved path. [Native check](../tests/profiler_capture.py) covers
attribution, frame totals and exact output with tracing on and off using
`--require-barriers`.

Poima submits the pending image and buffer transitions of each NVRHI
state commit in one synchronization2 command. Each barrier keeps its
original resource, stage and access masks, and image layout. To reproduce
the comparison, run the same native check with `--require-barriers` on
0.0.410 and 0.0.411 on the same adapter. Compare the ordered
`barrier_captures` rows in the two evidence files. The published fixture
records one fewer call in each of the shadow and opaque passes, with
identical barrier entry totals.
[Vulkan dependency info](https://docs.vulkan.org/refpages/latest/refpages/source/VkDependencyInfo.html) ·
[Checkpoint results](evidence/m2-nvrhi-barrier-batching-411.json)

### Shadow views

An active capture records `gpu.shadow_clear.ns` and `gpu.shadow_view.N.ns` for
each active atlas layer starting at `N = 0`. These are approximate GPU intervals
within `gpu.shadows.ns`. The clear and draw intervals can leave some attachment
transition time in the enclosing total. An unavailable or inconsistent view
readback emits `gpu.shadow_views.samples_dropped` with value 1 when the frame
recorded timestamps.

`renderer.shadow_view.N.draws`, `.batches` and `.triangles` report the retired
layer's submitted instances, actual draw commands and triangle instances.
`.reused` is 1 for an unchanged depth layer and 0 for a redrawn layer.
Reused layers have zero draw work. Their timestamp interval still includes
the boundary markers.
`.light_index` indexes the frame's effective light list. Match events by source,
session, tick and submission stamp before comparing layers. A layer can map to
a different light when active views change. View detail uses the same bounded
profiler capture as other events.

The [hierarchy counters](RUNTIME_HIERARCHY.md#inspect-reuse) report recomputed
and reused nodes for each completed traversal. Their sum is that pass's live
membership; multiple passes in one tick count the nodes again.

`render.retire` measures CPU polling, collection of completed frame results and related cleanup. It can occur without a wait. A nested `render.wait` appears only when an unfinished submission must complete before reusing a frame slot or draining outstanding work. These CPU scopes describe the current retirement operation; the delayed GPU events retain submission attribution separately. With render profiling requested, `render_diagnostics.cpu.completion_wait` summarizes the explicit completion-wait calls, excluding successful polls and the remaining retirement work. It is not a frame-time or presentation-latency measurement.

`renderer.submitted_submissions` and `renderer.outstanding_submissions` are recorded at submission, before presentation or report polling can retire work. They show logical queue depth at that point; they do not prove simultaneous GPU execution.

The [typed render schedule](RENDER_SCHEDULE.md) emits `render.pass.*` CPU scopes under `render.record` for the passes actually executed. These measure command recording, not GPU execution. Headless profiler regression checks verify required passes, nesting and fixed order on both laptop GPUs.

GPU pass observations distinguish skinning, light assignment, shadows, opaque rendering, post/output work and total queue duration. Light-assignment timing includes its upload and statistics-copy commands. Cluster counts and draw diagnostics describe the last retired submission, which may lag the latest submitted frame.

Scopes reserve complete-event slots before executing their bodies and fill durations on exit. The recorder allocates its fixed event array at start; recording does not allocate or throw. It uses the native monotonic clock and one owner thread. Nested bindings keep separate sessions isolated. Capture-array bytes exclude the recorder's lifetime identity, read/export buffers, GUI objects and engine allocations. Relative clock values saturate at the safe JSON integer limit after approximately 104 days, with `clock_saturated` set; such a capture no longer supplies accurate later timings.

## Native worker events

Worker callbacks never bind or write a Recorder. Submission captures a detached
recording generation and source/session/tick/parent. The shared executor records
bounded actual CPU intervals; collection on the owner routes each event to its
original active recording. Late work after stop, restart, destruction or capacity
closure is discarded. Collection under a different session does not relabel it.

`animation.sample.rig` measures one independent rig, including its ordered layers;
`runtime.animation.wait` measures the owner's wait/help operation. `material.bake`
measures compiled recipe baking and encoding, with publication measured on the
owner. Waiting and worker intervals can overlap; their sum is not frame time.
`jobs.status.trace_dropped` reports lifetime ring loss for the shared executor,
separately from a Recorder's first capacity overflow. Ring loss can span multiple
captures, so it is not assigned to the currently selected capture.

## Managed gameplay observations

Opt-in [C# callback metrics](MANAGED_CALLBACK_METRICS.md) report current-thread
allocated bytes and runtime-wide GC deltas around actual CoreCLR callbacks.
Cached reads are independent of native capture and authoritative state; disabled
and unsupported modules return explicit availability rather than zero counters.
These aggregates are not a heap profiler or a managed timeline in this window.

## Limits

This is an instrumentation profiler, not a complete production profiler. It does not sample call stacks, track arbitrary managed/native allocations or managed call stacks, sample arbitrary worker stacks, report total process/VRAM usage, calibrate GPU clocks, or replace the planned sustained benchmark workflow. Starting a capture can allocate several MiB; stopping/loading/exporting a large capture has a separate tool cost. Instrumentation and GPU timestamps affect the measured workload.

The current renderer implementation bounds outstanding frame submissions with `frames_in_flight` (one or two, default two). The one-slot reference path drains after each frame; captures also drain before publishing readback. Resource uploads and lifecycle operations can still wait independently. Two slots do not guarantee overlapping work or improved performance. Render wait/present intervals, GPU queue work, simulation ticks and full application frame time measure different boundaries. No reciprocal of one interval is advertised as a playable FPS result.

## Qualification

The [native jobs milestone](evidence/m2-native-jobs.json) qualifies actual worker
intervals, source/recording isolation, bounded trace loss and physical CPU thread
export on Linux and Windows. Its animation workload keeps timing samples
separate from profiler observations. The installed-client playbook and manual's
C++ example are checked against the same engine source checkpoint.

The [frame-retirement milestone](evidence/m2-frame-retirement.json) adds native deferred-recorder tests and original-submission attribution checks for all 49 changing frames per mode on both laptop GPUs. The evidence below records the earlier full profiler/editor checkpoint.

Run [the native capture test](../tests/profiler_capture.py) with
`--require-submission --require-barriers` to check submission identity in
CPU scopes, GPU timing, render diagnostics and trace export while checking
exact pixels and replay state. Add `--require-gpu-passes` to verify the
per-pass timings, including deferred lighting, ambient occlusion, half-resolution
diffuse bounce, exposure, bloom and capture readbacks. On a build with FSR,
add `--reconstruct-rich` to cover reconstruction and its input pass.

[Recorded evidence](evidence/m2-native-profiler.json) binds source and binaries to 45 runtime/37 authoring-only Linux suites, native recorder/service tests, real CoreCLR/Native AOT rollback/save/load traces on both OSes, exact GPU image/replay comparisons, and the final packaged editor. The [tool image](evidence/m2-profiler-window.png) is attached-window content; Windows was locked, so it is not an OS screenshot.

A native 128-box fixture compares the same instrumented binary with recording off/on over ten alternating pairs of 120 one-tick batches. Every pair preserves identical serialized logical state. Median paired overhead was 1.77% under Linux WSL2 and 0.51% on native Windows; individual pairs ranged from −4.33% to 18.29% and −7.10% to 5.05%, respectively. Trials lasted approximately 17–18 ms and include ordinary background noise. These are short workload observations, not stable overhead guarantees, compiled-out comparisons or shipped-game performance. Raw distributions and measurement boundaries are retained in the evidence.
