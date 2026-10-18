# Managed memory and context observations

`runtime.gameplay.memory` reads CoreCLR memory estimates and collectible-context
counts without requesting garbage collection. It complements
[callback allocation metrics](MANAGED_CALLBACK_METRICS.md) and the
[process-memory timeline](MEMORY_TIMELINE.md). These measure different things;
do not add them together as a total or treat allocation traffic as retained
memory.

## Read a snapshot

Discover the installed method schema through `world.describe`. With a running
simulation, supply its `session_id` and optionally its current integer `tick`:

```json
{"jsonrpc":"2.0","id":1,"method":"runtime.gameplay.memory","params":{"session_id":"0123456789abcdef0123456789abcdef","tick":120}}
```

The result contains `session_id`, `tick`, gameplay `revision`,
`structure_revision` and `module`. An unloaded runtime returns `module: null`.
A stale tick rejects with `-32009`; invalid request fields or types reject with
`-32602`. Bridge failures or malformed diagnostic data reject with `-32060`.
Reads are allowed during background play; that play can advance independently
between requests. The read itself does not simulate, reload, release contexts,
prune tracking entries, reset counters, or change authoritative state.

The module wrapper has format `poima.gameplay-memory-snapshot.v1`, version 1,
`backend`, game `identity`, `assembly_sha256`, `supported`,
`unavailable_reason` and `data`:

| Loaded module | `supported` | `unavailable_reason` | `data` |
| --- | --- | --- | --- |
| CoreCLR with negotiated bridge | `true` | null | Observed snapshot |
| CoreCLR using an older bridge | null | `bridge_not_negotiated` | null |
| Native AOT | `false` | `native_aot_not_supported` | null |

The native loader offers internal `memory_snapshot_version: 1` metadata to
CoreCLR. A matching bridge echoes version 1; an older bridge can ignore the
offer and continue loading gameplay. The observer is independent of the
`profile_callbacks` setting. There is no speculative operation call to an old
bridge and no automatic bridge replacement. Host and bridge paths stay pinned
for the process lifetime; restart the process to select another bridge.

This evolving query is outside the frozen authoring-core and runtime-game-session
v1 contracts. It changes neither the gameplay C ABI nor checkpoint formats.
Snapshots are diagnostics outside saved state and rollback, so full checkpoint
comparisons still retain every authoritative field and byte.

## Interpret the data

`data` has format `poima.managed-memory-snapshot.v1`, version 1. It contains:

| Field | Meaning |
| --- | --- |
| `process_id` | CLR process ID. Keep the external process creation identity when joining captures; a PID can be reused. |
| `timestamp_start`, `timestamp_end`, `timestamp_frequency` | Monotonic `Stopwatch` bracket around sampling, in process-local timestamp ticks and ticks per second. Serialization and transport occur outside the bracket. |
| `active_modules` | Modules currently registered in this bridge, across its runtimes; at most 32. |
| `retired_contexts_tracked` | Retired weak-reference entries retained by this bridge; at most 1,024. Sampling does not remove dead entries. |
| `retired_contexts_alive` | Entries whose weak references were observed alive during this read. This is not proof of a leak or an inability to unload. |

These counts cover this bridge's registry, not every assembly load context in
the process. A failed candidate load can add a retired entry without changing
the selected gameplay module. Reload and restore can also retire contexts.
Retirement requests unloading; it does not guarantee immediate reclamation.

The `gc` object contains:

| Field | Meaning |
| --- | --- |
| `total_memory_bytes` | `GC.GetTotalMemory(false)` estimate of allocated managed memory, without waiting for collection. It is not an exact reachable-object census. |
| `total_allocated_bytes` | Approximate process-wide managed allocation traffic from `GC.GetTotalAllocatedBytes(false)`, including other managed threads. |
| `gen0_collections`, `gen1_collections`, `gen2_collections` | Runtime collection counts observed during sampling. They do not identify a cause. |

The `last_gc` object reports the raw latest available `GC.GetGCMemoryInfo()`:
`index`, `generation`, `concurrent`, `compacted`, `heap_size_bytes`,
`fragmented_bytes`, `total_committed_bytes`, `memory_load_bytes`,
`high_memory_load_threshold_bytes` and `total_available_memory_bytes`.
Heap, fragmentation, committed and memory-load bytes describe historical GC
observations, not synchronized live heap measurements. The threshold and
available-memory fields describe runtime limits and can already be nonzero
before any collection. An index of zero means there is no completed-GC history;
zero-valued historical fields do not prove an empty heap. The
[CoreCLR implementation](https://github.com/dotnet/runtime/blob/v10.0.0/src/coreclr/gc/gc.cpp#L47911)
populates the limits independently of the last collection. Do not require
successive latest-GC indices to increase or infer that the current query caused
the reported GC.

Byte quantities, timestamp values, collection counts and the GC index use
canonical unsigned decimal strings. Registry counts, `process_id`, `version`
and GC `generation` are JSON integers; `concurrent` and `compacted` are Booleans.
The native boundary checks exact field membership and types, canonical ranges,
context-count bounds and timestamp ordering within an 8 KiB response budget.
Malformed observations fail instead of becoming zero-valued measurements.

## Observation effects and limits

Sampling and JSON serialization allocate and take time. Ordinary CLR collection
can occur during a read, even though the observer never calls `GC.Collect`,
waits for finalizers, or requests `GC.GetTotalMemory(true)`. Values are sampled
sequentially and are not an atomic census. Use sparse phase-boundary reads and
measure observation cost with a matched workload before assuming it negligible.

Keep engine/module hashes, session, revisions, tick, process identity and phase
with each original result. Join these observations to independent process and
GPU measurements. A stable managed heap does not prove stable native memory;
a retained process high-water mark does not by itself establish a managed leak.
The separate `runtime.gameplay.collect` operation deliberately forces collection
and must be labeled as a diagnostic intervention outside passive measurements.

## Check the implementation

The `managed_memory_json` CTest exercises the actual native wire parser without
a CLR runtime. The compiled helper in
[tests/managed_memory_snapshot](../tests/managed_memory_snapshot/) exercises the
real bridge load, query, retirement and negotiation paths. Native hosted RPC and
complete-save preservation require separate execution with the selected engine,
bridge and game artifacts. Helper checks alone do not qualify that integration,
the cause of memory growth, observer overhead, or the Alpha reliability gate.

## Full-workload investigation

Checkpoint 0.0.183 exercises the observer with the sealed 0.0.182 Windows engine,
the matching bridge and the original compiled Performance Yard gameplay. Its
2,394 authored entities include 24 animated characters and 64 lights; three
compiled template spawns bring live membership to 2,397. All fifty globals and
four component schemas remain intact.

This CPU-only run submits 60-tick batches on a one-second external schedule:
30 seconds of warmup, twelve five-second cycles with six compatible body reloads
and twelve compiled save/load operations, then six thirty-second intervals of
unchanged advancing simulation. It uses no player window. This is explicit
headless stepping, distinct from the native elapsed-time background player.
Callback profiling stays off. Passive CLR queries do not request collection or
prune retired entries.

The 306.202-second worker retains 1,144 original RPC pairs, forty complete v7
checkpoint references, fourteen whole-byte/type pairs and six complete lexical
reload forecasts. A fresh owner restores the cycle-12 save before callbacks and
continues gameplay. Thirty-four CLR snapshots accompany 304 OS queries; the six
fixed tail windows contain 179 whole eligible queries. Both game owners and the
three-process supervising Job close naturally.

At the final warm query, all eighteen tracked retired contexts are still observed
alive, with no generation-1 or generation-2 collection during the run. This leaves
collection-assisted unloadability untested. The managed-memory estimate grows
from about 3.4 to 6.4 MB during the tail; first-to-last window median process
private commit grows by 1,441,792 bytes. These are different measurements and do
not establish an allocation cause or leak. The historical rendered cohort,
observation overhead and broader Alpha gates remain separate.

The independently checked [results](evidence/m2-managed-memory-workload.json)
retain all six windows and the earlier failed file-audit records. Exact cohort
reproduction requires its retained world, compiled images, bridge/dependency
closure, schedule and original traces; this is not a clean-checkout benchmark.
For a new investigation, retain complete save operands and original observations,
declare the workload and windows before running, and label explicit collection
as a separate intervention.

## Explicit collection diagnostic

`runtime.gameplay.collect` accepts the current `session_id` only. It has no tick
guard, request ID or replay receipt. For an initialized CoreCLR bridge it may
run up to eight rounds, each calling `GC.Collect`, waiting for finalizers, then
calling `GC.Collect` again. It checks remaining weak contexts between rounds.
It counts `active_modules` and `retired_alive`, prunes dead weak entries, then
returns the counts. The count and pruning passes are separate; a later query
can see fewer entries. A Native AOT host, if initialized, has its own subsection.

Use this as an explicit investigation outside performance measurements. An
external diagnostic owner supplies the wall-clock limit; a bounded round count
does not bound arbitrary game finalizer execution. Retain the original result or
unknown outcome rather than automatically repeating the intervention. Ordinary
memory queries continue to request neither collection nor pruning.

Checkpoint 0.0.184 uses the same sealed 0.0.182 engine and bridge in a separate
full Performance Yard process. Two compatible precompiled body replacements
and two compiled warm restores create four tracked retired contexts. Exactly
one collection request sits between complete saved-state operands, with no Tick,
control callback or reload between them. Both reloads also have complete lexical
state forecasts; both restores are compared before callbacks update the epoch.

| Observation | Before | After |
| --- | --- | --- |
| Active modules | 1 | 1 |
| Retired entries tracked / observed alive | 4 / 4 | 0 / 0 |
| Generation 0 / 1 / 2 collection counts | 0 / 0 / 0 | 2 / 2 / 2 |
| Managed-memory estimate, bytes | 3,653,976 | 673,872 |

The worker closes naturally in 6.978 seconds. Independent review checks all 112
original RPC pairs, nine complete v7 checkpoint references, three whole-byte/type
pairs and two full reload forecasts. All fifty globals, four component schemas
and fourteen saved payload members remain intact. The source has 2,394 entities;
three normal template spawns bring live membership to 2,397. The two-process Job
is empty without forced termination. Original output is 29,366,948 bytes, within
the prospective 32 MiB limit.

These observations demonstrate reclamation of the selected four contexts without
changing current authoritative state. They do not guarantee arbitrary user-code
unloadability, establish ordinary collection timing, explain the earlier rendered
memory growth, or qualify performance. The passive 0.0.183 run remains separate.
Nonzero retention would also be a diagnostic result, with no retry to force zero.
[Original observations and limits](evidence/m2-context-unloading.json).
