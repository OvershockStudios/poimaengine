# Save and load from gameplay

C# gameplay can request native checkpoints through `GameContext`. CoreCLR development and compiled Native AOT games use the same owner, storage and restoration code as the editor Save/Load window. Requests are serviced after a successful simulation or control batch; they never perform file I/O inside gameplay callbacks.

This extension requires the **176-byte save-services prefix of services epoch 7**. Later additive prefixes retain that negotiated view; games declaring other extensions also need their required prefix/features. Use a matching gameplay SDK/bridge or published native artifact. Saves still require their exact original module image and schema: rebuilding gameplay is not a save migration. Keep matching runtime/module builds for existing checkpoints.

## Configure storage

In the editor, use **Window > Saves**, or call `save.configure` through the world service. A standalone bundle accepts an existing external directory:

```sh
poima game run path/to/game.json --save-root path/to/saves
# Opt into background gameplay writes with an existing external save directory:
poima game run path/to/game.json --save-root path/to/saves --save-write-mode background
```

Relative launch paths resolve from the launch directory. Storage must be outside the immutable bundle and asset store. Omit the option to disable gameplay saving. Games receive slot names and operation tickets; they cannot select filesystem or executable paths through this API. Native `Runtime` instances outside a configured owner also start with saving disabled.

`save.configure` accepts `write_mode: "synchronous" | "background"`; omission selects
`synchronous`. `save.status` reports the selected mode alongside the root and
configuration generation. For a fresh owner with an existing `saves` directory:

```json
{"jsonrpc":"2.0","id":1,"method":"save.configure","params":{"request_id":"00000000000000000000000000000001","expected_generation":0,"root":"saves","write_mode":"background"}}
```

Relative RPC roots resolve from the world file's directory. Refresh
`expected_generation` from `save.status` when reconfiguring an existing owner.
Background mode requires at least one [native job worker](JOBS.md);
`POIMA_JOB_WORKERS=0` rejects that configuration. The standalone `--save-root`
option keeps the synchronous default; `game run` and `game serve` accept
`--save-write-mode background` or explicit `synchronous` when `--save-root` is
provided. Unknown, repeated or missing mode values reject before bundle opening.
Background mode changes gameplay save
requests only; gameplay loads and external `save.write` remain synchronous.
The managed API and negotiated services prefix are unchanged.

## Typed API

| API | Behavior |
| --- | --- |
| `context.Saves` | Enabled flag, current runtime epoch, configuration generation and optional last-restore metadata. Memory only. |
| `TryRequestSave(slot, expectedGeneration)` | Queue one save; omitted generation selects the observed generation at host service time. |
| `TryRequestLoad(slot, expectedGeneration, allowRecovery)` | Queue a staged load using the host's trusted gameplay module. Recovery is opt-in. |
| `RequestSave` / `RequestLoad` | Same requests, throwing `SaveRequestException` for a typed rejection. |
| `GetSaveResult(ticket)` | Query queued, resolving, succeeded, failed or expired state without touching files or consuming the result. |

Slots contain 1–64 lowercase ASCII letters, digits, underscores or hyphens. Generation guards are safe nonnegative integers. Rejections distinguish disabled capability, a busy queue, invalid arguments and exhausted ticket space. The queue accepts one outstanding request per runtime; it does not silently grow.

A `SaveTicket` contains three `long` values: `EpochHigh`, `EpochLow` and `Sequence`. Epoch values are opaque signed bit patterns. Current gameplay schemas support primitive fields, so persist these three values separately in `TState`, then reconstruct the transient ticket when querying:

```csharp
var request = context.TryRequestSave("quick");
if (request.Accepted)
{
    state.SaveEpochHigh = request.Ticket.EpochHigh;
    state.SaveEpochLow = request.Ticket.EpochLow;
    state.SaveSequence = request.Ticket.Sequence;
}

// In a later Tick:
var ticket = new SaveTicket(state.SaveEpochHigh, state.SaveEpochLow, state.SaveSequence);
var result = context.GetSaveResult(ticket);
if (result.IsTerminal)
    state.SaveSequence = 0;
```

Request on an action or state transition, not unconditionally every tick. The [executable fixture](../tests/gameplay-save/SaveRequests.cs) demonstrates clearing a trigger before requesting and reconciling restored pending tokens. A persisted ticket is data, never an instruction to enqueue again.

## Commit and rollback

A callback at tick 100 may request a save during a batch advancing to tick 105. The checkpoint captures tick 105. Results report both `RequestedTick` and `CommittedTick`; this is a batch boundary, not an immediate save at the callback tick. Splitting that batch into separate calls can intentionally change the captured checkpoint.

If any later tick in that batch fails, gameplay state, ticket allocation and the request roll back together with simulation. The save owner is never called, so that request creates no slot or files. Storage failure after a successful batch does not undo those committed simulation ticks. Its outcome is a save-operation result.

Automatic editor playback and the standalone player use one-tick commits. A save requested at tick zero therefore snapshots committed tick one, even when the editor poll has more catch-up ticks to run. A later automatic tick failure cannot undo that earlier committed save. Explicit multi-tick `runtime.step` still defers servicing until its entire batch succeeds. A successful load ends the editor’s remaining catch-up work and opens the restored session paused with old input released.

Writes use retained operation identities and durable receipts. If storage reports an error after publication, the same receipt is checked before deciding success or failure. When storage cannot be inspected, or the relevant receipt has fallen outside retained history, the request stays `Resolving`; gameplay must not interpret this as permission to issue a fresh write. Synchronous unresolved work blocks another simulation batch. An owner-dispatched background save permits ordinary `Tick` and non-saving `Control` callbacks while keeping the captured request and its original committed tick unchanged. Another save or load remains `Busy`. Pending work also prevents reconfiguring storage, external writes/replacement, stopping, code reload, external structural changes and external field/UI edits.

In background mode the owner captures one immutable checkpoint after the complete
successful batch. A background worker inspects the slot, selects the generation,
constructs the retained identity, writes the checkpoint and resolves uncertain
receipts. Resolution retries keep that identity and never write or recapture.
The owner polls completed work in memory and publishes its result at request
entry or player polling, including paused playback and owners without an
active player. Save completion timing is an asynchronous external boundary; CPU
worker scheduling is not deterministic simulation state.

The owner retains 64 terminal operation results. Reads are repeatable; unknown or evicted tickets expire. The ordinary slot store retains 32 durable write receipts independently. These are bounded histories, not permanent audit logs.

## Loading and continued play

Loading stages a separate runtime, verifies frozen content and the exact trusted module, and restores supported typed state without gameplay `Initialize`. Failure keeps the committed source world active. Success assigns a fresh runtime session and epoch. Saved command queues are not replayed.

The replacement's first callback can read `Saves.LastRestore`, including the initiating ticket for gameplay loads. A saved pending token may resolve through the live owner's ledger or expire after process restart; both are terminal outcomes. Code should reconcile that token instead of waiting forever or repeating its load trigger.

The desktop editor pauses after a load and clears old input. The standalone player keeps its window and graphics device, replaces its audio timeline, clears held input and one-shot edges, resets timing, then waits for explicit click/Start resume. It revalidates the selected camera and controller; missing selections produce a player error. Interactive playback does not exit after its 32nd restore. The reported replacement count is diagnostic and saturates at `UINT32_MAX`; it does not limit the window's lifetime. Other host resource budgets still apply, including the current 10,000 runtime-session identity budget per world owner.

Recorded player and audio replay stop at a successful replacement with `runtime_replaced`; they do not continue the old scripted timeline through a rewound world. A caller can issue a new replay against the current session.

## Agent observation and retries

`runtime.save.status {session_id}` reports capability, epoch, pending work and last restore. `runtime.save.result {epoch, sequence}` queries the owner ledger even after a runtime stops. Both are memory-only. Discover current schemas through `world.describe`.

A `runtime.step` result preserves the source `session_id`, `previous_tick`, committed `tick` and `stepped` count. `current_session_id`, `current_tick` and `runtime_replaced` separately identify the world now active. `save_serviced` identifies owner capture or synchronous storage work; polling an existing background write does not mark every later tick as storage work. `save_operation`, when present, describes the operation at that boundary and may still be queued or resolving. Sound-event handles belong to the source batch.

The owner retains 32 advance receipts across load, stop and restart. An exact retry returns the original outcome before checking the currently active session; it cannot step the replacement or repeat a load. Changing a retained request's parameters rejects. A historical advance result is not a substitute for querying a currently resolving operation. Player/audio replay receipts also survive replacement.

`runtime.play` reports `pending_gameplay_save` at its final report boundary:
the original queued/resolving operation, or `null`. This is an observation, not
a storage fence; `null` does not mean a save succeeded. Query
`runtime.save.result` using the original operation ticket for its outcome.
Exact playback retries retain the original observation rather than waiting or
replaying ticks.

## Limits

Snapshot capture and serialization remain on the owner and may hitch even with
background writes. The writer retains at most one immutable checkpoint of
64 MiB; it does not coalesce autosaves or accept an unbounded queue. Synchronous
storage still runs on the owner. The host excludes serviced owner storage/capture
time from subsequent simulation catch-up; this is not a save-latency guarantee.
Shutdown waits for its own accepted write without shutting down another
consumer's shared pool. Generic job cancellation after write entry cannot erase
an actual durable receipt; cancellation before the callback starts writes nothing.

Player polling now pumps completed writes in the shared and blocking player
paths. The idle hook does not step simulation, invoke gameplay, initialize
devices or wait for storage. Finishing playback may leave a save queued or
resolving at its report boundary; owner destruction still waits for its own
accepted writer group. Standalone launches retain synchronous writing unless
explicitly configured. Rendered paused-player completion and save hitch budgets
require separate qualification.

Supported spawned root props and [complete hierarchical instances](RUNTIME_INSTANCES.md#save-and-reload-boundaries), including their typed bounded component arrays, persist in checkpoints. This is registered native/typed state, not arbitrary C# object serialization.

The external service supports [explicit source-to-target upgrades](SAVE_UPGRADES.md), including hierarchical snapshots and approved bounded-array capacity changes. The host chooses the trusted target and plan; **C# load requests remain exact**. Automatic user-storage selection, autosave policy and gameplay-selected upgrades remain unfinished. The existing 64 MiB snapshot limit, exact asset binding, reconstructed physics/audio state and storage durability limits still apply. Gameplay can opt into loading a verified prior payload; acknowledging a recovery write remains an explicit host/editor operation.

## Background write implementation

The opt-in path is implemented by [GameplaySaveWriter](../include/poima/gameplay_save_writer.hpp).
Eight Windows native checks pass, including a compiled CoreCLR game that saves
tick one while a held background worker lets tick two continue. They cover
failed-tick rollback, pending-operation guards, exact retries, idle owner
completion, captured worker traces and restoration in a fresh World owner
within the same process. Separate writer checks exercise real storage failures,
uncertain receipts and cancellation before and after durable publication.
The standalone writer also passes the same correctness test on Linux;
integrated Linux World qualification is separate work.

A retained Native AOT UI module separately passes compiled Control/Tick
continuation against a manually dispatched queue; it does not qualify a
worker-backed Native AOT World save. The Windows storage test skips its symlink
fixture because the host lacks that privilege. These are CPU correctness
checks, without a native player window, save-latency measurement or power-loss
qualification. [Background-save evidence](evidence/m2-background-gameplay-saves.json).
The historical evidence below describes the synchronous checkpoint.

## Qualification

The [0.0.103 evidence](evidence/m2-standalone-save-mode.json) records four
Windows native checks, three Linux disabled-backend groups, and a freshly
exported primitive bundle exercising 27 CLI commands and 20 service calls.
These verify owner-poll boundaries without device initialization, compiled
CoreCLR save continuation and same-process restoration, and headless launch
configuration. They do not qualify a rendered `game run`, durable completion
in a paused native window, fresh-process restoration or save latency.

[Recorded evidence](evidence/m2-gameplay-save-requests.json) binds source and binary hashes to real CoreCLR/Native AOT requests on Linux and Windows, a native-only Linux engine, final packaged editor checks and a relocated compiled game. It also records player replacement on both laptop GPUs, 100 compatible C# reloads, and 42 runtime/34 authoring-only Linux suites. The Windows session was locked; synthetic input and semantic GUI checks do not establish physical-input or OS-screenshot qualification.

That record describes its original save-request checkpoint. Later [hierarchical instance evidence](evidence/m2-runtime-instances.json) covers exact version-6 instance restoration. Use the [upgrade task](SAVE_UPGRADES.md#task-upgrade-a-retained-hierarchical-game) and current [implementation status](IMPLEMENTATION_STATUS.md) for the explicit schema-evolution contract and its separately qualified backends.
