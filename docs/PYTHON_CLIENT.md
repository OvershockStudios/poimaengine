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

### Open with development profiles or an owned command prefix

Pass a [development profile file](DEVELOPMENT_PROFILES.md) explicitly:

```python
with WorldClient.open(binary, world,
                      development_profiles=Path("profiles.json")) as engine:
    print(engine.discover())
```

The client emits `--development-profiles` once after the world path. `None`
preserves the original command. A profile is a nonempty text path or path-like
object returning text, without NUL or a leading `--`. A filename beginning
with `--` can use an explicit `./--profiles.json` string. Paths remain literal
argv entries; the client does not resolve them, open the profile, translate WSL
paths or change the supplied working directory. Invalid profile arguments reject
before starting a process or creating traces.

When a host supplies an approved wrapper command, use `open_command` with that
exact prefix. This example assumes the host has supplied `command_prefix`,
`world` and `profiles` with native platform paths:

```python
with WorldClient.open_command(command_prefix, world,
                              development_profiles=profiles,
                              trace_directory=Path("run-wire")) as engine:
    print(engine.discover())
```

The prefix must be a nonempty list or tuple of text argv entries, including its
executable and any wrapper flags or delimiter. The helper copies it unchanged,
then appends the world command and optional profile flag. It launches no shell,
infers no wrapper behavior and grants no process-tree ownership or sandbox.
Keep using the host's admitted execution route and lifecycle checks. Trace and
other transport options work as with `open`.

`--profiles` is unsupported. A CLI startup error is not a JSON-RPC response;
requests already written to that channel can still raise `OutcomeUnknown`.
Retain the original bytes and inspect the actual command and owner outcome.
These helpers do not retry, reconnect or reinterpret an uncertain result as a
successful or rejected RPC.

## Retain the original RPC streams

Opt into raw traces when a workflow needs records independent of decoded
results:

```python
with WorldClient.open(binary, world,
                      trace_directory=Path("build/run-01-wire"),
                      trace_limit_bytes=32 * 1024 * 1024) as engine:
    print(engine.inspect())
```

The trace directory must be new, with an existing parent. It receives
`stdin.ndjson`, `stdout.ndjson` and `stderr.log`. The input trace contains bytes
accepted by successful native pipe writes; output and error traces capture raw
chunks before parsing or diagnostic-tail truncation. Invalid or unterminated
output remains intact. These files can contain project data and local paths;
choose their storage and publication deliberately.

`trace_limit_bytes` caps each stream independently; the default is 32 MiB.
An I/O or cap failure disconnects the transport and preserves recorded prefixes.
It does not retry a request. A sent request can have an unknown outcome even
when its complete input was not recorded. Inspect the original error and
reconcile with the authoritative owner before taking another action.

Without `trace_directory`, the client creates no trace files. Trace files close
after their corresponding I/O threads retire; `close()` still reports bounded
cleanup failures. Tracing does not establish descendant-process ownership or
prove that a shared host exited. [Locomotion Yard's headless workflow](../examples/locomotion-yard/README.md)
uses these records alongside native CLI logs and complete checkpoint files.

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
underscores or hyphens.

### Explicitly close a host you own

`engine.shutdown_host(timeout=5)` explicitly sends `host.shutdown` with `{}`.
Use it only when you own the shared headless host or have the owner's permission.
The helper does not establish ownership, detach the client, wait for any process,
or retry. It returns the native reply unchanged; native errors and uncertain
outcomes propagate. There is no `request_id` parameter or durable shutdown
receipt. A standalone world or editor rejects this operation. Stop an owned
player first, then explicitly stop its runtime when appropriate.

A reply with `closed: true` acknowledges the request; it does not prove that the
host exited. For a host process you started, close its client bridge and observe
that exact process separately. This cleanup fragment assumes `host` is your own
`subprocess.Popen`, `engine` is its connected client, and the new host's readiness
was observed before connecting. Use a fresh endpoint for each owned host; never
infer ownership from discovering an existing endpoint. Drain host output or
redirect it to explicit log files while it runs, rather than leaving unread
pipes. Startup failures need their own cleanup of the process you launched.

```python
import subprocess
import sys

completion = {}  # Caller-owned; available after handling the operation's error.
try:
    # Work through engine; retain the IDs of any player/runtime you start.
    print(engine.inspect())
finally:
    primary = sys.exc_info()[1]
    errors = []
    forced = False
    try:
        reply = engine.shutdown_host(timeout=5)
        if reply.get("closed") is not True:
            errors.append(RuntimeError("Host did not acknowledge shutdown"))
    except BaseException as error:
        errors.append(error)
    try:
        engine.close()  # Detach/reap the bridge; this has its own cleanup budget.
    except BaseException as error:
        errors.append(error)
    try:
        host.wait(timeout=5)
    except subprocess.TimeoutExpired:
        forced = True
        errors.append(RuntimeError("Owned host required forced cleanup"))
        try:
            host.terminate()  # Only the exact process this task launched.
            try:
                host.wait(timeout=3)
            except subprocess.TimeoutExpired:
                host.kill()
                host.wait(timeout=3)
        except BaseException as error:
            errors.append(error)
    except BaseException as error:
        errors.append(error)
    if host.returncode != 0:
        errors.append(RuntimeError("Owned host exit: " + str(host.returncode)))
    completion.update(exit_code=host.returncode, forced=forced, errors=errors)
    if primary is None and errors:
        raise errors[0]
```

The caller explicitly inspects or records `completion` after handling the
operation; cleanup diagnostics do not require attributes on its exception.
The shutdown RPC, bridge cleanup and process waits have separate finite budgets;
this fragment is not a hard combined deadline or descendant-process supervisor.
Record forced cleanup as failure even if the host eventually exits with code 0.
An uncertain shutdown is not automatically resent or reconnected. For a host
you only attached to, use `engine.close()` and leave the owner running.

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
`response_contract()` returns a detached copy of the manifest. Read revisions
must be safe JSON integers. Boolean and floating-point values cannot become
valid guards through Python's equality with integers. An impossible successful
reply raises `ResponseContractError`, preserving the original request context
and leaving the transport usable.

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

## Traverse registered runtime components

```python
for page in engine.iter_component_observations(
        session_id, paused_tick, inventory_type,
        entity_fields=["world_matrix"],
        components=[{"type": inventory_type, "fields": [quantity_field]}],
        limit=32):
    for entity in page["entities"]:
        print(entity["id"], entity["components"][inventory_type])
```

Use an existing runtime session, its exact tick, a registered component type ID
and stable field IDs from the native schema. Type IDs here are 32 lowercase hex
digits without the authoring `game:` prefix. The helper uses `runtime.observe`
query membership; it does not register schemas, start or pause play, discover
components, advance the tick or write anything.

The first reply supplies any omitted starting guards. Subsequent calls pin
`structure_revision`, `component_revision`, `gameplay_revision`, `ui_revision`
and `control_sequence`, as well as the session and tick. Replies must preserve
the first page's authored/current source revisions and `source_stale` flag.
Optional `pins` supplies starting values for any of those five runtime guards.
Selected metadata and stable field projections must stay consistent. All rows
retain native IDs, order, values and cursors; no missing row is synthesized.

Select at most four component types and 32 stable fields per type. Omitting
`entity_fields` selects the native default state fields; `[]` selects none.
Omitting a component's `fields` requests all its native fields; `[]` requests an
empty projection. A selected component absent from a row remains `None`.
Membership in the queried component cannot be absent from its selected payload.
`include_schemas=True` includes native component schemas in the metadata.

A native conflict or transport error propagates unchanged. A malformed page,
crossed boundary or exhausted `max_pages` raises `ObservationError`. Pages
already yielded belong to the earlier observation; discard them after failure
if a complete snapshot is required. There are no automatic retries or boundary
repairs. This iterator still checks its own pagination and projection invariants
when `validate_responses=False` is set. Field-value schemas and the complete
field set for an omitted `fields` selector remain native authority.

Page limits are 1–64 and `max_pages` is 1–65,536, with defaults of 32 and 1,024.
`timeout` applies to each RPC, not the whole traversal. Iteration validates
arguments when it starts; constructing the iterator sends no request. These
pages observe runtime entities and selected components; they are not a retained
save, gameplay/UI snapshot or a general response-schema validator.

[Qualification](evidence/m2-python-runtime-observation.json) checks the SDK on
Linux and Windows and the checkout/built wheel against the retained 0.0.99
Windows native service: 65 entities in three pages, parity with individual
native reads, empty membership, bounded traversal and real conflict rejection.
This is a functional protocol check, with no renderer, managed build or
performance benchmark. Run the native regression with matching-platform Python
and a simulation-enabled engine:

```sh
python tests/python_runtime_observation_native.py --engine /path/to/poima
```


## Observe accepted development jobs

`development_status(job_id, include_output=True, timeout=30)` reads one accepted
job and preserves its native result. `wait_for_development(job_id, timeout=30,
poll_interval=0.1, include_output=False)` waits using compact reads by default.
Both validate identity and the lifecycle fields they consume, preserving future
metadata rather than defining another job model. Compact inspection requires
schema revision 79's optional `include_output` parameter; unsupported hosts
reject it without an implicit fallback.

Both helpers also accept `include_process=False`. Explicit `True` sends the
optional selector on every inspection and preserves the native versioned
[process/cleanup observations](DEVELOPMENT_JOBS.md#process-and-cleanup-observations)
without interpreting exit causes. Omission and `False` do not send the selector,
so older hosts keep the existing request path. A host that rejects an explicit
`True` raises its original RPC error; there is no retry or hidden downgrade.

The wait returns failed, cancelled, timed-out and launch-failed terminal outcomes
as well as success. `JobWaitExpired` means the local observation budget expired;
its `job_id` and detached `last_status` preserve recovery context. It does not
cancel native work. `DevelopmentJobError` reports a malformed lifecycle response,
with `job_id` and the received `status`. Neither helper retries, reconnects,
cancels, forgets or loads anything implicitly. Full output and selected structured
diagnostics remain deliberate reads. Job IDs identify work only in the same live
host. See the [build-job recipe](AGENT_BUILD_JOBS.md) for examples, deadline
semantics and deliberate cancellation.

## Failure and deliberate recovery

There are no automatic retries, revision repairs or reconnects.

| Failure | Meaning |
| --- | --- |
| `ValueError` | Local encoding/argument failure; no request sent. |
| `RpcError` | A valid terminal native error, with `code`, `message`, optional `data` and detached request context. The channel remains usable. Follow the operation's failure contract. |
| `TransportError` | Launch/local capacity failure or a disconnected request known not to have been sent. |
| `OutcomeUnknown` | A request began writing but received no trusted terminal result. It may have applied. The entire transport disconnects. |
| `ResponseContractError` | A terminal success arrived with an unexpected core shape or invariant. It may already have committed. The channel remains usable; inspect deliberately. |
| `PaginationError` | Traversal guards/cursors are invalid or the page budget was exhausted. |
| `DevelopmentJobError` | A received job record violates the requested identity or lifecycle fields. |
| `JobWaitExpired` | Local polling budget expired; native work remains uncancelled and terminal completion is unobserved. |

Terminal `RpcError` retains `method`, transport `rpc_id`, immutable
`params_json`, a detached `params` copy and optional operation `request_id`.
The native transport attaches context only after validating response correlation;
`WorldClient` also retains its original request for explicitly embedded transports.
An embedding transport without a correlation ID leaves `rpc_id` as `None`.
The existing constructor, `args`, `str` and `repr` stay unchanged: parameters and
native error `data` are never added to automatic exception formatting. Native
messages are preserved and may themselves contain host-provided details. Treat
explicit parameter inspection as sensitive. Context shares the existing 1 MiB
request bound; mutating `params` does not change the saved JSON string.

```python
from poima_client import RpcError

try:
    engine.call("runtime.ui.inspect", selectors)
except RpcError as failure:
    print(f"{failure.method}: {failure.message} (RPC {failure.rpc_id})")
    # Deliberately inspect the relevant method schema if needed:
    schema = engine.discover("method", failure.method)
    # Decide how to repair the request; the SDK does not retry or rename fields.
```

A terminal error follows its operation's documented failure/partial-progress
contract; it does not imply that nothing changed. Request context adds no schema
lookup, transmission, automatic repair or retry. `rpc_id` is local correlation;
it is never a durable mutation receipt or evidence that a retry is safe.

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
primary exception when close also raises `TransportError`.

Both `WorldClient.cleanup_error` and `JsonRpcProcess.cleanup_error` are read-only
properties retaining the most recent `TransportError` observed by that object's
`close()`, including one suppressed by context exit to preserve a body exception.
They start as `None`. A later successful close leaves the diagnostic available;
a later failed close replaces it. Direct close and successful-body context exit
still raise cleanup errors. A body exception propagates unchanged when cleanup
also raises `TransportError`; the client does not attach attributes to it.

```python
engine = WorldClient.open(binary, world_path)
try:
    with engine:
        engine.inspect()
except RpcError:
    cleanup_failure = engine.cleanup_error  # Inspect separately from the primary error.
    raise
```

With an embedded transport, `WorldClient` records the exact `TransportError`
raised by its `close()`. With `JsonRpcProcess`, the wrapper and transport retain
the same exception. Reading these properties performs no I/O, logging, retry or
cleanup. Error objects/messages are not redacted or added to client/transport
representations automatically; choose explicitly what to record. The property
is an observed close failure, not a live cleanup-health or process-ownership
signal, and does not establish that a request succeeded or is safe to retry.
A non-`None` value does not mean the process is still live.
Unexpected errors and interrupts from close continue to propagate normally.

Use `closed`, `process_id`, `returncode`, `stderr_tail` and `stderr_truncated`
for bounded diagnostics. Tests exercise real Windows/Linux native authoring,
shared sessions, durable/legacy retries and conflicts, plus synthetic reordered
responses, corrupt frames, blocked writes and interrupt races. These are client
and core-authoring checks, not GUI, GPU, agent-provider, game-scale performance
or full API stability qualification. See the [checkpoint evidence](evidence/m2-python-client.json).

The [0.0.133 diagnostics record](evidence/m2-python-rpc-diagnostics.json) passes
32 client tests on both Windows and Linux, including real process transport,
native terminal-error context and same-channel deliberate recovery. The wheel
contains the exact qualified modules/resources and passes an isolated native
error smoke check. These SDK checks use the qualified native 0.0.132 engine.

## Prospective runtime/save result checks

The optional `poima_client.workflow_responses` module checks successful results
for 21 selected runtime/component/UI/save methods against their initiating
requests and optional trusted schema context. It returns the original result or
raises `ResponseContractError`; normal `WorldClient.call` behavior remains
unchanged. This is a candidate surface, not an extension of the released stable
authoring-core contract. See [usage, distinctions and qualification](WORKFLOW_RESPONSE_CONTRACT.md).

## Stable runtime-session result checks

`poima_client.game_session_responses` supplies
`game_session_response_contract()` and `validate_game_session_result(...)` for
the separate [19-method runtime game-session v1](RUNTIME_GAME_SESSION_V1.md).
Both are also exported from `poima_client`. Its frozen packaged schema is
independent of the prospective workflow validator. Original request guards and
trusted dynamic schema context enable stronger checks; validation never fetches
schemas, retries operations or modifies normal `WorldClient.call` behavior.
Check the actual host's qualified contract identity before relying on that
release boundary.
