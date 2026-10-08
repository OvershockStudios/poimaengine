# Python automation client

`poima-client` is an external authoring/test client for the native engine, with
no runtime dependencies outside Python's standard library. Python is not a
gameplay interpreter; engine and shipped gameplay remain compiled code.
The client and [authoring-core contract](AUTHORING_API_COMPATIBILITY.md) are in
development. Pin an engine revision and retain project-specific behavior checks.

## Install and open a world

From a checkout, install into your Python environment:

```sh
python -m pip install ./tools/python
```

The package includes neither the engine executable nor an agent provider. Build
Poima first. Use an executable and paths native to the operating system running
Python. Windows Python uses Windows Poima and `D:/...` paths; Linux Python uses
Linux Poima and Linux paths. The client does not translate WSL paths. The world
file's parent directory must already exist.

```python
from pathlib import Path
from poima_client import WorldClient, new_id

binary = Path("build/headless/poima").resolve()
world = Path("build/python-example.world.json").resolve()
world.parent.mkdir(parents=True, exist_ok=True)

with WorldClient.open(binary, world) as engine:
    catalog = engine.discover()  # compact names; full descriptor is explicit
    print(engine.discover_mutation("component.set", "Transform"))
    print(engine.discover("method", "world.transact"))  # complete operation union
    print(engine.discover("section", "invariants"))
    revision = engine.inspect()["revision"]
    entity_id, receipt_id = new_id(), new_id()
    result = engine.transact(
        [{"op": "entity.create", "id": entity_id, "name": "Harbour"}],
        revision, request_id=receipt_id)
    print(engine.get(entity_id, component="Transform",
                     revision=result["revision"]))
```

`open` owns one standalone world writer. It does not launch a desktop editor.
Startup errors can surface on the first call; there is no implicit handshake or
discovery request. Context-manager exit closes and reaps the owned CLI process.

## Share an editor or headless host

Use the endpoint of an existing [shared session](SHARED_SESSIONS.md):

```python
with WorldClient.connect(binary, "my-project", timeout_ms=10000) as engine:
    print(engine.inspect())
```

`connect` launches the native endpoint bridge. `timeout_ms` bounds connection
establishment and each native bridge exchange, independently of Python call
deadlines. Closing this client detaches it; it does not call
`session.close`, `host.shutdown` or terminate the owning editor/headless host.
Other clients keep working. Endpoint names contain 1–64 ASCII letters, digits,
underscores or hyphens. Stop a headless host only with an explicit
`engine.call("host.shutdown")` from an authorized client.

## Discover and call the entire native API

Conveniences cover inspection, history, entity reads/queries, transforms,
transactions and undo/redo. They return native JSON results and maintain no
second world model. Any discovered operation remains available through:

```python
descriptor = engine.discover("method", "runtime.status")
runtime = engine.call("runtime.status", {})
```

`discover_mutation(operation, type=None)` is available in the 0.0.48 client
and requires a native host advertising mutation discovery (schema 48).
It returns the partial transaction schema with revision and receipt requirements.
Component-type selections apply only to `component.set`/`component.remove`;
custom types must be registered in that world. Selectors contain 1–128 UTF-8
bytes. Response checks verify the echoed selection, writable-session metadata
and transaction envelope shape; native equivalence checks establish preservation
of the full schema's guards and bounds. Existing `discover(view, name)` views
remain available, and the generic `call` can send any discovered native request.

Read its descriptor and applicable subsystem invariants before calling it.
Discovery is not cached. Methods outside the nine-method core do not receive
invented wrapper schemas. Core successes are checked against the packaged
`core_responses.v1.json` manifest (the historical candidate resource is also retained) and contextual invariants: requested
IDs/read revisions, original-base-plus-one mutation revisions, focused
discovery, history bounds, sorted IDs and query filters/cursors. Additional
result properties are allowed. Component-specific result validation is limited
to `Transform` shape, finite numbers and positive scale. Generic component
results require an object; full component semantics remain engine-owned.
`response_contract()` returns a detached copy of the manifest.

`validate_responses=False` explicitly disables these result checks. It does not
disable transport validation, engine guards or bounded pagination checks.

## Traverse one revision

```python
for page in engine.iter_query_pages(component="Transform", limit=256):
    print(page["revision"], page["entities"])

roots = list(engine.iter_entities(parent=None, limit=64, max_pages=1024))
```

An omitted parent selects all entities; `None` selects roots; a parent ID selects
its children. The first page pins its observed revision unless supplied
explicitly. All continuation calls include that revision and an exclusive
cursor. A concurrent edit propagates the native stale-revision error; previously
yielded pages belong to the old snapshot. Discard/restart the traversal if a
complete current snapshot is required. Exceeding `max_pages` raises
`PaginationError`; it never silently reports truncation as completion.
Limits are 1–256 rows and 1–65,536 pages, with 1,024 pages by default. Timeouts
apply per page, not to the entire traversal. `query` fetches just one page.

## Failure and deliberate recovery

There are no automatic retries, revision repairs or reconnects.

| Failure | Meaning |
| --- | --- |
| `ValueError` | Local encoding/argument failure; no request sent. |
| `RpcError` | A valid terminal native error, with `code`, `message` and optional `data`. The channel remains usable. Follow the operation's failure contract. |
| `TransportError` | Launch/local capacity failure or a disconnected request known not to have been sent. |
| `OutcomeUnknown` | A request began writing but received no trusted terminal result. It may have applied. The entire transport disconnects. |
| `ResponseContractError` | A terminal success arrived with an unexpected core shape or invariant. It may already have committed. The channel remains usable; inspect deliberately. |
| `PaginationError` | Traversal guards/cursors are invalid or the page budget was exhausted. |

`OutcomeUnknown` inherits `TransportError`; catch it first. Mutation helpers
generate a receipt ID once when omitted. Failed-call recovery context retains
`method`, immutable `params_json`, a convenient detached `params` copy and
`request_id` for native receipt-bearing operations. The transport's `rpc_id`
is only response correlation; it is not that receipt ID. Interrupts preserve
`KeyboardInterrupt`/`SystemExit` and attach the helper's original parameters.
Never rebuild a failed request from newer state or generate another receipt ID
while assuming it is the same operation.

```python
import json
from poima_client import OutcomeUnknown

try:
    engine.transact(ops, base_revision, request_id=receipt_id)
except OutcomeUnknown as failure:
    engine.close()  # wait for owned process cleanup
    original_method = failure.method
    original_params = json.loads(failure.params_json)
    # Deliberate exact retry against the SAME world or still-running host:
    with WorldClient.open(binary, world) as recovered:
        receipt = recovered.call(original_method, original_params)
        current = recovered.inspect()
```

Use `connect` when recovering a shared session; `open` would compete with its
writer. Durable authored receipts retain only the latest 128 commits. Retained
retries return their original revision/result with `replayed: true`; that
revision may differ from current `inspect`. Legacy retained receipts can lack
`history_recorded`; the client preserves that absence instead of inventing
history evidence. An evicted retry is subject to the original revision guard.
Other subsystems have different retention and partial-progress contracts; this
example is not a blanket retry recipe for every native operation.

## Transport limits and qualification

`JsonRpcProcess` takes explicit argument strings, never a shell command. It
supports calls, not notifications or JSON-RPC batch arrays. It correlates exact
integer IDs and rejects duplicate keys, nonfinite numbers, invalid UTF-8,
malformed/error envelopes and unknown/duplicate replies.

Defaults are 32 pending calls (configurable 1–128), a 1 MiB request frame, a
16 MiB response frame, a bounded 64 KiB stderr tail and a 10-second cleanup
deadline. Capacity failure does not enqueue a request. Wrapper preparation
serializes a detached, bounded parameter snapshot before the transport deadline
starts; that preparation is outside the per-call timeout. The transport deadline
covers its encoding, queuing, writing and reply wait. Bound simultaneous wrapper
calls in your embedding application. Timeouts/protocol failures disconnect the
whole channel: writing/written calls have unknown outcomes, queued calls remain
known unsent. Closing cancels pending calls and reaps the direct child; generic
descendant-tree cleanup is not promised. Context exit preserves an existing
primary exception if cleanup also fails.

Use `closed`, `process_id`, `returncode`, `stderr_tail` and `stderr_truncated`
for bounded diagnostics. Tests exercise real Windows/Linux native authoring,
shared sessions, durable/legacy retries and conflicts, plus synthetic reordered
responses, corrupt frames, blocked writes and interrupt races. These are client
and core-authoring checks, not GUI, GPU, agent-provider, game-scale performance
or full API stability qualification. See the [checkpoint evidence](evidence/m2-python-client.json).
