# GPU memory diagnostics

For a quick device check, run a Vulkan-enabled native build:

```sh
poima render-smoke --gpu 0 --frames 2 --width 640 --height 360
```

Read `result.memory_budget` in the returned JSON envelope. For a running game,
read `player.inspect` and inspect `report.render_diagnostics.memory_budget`. Other interfaces expose the same object at these specific paths:

- World-service scene capture: `render_diagnostics.memory_budget` in its result.
- CLI `render-smoke`: `result.memory_budget` in the JSON command envelope.
- Desktop bridge scene/game viewport inspection or capture: `render.memory_budget` within that view/capture result, once a render report exists.

These diagnostics are experimental and require a Vulkan-enabled build. A supported device reports every Vulkan memory heap: its capacity, flags, the process's estimated allocated bytes and the current process budget. This is useful when checking whether imported content or a rendering option changes allocation demand. The driver supplies these estimates through [Vulkan’s memory-budget query](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMemoryBudgetPropertiesEXT.html).

For agent workflows, first discover the actual build and start a normal player through the [shared player service](LIVE_PLAYER.md). Wait for `ready: true`; the start acknowledgement has no render report. Check field presence and the reported extension capability rather than assuming support from a version number. Read its exact owner/session/tick and select the diagnostic by that retained owner; do not infer an adapter from a machine-wide GPU index alone. Keep device UUID and heap index/flags with each sample. Read at a modest interval, and only add a new sample when its sequence/time changes. Compare ordinary content/configuration changes separately from saves, scene state and timing measurements. The read does not change authoritative game state.

For people investigating memory, collect the existing player report alongside the frame-performance capture, keeping process working set/private commit as separate measurements. The native reports expose the data; the editor does not yet have a dedicated memory meter.

Interpret the fields carefully:

- `extension_supported:null` means device initialization has not supplied the capability. `false` means the selected device lacks it. Neither is measured zero usage.
- `status:sampled` and `available:true` identify a validated query shape. A real zero allocation estimate remains integer `0`. Unsupported, awaiting or invalid observations use null estimates/budgets.
- `sample_sequence`, `sampled_at_ns`, `reported_at_ns` and `age_at_report_ns` identify the cached observation. Time uses the native engine steady-clock nanosecond domain with unspecified epoch; compare within the same process/context, not to UTC, GPU timestamps or another process. Age describes the report's creation; an old terminal response is not a live sample. `sampling_active:false` explicitly identifies a stopped/non-sampling report.
- `query_cpu_ns` measures elapsed host wall time around the synchronous driver query, including scheduling delays; it is not CPU execution time. Sampling is no more frequent than twice per second per context and is driven by owner reports, not a background worker.
- `heaps[].size_bytes` is hardware heap capacity. `allocated_estimate_bytes` is the current-process estimate, while `budget_bytes` can change due to other applications. Usage can exceed budget. The fields must not be renamed resident/peak memory.
- `device_local:true` does not imply dedicated VRAM: integrated GPUs can use shared system RAM. Multiple viewports on the same physical device report the same process scope, so do not add them together.
- An `invalid` query retains its reason and known heap capacities but clears estimated usage/budgets. Keep that observation and inspect a later sample; do not substitute an old value or zero.

This telemetry does not modify allocations, reserve memory, choose a graphics tier or evict resources. It does not measure every instant, attribute memory to an entity, establish a leak-free run, qualify the Alpha VRAM ceiling or replace actual frame-time measurements.

Memory observations are not saved game state. A stopped player returns a frozen terminal report with its original report time/age; reading it again does not issue a new Vulkan query. When sampling a new owner after loading a save, use that new owner/device identity and new observation times rather than expecting diagnostic continuity across processes.

## Qualification

[Checkpoint 0.0.152](evidence/m2-gpu-memory.json) records four paused native player
owners at 640×360 on the laptop’s AMD integrated GPU and RTX 4070 Laptop GPU.
Each owner supplies five distinct usable samples, with cached fields unchanged
under the same sequence. Eight complete native saved payloads remain byte-exact
across observation, stopping and separate capture; receipts, durable bytes and
original snapshot checksums agree. Terminal reports remain frozen. Separate
World captures and CLI smoke renders pass on both devices.

Seven Windows native suites and five Linux headless tests pass. CPU tests cover
unsupported and invalid samples, valid zero usage, clock/cadence behavior and
64-bit JSON values, including release builds with `NDEBUG`. Both actual devices
support the extension; unsupported-device coverage remains CPU-only. The Linux
rendering-disabled CLI returns unknown data with its expected rendering error.
Desktop bridge exposure is compiled and source-reviewed; this record does not
qualify embedded HostedViewport or legacy EditorViewport sampling.

The first hardware verifier attempt correctly hit the engine’s graphics-owner
guard when it requested World capture during attached player presentation. Its
failure is retained. The corrected sequence stops the player before the separate
capture. These short diagnostic runs do not establish a frame-rate target,
peak/resident VRAM, query-cost guarantee or representative workload budget.
