# Process-memory timelines

Tooling 0.0.160 can record process-memory query intervals and reliability-harness
phase boundaries in the same integer clock. This helps separate observations
during play, saving, loading, capture and cleanup. It does not identify allocation
causes, establish a leak or close the Alpha memory-growth gate.

## Record a timeline

Add `--record-memory-timeline` to the existing
[Performance Yard reliability command](PERFORMANCE_YARD_RELIABILITY.md#run-the-check).
The option also works through its diagnostic wrapper. Use the same prepared
inputs, native Windows Python, owned process tree and available foreground
window required by that check. Enabling recording does not relax its gameplay,
focus, clock, save-state, duration or cleanup requirements.

Recording is off by default. The default sampler retains its version-1 report
and existing scheduling. Enabled recording adds no native request or sampling
thread, and keeps the existing one-second memory sampling interval. Its
additional clock reads, bookkeeping and final serialization have not been
measured during representative engine play.

The final `evidence.json.memory_timeline` contains:

- `recording_passed`, bounded diagnostic errors and explicit omissions.
- `phase_report`, the ordered phase marks and a separate closing horizon.
- `owners`, identifying each actual RAM report file and its per-query association.
- Byte accounting for the pretty-printed timeline plus referenced RAM files.

The existing owner RAM reports retain all admitted counters. A failed file
publication can retain its partial report inline instead. An unfinished sampler
is reported as unavailable; its live result is not read concurrently. Recording
errors preserve the original native failure and cleanup, but make the requested
recording and overall check fail. Inspect the recording verdict as well as the
gameplay results.

## Clock and assignment rules

Version-2 `poima.windows-process-memory` reports add a `clock` containing
`domain: "python.time.monotonic_ns"`, `collector_pid` and `origin_ns`.
`sampler_started_ns` records the sampler's own start. Each admitted sample has
`query_begin_ns` and `query_end_ns` surrounding the entire provider call,
including process identity and optional system-RAM queries. The one retained
`last_query` also describes a final failed or interrupted attempt; it is not an
admitted counter sample.

The collector shares one origin across owner samplers and the phase log.
Legacy `elapsed_seconds` remains the local scheduling timestamp taken before
the query. It is not converted to nanoseconds or used to associate phases.
Slow queries retain their full actual interval. Invalid clocks retain valid
partial observations and a bounded error; no timestamp is repaired.

An association requires matching clock domain, collector PID and origin, a
closed valid trace, and an observed owner PID and process creation FILETIME.
Creation FILETIMEs are canonical positive decimal strings. Player/session,
generation, replacement count, cycle and explicitly observed tick are optional
tags. A cached tick is not reported as current at a later boundary.

Queries must lie wholly inside one phase interval. A query ending exactly at
the next boundary, spanning a boundary, touching a tied boundary, preceding the
first mark or reaching the closing horizon remains unassigned. Unknown or
mismatched process identity also remains unassigned. Valid legacy reports,
incomplete traces and mismatched clocks explicitly report unsupported
association; malformed typed reports raise an error. Older measurements cannot
be retrospectively assigned phases from elapsed floats.

Phase names describe harness operations and request boundaries, rather than an
exhaustive trace of internal engine work. An assigned sample does not prove
which subsystem allocated memory. The measured counters remain one process's
working set and private commit, including planned pauses. They exclude child
processes and do not measure GPU residency.

## Bounds and recovery

The phase log permits at most 4,096 marks and 1 MiB of canonical compact UTF-8
JSON. A clock, tag or limit error retains its valid prefix and marks the trace
incomplete; it never silently evicts events. The soak recorder retains at most
two owner samplers and sixteen diagnostic errors, counting further errors
separately. Its combined timeline and referenced RAM-report budget is 16 MiB.
Oversized associations or inline partial reports are explicitly omitted and
the recording fails; original RAM files remain available.

For an isolated sampler, use native Windows Python:

```powershell
python tools/performance/windows_process_memory.py --pid <PID> --duration 10 --interval 1 --query-timing --output build/memory-query.json
```

The output must be new and its parent must already exist. An optional
`--clock-origin-ns` requires query timing and must come from the same collector's
clock. Do not reuse an origin from another process or invent an epoch for old
data. A standalone sampler without a phase log records intervals only.

## Verification

Portable contract checks use injected clocks and providers:

```sh
python tests/windows_process_memory.py
python tests/memory_phases.py
python tests/performance_yard_memory_timeline.py
```

A separate finite smoke probe reads real Windows counters from its own Python
process. It requires no engine, foreground window or GPU:

```powershell
python tests/windows_memory_timeline_smoke.py --output build/memory-timeline-smoke.json
```

Its seven queries include two deliberately crossed phase boundaries. Those
phase names are test labels, rather than actual engine gameplay. This probe
qualifies the Windows clock/counter/association path only. Sustained mixed-game
reliability and integrated recording overhead still need qualification.

## Background-game integration

The [0.0.166 native check](evidence/m2-background-player.json) connects this
recorder to two real native-game process identities using
[background elapsed-time play](LIVE_PLAYER.md#background-play-for-agents).
The complete Performance Yard runs five intervals totaling 19.132 seconds,
with warm and fresh-process continuation and exact complete saves. Its 29
actual OS queries join 69 phase marks in one collector clock; every query
fits a single observed phase in this run. Boundary-crossing queries remain
unassigned under the existing policy.

The largest queried working set is 311,644,160 bytes and the largest queried
private commit is 590,794,752 bytes. These are sparse observations, not
continuous peaks or memory budgets. Startup, playback and storage phases
are mixed, and the warm working set increases during this short run. No
leak cause, plateau, instrumentation-overhead bound, peak VRAM or thirty-minute
reliability qualification is established. The separate focused-player
requirements remain unchanged.

The [0.0.168 CoreCLR integration](PERFORMANCE_YARD_CORECLR.md) records 41 actual
queries and 67 phase marks through compile/error/repair, reload and warm/cold
restoration in the complete Performance Yard. It preserves native module and
process lifetimes separately and uses conservative request brackets for observed
simulation cadence. This extends short integration coverage; it does not qualify
recording overhead, continuing growth, continuous memory peaks or thirty-minute
reliability.
