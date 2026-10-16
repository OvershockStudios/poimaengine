# C# callback allocation and GC observations

CoreCLR gameplay can opt into cached diagnostics for `Initialize`, `Tick` and
`Control`. Each loaded module owns three fixed counter buckets. The ordinary
load path leaves measurement disabled; NativeAOT has no matching observer.
These observations complement the [native profiler](PROFILER.md) and
[process-memory timeline](MEMORY_TIMELINE.md).

## Enable and read

Discover the current schemas with `world.describe`. Add `"profile_callbacks":
true` to an otherwise normal guarded `runtime.gameplay.load` request. Keep the
matching managed bridge output together with its SDK and dependency metadata.
The bridge must explicitly negotiate version 1; the native host rejects an opt-in load using an older bridge before calling the candidate game's `Initialize`. The existing
module and authoritative game state remain selected on failure. Loading code
can have arbitrary type-constructor or external effects outside that guarantee.

The process pins its CoreCLR host and bridge. If an older bridge is already
loaded, select the matching bridge when starting a new engine process; changing
the bridge path in that same process is not a recovery route. Game-assembly
reload remains available within the selected bridge.

```json
{"jsonrpc":"2.0","id":1,"method":"runtime.gameplay.metrics","params":{"session_id":"0123456789abcdef0123456789abcdef"}}
```

An optional integer `tick` rejects a stale tick with the normal runtime conflict.
A successful read returns `session_id`, `tick`, gameplay `revision`,
`structure_revision` and `module`. With no gameplay loaded, `module` is null.
The read is allowed during advancing background play, which can progress
independently between observations. The read itself never advances a simulation tick, performs a process-memory query or forces collection.

A module reports its format (`poima.gameplay-callback-metrics.v1`), version,
backend, game identity, assembly SHA-256, `supported`, `enabled`,
`unavailable_reason` and `data`:

| Module configuration | `supported` | `enabled` | `unavailable_reason` | `data` |
| --- | --- | --- | --- | --- |
| Negotiated CoreCLR observer | `true` | `true` | null | Cached counters |
| Default/explicit-false CoreCLR load | null | `false` | `not_requested` | null |
| NativeAOT | `false` | `false` | `native_aot_not_supported` | null |

Default-off does not probe bridge support and does not invent zero-valued
measurements. Omission and explicit false both disable measurement; their
caller-supplied request shapes remain distinct for retry identity. Do not reuse
one request ID with a changed flag. This method and the opt-in flag are outside
the frozen authoring-core and runtime-game-session v1 contracts. The gameplay
C ABI and save format are unchanged; the added C++ configuration field does not
establish binary compatibility for separately compiled C++ consumers.

## Interpret the counters

Enabled `data` has format `poima.managed-callback-metrics.v1`, version 1,
`module_handle`, `owner_thread_id`, `saturated`, `last_issue` and `initialize`,
`tick`, `control` buckets. Every bucket contains:

| Counter | Meaning |
| --- | --- |
| `calls`, `returned`, `threw` | Actual managed callback attempts and return outcomes. Argument/service validation failures before the callback are excluded. |
| `samples`, `invalid_samples` | Valid measurement brackets and rejected brackets. |
| `allocated_bytes`, `max_allocated_bytes` | Cumulative and largest valid current-thread allocation deltas. |
| `gen0_collections`, `gen1_collections`, `gen2_collections` | CLR collection-count deltas observed during valid brackets. |

Counter values and the module handle are canonical unsigned decimal strings,
preserving the full uint64 range for JSON clients. The owner thread ID is an
integer. Counters saturate instead of wrapping; inspect `saturated` and
`last_issue` before deriving rates. A thread mismatch, negative counter or
counter regression invalidates the bracket without changing the game's callback
result. Unsaturated counts satisfy `returned + threw = calls` and
`samples + invalid_samples = calls`.

Allocations are those counted by `GC.GetAllocatedBytesForCurrentThread` between
the actual callback's entry and exit, including synchronous work it calls.
They exclude other threads, bridge query serialization and work outside those
brackets. GC counts are runtime-wide observations during those intervals;
another CLR thread may trigger them. They do not establish which callback
caused collection. These are neither live heap sizes nor process/VRAM totals.
A read may allocate and normal CLR collection may occur; it never invokes
`GC.Collect` or waits for finalizers. The separate `runtime.gameplay.collect`
operation does force collection and is unsuitable for passive sampling.

## Lifetime, rollback and cost

Counters are diagnostic attempts outside authoritative state. A callback may
return successfully and still fail native validation or command commitment;
`returned` does not mean committed. Throwing callbacks and rolled-back batches
remain counted. Full save comparisons exclude nothing: diagnostics are not
serialized into gameplay state or checkpoint payloads in the first place.

Successful reload or restoration creates a fresh module and counter lifetime.
A failed candidate does not reset the retained module's counters. The opt-in
flag is control configuration, not a saved setting. An in-memory trusted load
configuration may reenable fresh counters on restoration; an external cold
trusted configuration defaults off unless its supported load workflow explicitly
opts in. Keep the session/host identity with the process-local module handle. Do not
join counters across hosts, module handles or assembly changes.

Measurement uses fixed numeric buckets and value-type brackets; no per-callback
JSON records or diagnostic delegates are created. Cached reads serialize a new
bounded response outside measured callbacks. Profiling still adds work and may
affect timing. Measure overhead with matched workloads before treating it as
negligible. Native profiler captures and desktop profiler UI remain separate;
this API does not add a managed allocation timeline or call-stack viewer.

## Check the implementation

With an installed .NET 10 SDK, the standalone compiled helper checks can run
from the checkout:

```sh
dotnet run --project tests/managed_callback_metrics/ManagedCallbackMetrics.csproj --configuration Release
```

Those checks cover counter boundaries, invalid brackets, saturation, actual
held allocations and thrown attempts. They also check that the warmed helper
path allocates no owner-thread heap bytes. This does not measure game-workload
recording overhead or qualify native hosting.

The native hosted test is
[gameplay_callback_metrics_contract.py](../tests/gameplay_callback_metrics_contract.py).
It requires an external Windows Job supervisor, compiled game/new-bridge
closures and the explicitly selected retained old bridge and NativeAOT world
with their original inventories. It is a retained-cohort test, not a default
clean-checkout CI command. Keep those input artifacts, original requests,
responses and complete saves when reproducing its results. Unsupported or
missing legacy inputs must fail admission rather than silently skip cases.

[Recorded results and limits](evidence/m2-managed-callback-metrics.json) separate
helper checks, actual native hosting, Linux disabled-runtime regression and the
separate full-workload qualification.

The [full Performance Yard CoreCLR check](PERFORMANCE_YARD_CORECLR.md) adds
actual 50-field/four-component reload, warm restoration with thirty zero-valued
fresh counters, default-off cold restoration and complete saved-state comparisons.
Its short background run qualifies integration; observer cost, retained heap and
sustained memory growth remain separate questions.
