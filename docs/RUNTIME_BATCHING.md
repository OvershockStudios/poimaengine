# CPU stepping and observation costs

Checkpoint 0.0.187 compares explicit one-tick and sixty-tick stepping in the
complete Performance Yard. The same sealed 0.0.186 Windows executable and
qualified 0.0.185 CoreCLR bridge run both treatments. The investigation identifies
work repeated at each batch boundary before changing the runtime.

The [earlier CPU memory investigation](MANAGED_MEMORY.md#full-workload-investigation)
used sixty-tick calls, while the native player advances one tick at a time.
Removing rendering also changed batching and observation frequency. Comparing
those cohorts cannot isolate rendering or CLR memory behavior.

## Workload and schedule

The unchanged world has 2,394 authored entities, 24 animated characters and
64 lights. Original compiled gameplay creates three more live entities and
retains all fifty globals, four component schemas and fourteen checkpoint
payload members. The sixteen-file world/asset closure remains intact.

Six fresh native owners run serially in this fixed order:

| Pair | First owner | Second owner |
| --- | --- | --- |
| 1 | One tick per call | Sixty ticks per call |
| 2 | Sixty ticks per call | One tick per call |
| 3 | One tick per call | Sixty ticks per call |

Each owner warms up for 120 ticks, then records five consecutive 120-tick
windows. Each treatment contributes 1,800 measured ticks. The complete run
contains 4,320 committed ticks and thirty profiler captures. No rendering,
compilation, reload, explicit collection or gameplay edits occur during it.
The worker takes 79.174 seconds; this is a short, unpaced CPU diagnostic.

The laptop is the Ryzen 7 8845HS development machine with 16 GB RAM. Power
observations immediately before and after report AC power and 100% battery.
The worker sets `DOTNET_PROCESSOR_COUNT=2` and `POIMA_JOB_WORKERS=2` for both
treatments. Native animation work uses the shared executor; Jolt uses its
single-threaded job system in this build. This is not a physics-scaling test.

The RPC helper inspects the current runtime before every step request. Grouping
sixty ticks therefore reduces both guard-query frequency and transaction-boundary
work. Profiling is enabled in both treatments. Cache, scheduling, thermal and
ordinary GC effects are not isolated.

## Measured boundaries

These are total inclusive durations divided by committed measured ticks,
averaged across the three owners of each treatment. They are neither frame
times nor mutually exclusive pieces of a frame. Nested regions overlap, and
animation worker intervals can run concurrently; do not add the rows.
CPU scopes measure elapsed wall time, including waits and scheduling delays,
rather than sampled on-CPU utilization.

| Measured region | One-tick calls, ms/tick | Sixty-tick calls, ms/tick |
| --- | ---: | ---: |
| Native `runtime.batch` | 2.470 | 0.826 |
| Native `runtime.checkpoint` | 0.1572 | 0.002889 |
| Native `runtime.tick` | 0.939 | 0.801 |
| Native `runtime.sync` | 1.338 | 0.510 |
| Native `runtime.hierarchy` | 1.252 | 0.475 |
| Native `runtime.animation.sample` | 0.839 | 0.278 |
| Guard `runtime.inspect` | 5.453 | 0.08785 |
| External RPC helper loop | 19.437 | 1.115 |

The guard row is normalized per tick: it makes 1,800 calls in the one-tick
treatment and thirty calls in the sixty-tick treatment. It does not say that an
individual grouped-treatment inspection takes 0.08785 ms. The helper-loop row
also includes Python, transport, response validation and retained trace writes.
It does not measure the continuous native player's frame time or throughput.

Per 600 measured ticks, one-tick calls produce 600 checkpoint regions,
1,200 animation-sampling regions and 2,400 synchronization/hierarchy regions.
Sixty-tick calls produce ten, 610 and 1,220 respectively. Both execute 600
`runtime.tick` regions. The reduction in the instrumented checkpoint region
is about 9.4% of the observed inclusive batch-time difference. This arithmetic
does not identify the cause of the remaining difference. The checkpoint scope
also excludes earlier structure-checkpoint and previous-motion preparation.

All three pairs show the same broad direction. Their native batch means are
2.450/0.817, 2.536/0.856 and 2.424/0.806 ms per tick, expressed as
one-tick/sixty-tick in each pair. Full selected trial statistics, including
per-invocation percentiles, are in the [evidence](evidence/m2-runtime-batching.json).

## Complete state and process ownership

The original 4,788 request/response pairs, native stderr, thirty full profiler
exports and thirty-six complete v7 checkpoint references are retained. Full
snapshots are archived losslessly before normal save-generation retirement.
Independent readers join every request, reconstruct profiler statistics from
all 204,450 exported events and verify zero overflow, unfinished regions or
failed steps in the selected captures.

Fresh owners receive different random save epochs. Gameplay copies those epoch
halves into two signed-int64 string fields. The original cross-owner payloads
therefore differ. Before the counterpart's first tick, each comparison freezes
a complete expected payload using the already observed destination epoch and
the source-derived field mapping. Eighteen such forecasts match complete
typed and lexical payloads; every other field remains included. No actual
checkpoint, receipt or observed value is rewritten. This is full forecast
agreement, not raw cross-owner byte equality or a general determinism proof.

All six native owners reach EOF and exit zero. The enclosing Windows Job records
seven total processes, zero active and zero terminated at closure, without
forced cleanup. Final owned output contains 139 files totaling 99,815,023 bytes.
Earlier file-only preparation/reader errors remain separate failed attempts;
their corrected readers examine the same unchanged native results.

## Implications and reproduction

The strongest authoring-core candidate is repeated source-state comparison in
`runtime.inspect`: its scalar summary evaluates `same_authored_state` over the
large authored document on each read. Caching the exact result would require
invalidation on every relevant authored/runtime document publication, including
same-revision replacement. A revision-only cache would change semantics.
Separate animation-boundary work needs its own history, IK and rollback checks.
This checkpoint implements neither optimization.

For a new measurement, use the supported [fixed-step runtime](RUNTIME.md) and
[profiler](PROFILER.md): freeze workload, artifact identities, call schedule,
worker counts and measurement windows before execution. Retain original requests,
responses and complete saves. Distinguish per-call statistics from per-tick
normalization, and preserve failures and owner closure. A different batch size
changes transaction atomicity, input opportunities and observation frequency;
sixty-tick calls are not a replacement for responsive interactive stepping.

Exact reproduction of this cohort requires the retained world/assets, original
compiled game closure, selected bridge/SDK closure, hostfxr, schedule and original
traces. It is not a clean-checkout benchmark. The original game SDK and selected
bridge SDK have different hashes; both are pinned separately. CoreCLR unifies
the SDK to the bridge's assembly, and that actual retained-game combination has
separate native-host qualification. Equal ABI numbers alone are insufficient.

This result does not qualify FPS, input latency, ordinary GC timing, memory-growth
causes, observer overhead in rendered play, thirty-minute reliability or the
broader Alpha gate. Those requirements remain in the [Alpha roadmap](ALPHA_ROADMAP.md).
