# Native profiler

Poima can record bounded native CPU work, renderer GPU durations and selected resource counters. The editor and command service inspect the same session-owned capture. Profiling is opt-in and independent of authored documents, game state, saves and rollback.

## Editor

Open **Window → Profiler** and press **Record** (16,384 events). Run the workload, then press **Stop** to load the capture. Recording does not start Play or otherwise change simulation. Closing this tool does not stop a capture owned by the shared session.

The selectable CPU timeline shows individual scoped operations. Filter by source to distinguish editor polling, Scene, Game and standalone player work. Select a span to inspect its parent, runtime session, tick, elapsed time and failure flag. Use the duration summary to sort aggregate, mean, p95 or maximum cost. Timeline display is paged; the native capture and export retain all recorded events.

CPU scopes measure elapsed wall time, including waits and scheduling delays; they are not sampled on-CPU utilization. Scopes are inclusive: a parent includes its children, so summing rows double-counts nested work. The editor records native polling and viewport work, not total Avalonia layout/rendering, managed UI allocations or operating-system scheduling. A simulation tick is not a presented frame.

GPU rows report timestamp-derived durations observed after the existing graphics-queue completion wait. They are **not aligned to the CPU timeline**. Resource counters show last/min/max/mean values rather than sums. Texture payload and skin-buffer bytes are partial tracked resources, not total GPU residency or VRAM usage.

**Export** writes a new Chrome trace-event JSON file. Existing files are preserved. The native export can also be opened in viewers supporting that format, including [Perfetto](https://perfetto.dev/docs/getting-started/other-formats). CPU events export as complete spans; GPU durations export as counter observations. Source lanes are logical labels on the owner thread, not separate OS threads.

## Command service

Discover schemas with `world.describe` (revision 29). All commands work through a persistent `world`, shared headless host or editor session; capture is lost when its owning session closes.

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
| `profiler.start` | New 32-character lowercase hexadecimal ID, observed `expected_capture_id` (null initially), capacity 64–65,536; default 16,384. Allocates before replacing a stopped capture. |
| `profiler.stop` | Requires current capture ID; repeat is harmless. Stops accepting records while existing scopes finish. |
| `profiler.status` | Current identity/state, recording flag, capacity, count, open scopes, loss indicator, elapsed ns and recorder-array bytes. Does not advance simulation. |
| `profiler.events` | Sealed capture only; offset 0–count, limit 1–1,024, default 256. Returns total and nullable next offset. |
| `profiler.summary` | Sealed capture only; groups by name, kind and source. Reports sample count, failed count, last/min/mean/max and nearest-rank p50/p95/p99. |
| `profiler.export` | Sealed capture only; complete trace-event document plus timing and scope metadata. Export allocates outside measured work. |

Starting again with the current ID and identical original parameters returns the current capture without resetting it. Changed parameters reject. Replacing a capture requires stopping it and supplying its observed ID. IDs cannot be reused during the owner's lifetime; the session supports up to 4,096 distinct captures. A stale read/stop cannot act on a replacement capture.

When the next event would exceed capacity, recording automatically stops. The retained prefix and already-open scopes remain valid. `dropped` includes the first rejected reservation; subsequent work uses the disabled path and is not counted individually. `full` therefore means a truncated workload, not a complete count of every missing event. Reading waits until recording is false and open scopes are zero. Captures never overwrite their oldest entries.

## Recorded data

Each event has a capture-local ID, parent ID, bounded name, kind (`cpu`, `gpu`, `counter`), source, optional runtime session and tick, relative `start_ns`, `duration_ns`, value and failure/truncation flags. Children inherit tick/session context unless explicitly changed. Source and session are recorded at entry; a parent spanning a load describes its original context. Later work uses the restored session, so rewound ticks remain distinguishable.

CPU failure flags indicate exception unwinding, not a complete semantic classification of every operation error. Work performed before a rolled-back batch remains in the diagnostic trace. It does not mean that simulation state committed. Scope parentage identifies the enclosing batch; there is no separate permanent gameplay event log.

Native instrumentation covers runtime batch validation/checkpointing, fixed ticks, C# calls and command application, physics, animation sampling, hierarchy synchronization, rollback, owner advancement, snapshot/save preparation, renderer preparation/acquire/record/submit/present/wait/readback, player presentation and native audio queue/DSP boundaries. Only paths actually executed produce samples. Native audio device output remains a standalone-player feature; editor audio is still unfinished.

Scopes reserve complete-event slots before executing their bodies and fill durations on exit. The recorder allocates its fixed event array at start; recording does not allocate or throw. It uses the native monotonic clock and one owner thread. Nested bindings keep separate sessions isolated. Capture-array bytes exclude read/export buffers, GUI objects and engine allocations. Relative clock values saturate at the safe JSON integer limit after approximately 104 days, with `clock_saturated` set; such a capture no longer supplies accurate later timings.

## Limits

This is an instrumentation profiler for the current synchronous engine, not a complete production profiler. It does not sample call stacks, track managed GC or arbitrary native allocations, trace worker threads, report total process/VRAM usage, calibrate GPU clocks, or replace the planned sustained benchmark workflow. Starting a capture can allocate several MiB; stopping/loading/exporting a large capture has a separate tool cost. Instrumentation and GPU timestamps affect the measured workload.

Current rendering serializes submissions and waits for GPU completion. Render wait/present intervals, GPU queue work, simulation ticks and full application frame time measure different boundaries. No reciprocal of one interval is advertised as a playable FPS result.

## Qualification

[Recorded evidence](evidence/m2-native-profiler.json) binds source and binaries to 45 runtime/37 authoring-only Linux suites, native recorder/service tests, real CoreCLR/Native AOT rollback/save/load traces on both OSes, exact GPU image/replay comparisons, and the final packaged editor. The [tool image](evidence/m2-profiler-window.png) is attached-window content; Windows was locked, so it is not an OS screenshot.

A native 128-box fixture compares the same instrumented binary with recording off/on over ten alternating pairs of 120 one-tick batches. Every pair preserves identical serialized logical state. Median paired overhead was 1.77% under Linux WSL2 and 0.51% on native Windows; individual pairs ranged from −4.33% to 18.29% and −7.10% to 5.05%, respectively. Trials lasted approximately 17–18 ms and include ordinary background noise. These are short workload observations, not stable overhead guarantees, compiled-out comparisons or shipped-game performance. Raw distributions and measurement boundaries are retained in the evidence.
