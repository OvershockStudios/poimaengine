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
| `retired_contexts_tracked` | Retired weak-reference entries retained by this bridge. The accepted snapshot range is 0–1,055, covering releases of already admitted modules; see [retirement bounds](#retirement-observation-bounds). Sampling does not remove dead entries. |
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

## Retirement observation bounds

From 0.0.190, the managed serializer and native parser accept up to 1,055 tracked
retired contexts. This is an observation limit, separate from the unchanged load
admission limits of 32 active modules and 1,024 retired entries.

On the serialized owner thread, a new load can start with at most 31 active
modules and 1,023 retired entries. Creating one context brings their combined
count to at most `31 + 1,023 + 1 = 1,055`. Release transfers an admitted context
to the retired list; it does not create another context. Releasing all admitted
modules can therefore leave 1,055 retired entries. With one live observer still
registered, at most 1,054 can be retired. The parser's global upper bound does
not mean that a live query can report 1,055.

Passive observation neither removes entries nor collects contexts to fit a
budget. Alive counts remain bounded by the actual tracked count. All existing
field, type, canonical-counter, clock and 8 KiB wire checks remain in effect.
This change affects diagnostic availability rather than game load capacity,
state serialization or whether retired code can be reclaimed.

The query remains an evolving version-1 diagnostic outside the frozen authoring
and game-session contracts. Both the bridge and native parser need the expanded
bound: older consumers can still reject counts above 1,024. The gameplay C ABI,
checkpoint formats and optional negotiation field are unchanged; no broader
mixed-version compatibility is implied.

The [compiled lifetime fixture](../tests/managed_retirement_bounds/README.md)
builds the boundary from actual admitted modules and releases, holding their
real collectible contexts in a no-inline test frame. Its controlled roots make
liveness reproducible; they do not demonstrate a game leak. Native parser unit
cases at 1,055 are synthetic wire checks, distinct from a live 1,054-context
query and from the final 1,055-entry registry after its observer is released.

### Recorded checks

The 0.0.190 Windows and Linux runs each execute the same lifetime sequence in
separate retained-bridge and corrected-bridge processes. Each process accepts
and releases 1,055 real contexts and retains 2,122 literal bridge calls. The
retained bridge returns its original bounds error above 1,024. The corrected
bridge returns complete observations at 1,025 and 1,054, including repeated
reads that preserve all twelve gameplay-state bytes. Initialize and a subsequent
Tick both execute successfully. One explicitly labeled collection call follows
the root frame's return; it establishes none of the decisive preconditions.

Both native parser executables pass 349 synthetic cases and replay both actual
1,054-context DTOs without changing any field. Core conformance and metadata
checks also pass. A separate Windows hosted regression checks current CoreCLR,
an older nonnegotiating bridge and Native AOT through 206 original RPC pairs,
fourteen complete checkpoint references and six complete saved-byte equality
pairs. Its smaller live registry does not qualify native world hosting at the
1,054-retirement boundary.

All original owners close naturally without forced cleanup. The gameplay SDK
binary is reused; only the bridge and native parser need this correction.
[Selected results and artifact identities](evidence/m2-managed-retirement-bounds.json)
record the distinction between actual lifetime observations, synthetic wire
boundaries and hosted-world regressions. The fixture README gives reproduction
steps; exact historical comparisons require the identified retained binaries.

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

The later [CPU batching investigation](RUNTIME_BATCHING.md) keeps the original
full workload and compares one-tick with sixty-tick calls. It demonstrates that
batch size changes rollback preparation, animation/hierarchy work and observer
frequency. Consequently, the CPU-only cohort above cannot isolate the effect of
removing rendering from the earlier mixed workflow. The batching investigation
measures timing rather than CLR retention or memory-growth causes.

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

## Load capacity recovery

From 0.0.185, load admission can recover when ordinary GC has reclaimed retired
contexts but their weak-reference entries still fill the tracking budget. If
fewer than 32 modules are active and at least 1,024 retired entries are recorded,
the bridge removes entries already observed dead before checking the existing
capacity guard. It does not scan this list on every load below capacity.

This pass requests no collection, finalizer wait or unloading. Passive memory
queries still neither collect nor prune; the explicit collection operation
remains a separate intervention. Removing a dead weak entry is bookkeeping,
not evidence that this load caused its context to be reclaimed.

The original refusal remains when 32 modules are active or at least 1,024
entries remain after the capacity pass:
`Managed context budget reached; collect diagnostics or restart the session.`
Release active owners where appropriate, use the separately bounded explicit
collection diagnostic, or restart the process. Collection cannot force a context
to unload while game code or another owner retains it.

The 0.0.185 capacity qualification does not cover releases of multiple already
admitted modules above 1,024 retired entries. The separate 0.0.190
[observation bound](#retirement-observation-bounds) covers that diagnostic range;
load admission and its dead-entry recovery remain unchanged.

The [focused fixture](../tests/managed_reload_capacity/README.md) builds its
capacity precondition from 1,024 real accepted loads and releases. Controlled
strong roots keep those contexts alive until a no-inline test frame returns;
test-host GC then establishes dead entries without invoking bridge operation 5.
Repeated passive queries must preserve that history. The retained bridge rejects
the next valid load, while the corrected bridge admits it and executes the
fixture's initialization. Active-module limits are checked afterward, separately
from that comparison. [Qualification records](evidence/m2-managed-reload-capacity.json)
distinguish these managed checks from native hosting and broader reliability.
