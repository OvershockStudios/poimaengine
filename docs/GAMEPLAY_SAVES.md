# Save and load from gameplay

C# gameplay can request native checkpoints through `GameContext`. CoreCLR development and compiled Native AOT games use the same owner, storage and restoration code as the editor Save/Load window. Requests execute after a successful simulation batch; they never perform file I/O inside `Tick`.

This extension requires **services ABI 7 (176 bytes)**. Rebuild the gameplay SDK, managed bridge and game assemblies together; republish native game libraries. Older service tables and native descriptors reject. Saves still require their exact original module image and schema: rebuilding gameplay is not a save migration. Keep matching runtime/module builds for existing checkpoints.

## Configure storage

In the editor, use **Window > Saves**, or call `save.configure` through the world service. A standalone bundle accepts an existing external directory:

```sh
poima game run path/to/game.json --save-root path/to/saves
```

Relative launch paths resolve from the launch directory. Storage must be outside the immutable bundle and asset store. Omit the option to disable gameplay saving. Games receive slot names and operation tickets; they cannot select filesystem or executable paths through this API. Native `Runtime` instances outside a configured owner also start with saving disabled.

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

Writes use retained operation identities and durable receipts. If storage reports an error after publication, the owner checks the same receipt before deciding success or failure. When storage cannot be inspected, or the relevant receipt has fallen outside retained history, the request stays `Resolving`. Another simulation batch is blocked until that outcome can be resolved; gameplay must not interpret this as permission to issue a fresh write. Pending work also prevents reconfiguring storage, external replacement, stopping, code reload or field edits.

The owner retains 64 terminal operation results. Reads are repeatable; unknown or evicted tickets expire. The ordinary slot store retains 32 durable write receipts independently. These are bounded histories, not permanent audit logs.

## Loading and continued play

Loading stages a separate runtime, verifies frozen content and the exact trusted module, and restores supported typed state without gameplay `Initialize`. Failure keeps the committed source world active. Success assigns a fresh runtime session and epoch. Saved command queues are not replayed.

The replacement's first callback can read `Saves.LastRestore`, including the initiating ticket for gameplay loads. A saved pending token may resolve through the live owner's ledger or expire after process restart; both are terminal outcomes. Code should reconcile that token instead of waiting forever or repeating its load trigger.

The desktop editor pauses after a load and clears old input. The standalone player keeps its window and graphics device, replaces its audio timeline, clears held input and one-shot edges, resets timing, then waits for explicit click/Start resume. It revalidates the selected camera and controller; missing selections produce a player error. Interactive playback bounds replacements to 32 per invocation.

Recorded player and audio replay stop at a successful replacement with `runtime_replaced`; they do not continue the old scripted timeline through a rewound world. A caller can issue a new replay against the current session.

## Agent observation and retries

`runtime.save.status {session_id}` reports capability, epoch, pending work and last restore. `runtime.save.result {epoch, sequence}` queries the owner ledger even after a runtime stops. Both are memory-only. Discover exact schemas through `world.describe`, revision 28.

A `runtime.step` result preserves the source `session_id`, `previous_tick`, committed `tick` and `stepped` count. `current_session_id`, `current_tick` and `runtime_replaced` separately identify the world now active. `save_serviced` identifies synchronous owner work; `save_operation`, when present, describes its outcome. Sound-event handles belong to the source batch.

The owner retains 32 advance receipts across load, stop and restart. An exact retry returns the original outcome before checking the currently active session; it cannot step the replacement or repeat a load. Changing a retained request's parameters rejects. A historical advance result is not a substitute for querying a currently resolving operation. Player/audio replay receipts also survive replacement.

## Limits

Storage and snapshot work are synchronous and may hitch. The host excludes serviced storage time from subsequent simulation catch-up; this does not make file I/O asynchronous. Automatic user-storage selection, background workers, autosave policy, schema migrations, spawned/despawned entities and general object serialization remain unfinished. The existing 64 MiB snapshot limit, exact external-asset binding, reconstructed physics/audio state and storage durability limits still apply. Gameplay can opt into loading a verified prior payload; acknowledging a recovery write remains an explicit host/editor operation.

## Qualification

[Recorded evidence](evidence/m2-gameplay-save-requests.json) binds source and binary hashes to real CoreCLR/Native AOT requests on Linux and Windows, a native-only Linux engine, final packaged editor checks and a relocated compiled game. It also records player replacement on both laptop GPUs, 100 compatible C# reloads, and 42 runtime/34 authoring-only Linux suites. The Windows session was locked; synthetic input and semantic GUI checks do not establish physical-input or OS-screenshot qualification.
