# Frame performance capture

Use the world service's `performance.*` methods to retain native player polls
and their GPU durations. This capture is independent of `profiler.*`: stopping
one does not stop or extend the other. It is intended for diagnosing cadence,
waits and interventions before evaluating a game workload.

## Capture a player

Start a shared host with `poima serve`, start its runtime and native player, and
wait until `player.inspect` reports `ready: true`. Connect through the CLI,
Python client or MCP. `world.describe` discovers the complete request schemas.

Start with a new 32-character lowercase hexadecimal capture ID, the current
player ID and the previous capture ID (`null` for the first capture):

```json
{"jsonrpc":"2.0","id":1,"method":"performance.start","params":{"capture_id":"00000000000000000000000000000001","expected_capture_id":null,"player_id":"00000000000000000000000000000002","capacity":32768}}
```

Capacity is 1–262,144 rows, allocated once before admission. An exact start retry
returns the retained capture without restarting it; changed retry parameters,
stale previous IDs and reused retired IDs fail. A previous capture must be
frozen before replacement. The player continues when the buffer fills: the
first rejected frame ends admission and reports `full` and `frames_dropped`.

`performance.status` returns bounded counts during collection. Avoid frequent
heavy authoring requests during a measurement: host work and scheduling gaps
remain part of observed cadence.

```json
{"jsonrpc":"2.0","id":2,"method":"performance.stop","params":{"capture_id":"00000000000000000000000000000001"}}
{"jsonrpc":"2.0","id":3,"method":"performance.summary","params":{"capture_id":"00000000000000000000000000000001"}}
{"jsonrpc":"2.0","id":4,"method":"performance.frames","params":{"capture_id":"00000000000000000000000000000001","offset":0,"limit":128}}
```

Stop closes admission, drains existing renderer submissions, then freezes the
capture. It does not advance simulation or present another frame. Drain failure
reports `drain_failed` and explicitly abandons unresolved GPU tickets; it does
not manufacture completed samples. After freezing, pages are immutable. Follow
`next_offset` until it is `null`; page limits are 1–512. Save returned pages
outside the timed run using your client. There is no per-frame JSON encoding or
file write in the recorder.

## Read the timings

Each row has an ordered sequence, CPU start/end, native tick boundaries, runtime
session, extent, flags and submission outcome. CPU timestamps are nanoseconds
relative to the capture epoch. `poll_wall_ns` measures the whole player poll;
`host_poll_gap` measures time between retained polls.

The named CPU stages are wall durations, including waits and scheduling, not
CPU execution cycles. They overlap: `render` includes `retire`, `acquire`,
`record`, `submit` and `present`; `gpu_wait` also occurs inside retirement and
presentation-fence handling. Do not sum them as exclusive frame costs.
`simulation_owner` includes committed ticks and their owner/audio/save work.

`present_return_ns` is stamped immediately after `vkQueuePresentKHR` returns,
before forced capture/serialized retirement. Consecutive successful or
suboptimal returns yield raw cadence, including intervening host gaps. An
out-of-date attempt is not a successful return, even if GPU work was submitted.
This measures application present-call return cadence, **not scanout FPS,
display completion or click-to-photon latency**. Vulkan allows the call to block
and delegates exact display timing to the presentation engine.
[Khronos reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html).

GPU timestamps are read after actual graphics execution completion. Values are
queue durations in their own clock domain, not calibrated spans on the CPU
clock. `gpu_ns.pass_mask` identifies measured passes, including valid zero
values: total, skinning, light assignment, shadows, opaque, deferred lighting,
AO, AO filtering, reconstruction and post occupy bits 0–9. The existing post
interval can overlap optional passes; do not sum pass durations.
[Khronos query specification](https://docs.vulkan.org/spec/latest/chapters/queries.html).

Accounting distinguishes:

- `gpu_submitted = gpu_completed + gpu_pending + gpu_dropped`.
- `gpu_completed = gpu_timed + gpu_unavailable + gpu_query_failed`.
- `gpu_dropped` means execution completion was not established.
- `gpu_not_submitted_unavailable` is a separate explicit observation with no
  submission; it cannot increase completed or submitted counts.

Capture, storage, resize, pause/focus changes, skipped presentation and runtime
replacement leave sticky flags for the following successful return. Explicit
screenshots are interventions, rather than normal recorded polls. The eligible
cadence summary requires focused endpoints and intervening rows, contiguous
sequences and no paused/replay/resize/capture/storage/skip/error/replacement flags.
Raw rows and raw intervals remain available; eligibility is a diagnostic filter,
not proof of steady-state performance. Replay advances one tick per presentation
and is excluded. FIFO presentation can limit cadence to display refresh.

A malformed CPU sample is retained as `cpu_failed`, increments `frames_failed`,
and is excluded from CPU/cadence distributions. Its session bytes are bounded
hex data rather than potentially invalid text. Pending GPU work still requires
retirement. Counter saturation and incomplete rows prevent broad performance
claims; inspect counts before using percentiles.

## Process memory

Run the separate sampler with **native Windows Python** while the measured
engine process is alive:

```powershell
python tools/performance/windows_process_memory.py --pid 1234 --duration 60 --interval 0.2 --system-ram --output memory.json
```

Replace the PID with your actual owner process. The sampler opens one read-only
handle, pins its creation time and never follows a reused PID. You can supply
`--expected-creation-filetime` from a prior trusted observation. It collects at
most five samples per second, has bounded duration/count, retains partial data
on failure and writes once after collection. Existing output files are rejected.

`working_set_bytes` is resident process working set, potentially including shared
pages. `private_commit_bytes` is private committed memory; it is not resident
RAM. Optional system counters report total and available physical RAM. Sample
extrema are observed values, not lifetime peaks or continuous measurements.
Child processes, WDDM residency and dedicated/shared GPU-memory use are not
measured. [Microsoft counter definitions](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex),
[access requirements](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo).

## Qualification

[Recorder tests](../tests/frame_performance.cpp) check delayed GPU retirement,
restart/stale tickets, allocation-free record paths, bounds and explicit failures.
[Service tests](../tests/frame_performance_service.cpp) check guarded retries,
immutable pages, raw/filtered intervals and safe failed-sample serialization.
[Memory tests](../tests/windows_process_memory.py) use injected providers/clocks.

The actual-window probe is separate:

```powershell
python tests/player_performance.py build/windows-runtime/poima.exe --gpu 1 --output build/player-performance
```

It uses a small physics fixture, actual Vulkan submissions and targeted resize
messages to its own window. It also checks profiler independence, paused ticks,
capture flags, full-buffer behavior and actual process memory. It does not
establish representative game FPS, physical device input, scanout latency,
clean-machine support, a memory leak-free soak or GPU residency.

[Performance Yard](../examples/performance-yard/README.md) supplies a separate
compiled game workload with a continuously navigated controller, additional
imported rigs, geometry, lighting and physics. Its benchmark checks the normal
interactive clock and completed draw work; it preserves missed budgets as
results. Its simple art and bounded runs do not close the full release gates.

The [0.0.95 AO comparison](evidence/m2-ao-optimization.json) repeats that workload
against retained reference and optimized binaries with identical settings.
AMD mean AO falls from 10.777 to 4.001 ms and mean total GPU duration from
19.584 to 13.416 ms; present-return p95/p99 are 18.625/19.486 ms. The p99 budget
passes while p95 still misses. NVIDIA mean AO falls from 1.079 to 0.902 ms, but
its measured cadence does not improve. These are short observed comparisons,
not a guarantee that reducing one GPU pass reduces present-return cadence.
Use the [paired capture verifier](AMBIENT_OCCLUSION.md#reproduce-the-paired-captures)
to check bounded image/probe equivalence separately from timing.

The [0.0.96 camera-report comparison](CAMERA_OBSERVATION.md) removes a redundant
full scene extraction at the report boundary while retaining rendering and
host waits. Both GPUs show lower mean host gaps. NVIDIA p95 is effectively
unchanged; AMD p95 improves in the recorded pair but still misses its target.
GPU durations also vary, so the entire cadence difference is not attributable
to the CPU query alone. [Raw observations](evidence/m2-camera-observation.json).
