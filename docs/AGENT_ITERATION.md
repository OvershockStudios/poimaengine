# Iterate on a game with an agent

Use this guide to connect discovery, observation, guarded changes, compiled
gameplay and verification in an existing project. Codex, Claude Code and custom
clients use the same authoritative engine service. Follow the
[client setup](AGENT_CLIENTS.md) first; this guide does not create a project,
install a toolchain or supply missing engine capabilities.

For a complete runnable example, choose a [task playbook](PLAYBOOKS.md).
[Relay Yard iteration](RELAY_YARD_ITERATION.md) covers real C# edits and retained
game state. Its recorded 20-edit functional check passed; its end-to-end p95 was
**7.598753 seconds**, missing the three-second target. That result is scoped to
the recorded native engine, managed tooling and workload.

## Define a small, verifiable change

Before editing, record the desired behavior, relevant entity/type/field IDs,
permitted changes and acceptance checks. For example: increase a door's use
distance, prove that interaction succeeds at the new distance and fails beyond
it, and preserve inventory and the current character pose.

Record who owns the host and player, whether simulation may be paused or
restarted, and where new artifacts and observations may be written. Set finite
request, traversal, job and whole-task deadlines. When sharing a human's session,
attaching a client does not authorize stopping their game or shutting down their
host.

Start with compact capability discovery, then request only the relevant method,
mutation and component schemas. Discover the actual host scope: an editor,
standalone world and shared headless player expose different operations. The
[authoring-core v1 boundary](AUTHORING_API_COMPATIBILITY.md) stabilizes nine
selected methods; it does not freeze runtime, rendering or development-job APIs.

## Read the state that matters

Keep these identities separate:

| State | What to retain |
| --- | --- |
| Authored world | World identity, authored revision and the original edit draft |
| Live runtime | Session ID, tick, structure/component/gameplay/UI revisions and control sequence |
| Presented game | Player identity, generation, control revision and readiness/presentation metadata |
| Background work | Job identity, accepted request and actual terminal state |

An authored change does not automatically change the runtime's frozen scene.
`source_stale: true` can be legitimate; decide explicitly whether the task needs
an authorized restart. A tick alone does not identify every live change.

Use [runtime observations](RUNTIME_OBSERVATION.md) to read selected poses and
component fields together. Request animation and motion explicitly; the default
entity fields omit them. Gameplay values and logical UI contents require their
separate inspections. A projected observation is not a complete save or a
retained snapshot.

### Read two known selections at one boundary

This Python recipe assumes an existing connected `WorldClient`, an active
runtime, and two nonempty lists of known live entity IDs. Each list must contain
at most 32 unique lowercase hexadecimal IDs. It requires no custom component
types. It neither starts nor pauses simulation.

```python
from poima_client import RpcError

def observe_two_selections(client, first_ids, next_ids):
    for ids in (first_ids, next_ids):
        if not 1 <= len(ids) <= 32 or len(set(ids)) != len(ids):
            raise ValueError("Supply 1–32 unique live entity IDs per selection")

    catalog = client.discover()
    if not {"runtime.status", "runtime.observe"} <= set(catalog["methods"]):
        raise RuntimeError("Runtime observation is unavailable in this host")
    client.discover("method", "runtime.observe")
    status = client.call("runtime.status")
    if not (status["available"] and status["active"]):
        raise RuntimeError("An active runtime is required")

    selection = {
        "entity_fields": ["world_matrix", "layout", "animation"],
        "components": [],
        "include_schemas": False,
    }
    first = client.call("runtime.observe", {
        **selection, "ids": first_ids,
        "session_id": status["session_id"], "tick": status["tick"],
        **{key: status[key] for key in
           ("structure_revision", "ui_revision", "control_sequence")},
    })
    guards = {key: first[key] for key in (
        "structure_revision", "component_revision", "gameplay_revision",
        "ui_revision", "control_sequence",
    )}
    second = client.call("runtime.observe", {
        **selection, "ids": next_ids,
        "session_id": first["session_id"], "tick": first["tick"], **guards,
    })
    source_keys = ("authored_revision", "current_authored_revision", "source_stale")
    if any(first[key] != second[key] for key in source_keys):
        raise RuntimeError("Authored source changed; discard this observation")
    return first, second

# Call with your connected client and IDs. On RpcError -32009 (stale tick/guard)
# or -32030 (absent/replaced session), discard the entire attempt, inspect again
# and decide whether to retry. Do not merge its partial rows with a later read.
```

The first observation supplies all five guards. `runtime.status` does not supply
component or gameplay revisions; do not invent those tokens. A running owner
can advance between calls, so this recipe can correctly reject without changing
anything. Other RPC errors must also be handled; an absent entity rejects the
whole request with `-32004`.

For component-query pagination, retain the same session, tick and guards, pass
the returned exclusive `query.next_after` as `query.after`, and stop at `null`.
Both structure and component revisions are required for continuation. Set a
page limit; do not truncate silently or substitute current guards partway
through. See the [selection and response bounds](RUNTIME_OBSERVATION.md#bounds-and-availability).

## Change deliberately and recover by outcome

Keep the original observation with each edit draft. Submit a guarded transaction
or runtime command using its discovered schema. On conflict, inspect the new
state, reconsider the change and construct a new operation. Automatically
replacing stale guards with current ones defeats stale-edit protection.

A native rejection, an unknown outcome and a malformed success response require
different recovery:

| Result | Next action |
| --- | --- |
| `RpcError` | Handle the native rejection; retain the original draft when reconciling a conflict. |
| `OutcomeUnknown` | Preserve the exact method, immutable payload and receipt ID. The operation may have applied; the transport disconnects. Reconnect and use that operation's documented recovery rules. |
| `ResponseContractError` | The engine already returned success and may have committed it. Inspect state before attempting another mutation. |

`OutcomeUnknown` inherits `TransportError`; catch it first. The JSON-RPC
correlation ID is not a mutation's `request_id`. Do not issue the uncertain
operation under a new receipt just to obtain an answer. Durable authored
receipts, runtime/player receipts and development-job receipts have different
retention and restart rules. A replayed receipt reports a historical result;
inspect current state afterward. Follow [Python failure and recovery](PYTHON_CLIENT.md#failure-and-deliberate-recovery)
and the operation's own reference.

## Compile, inspect, then load

For C# changes, preserve the exact source edit and matching SDK, bridge, manifest
and toolchain identities. Use [development jobs](DEVELOPMENT_JOBS.md) with a
fresh request ID and separate output directory. Compilation acceptance is not
completion, and successful compilation does not load the artifact.

Poll under a finite deadline. Inspect the terminal job state, exit status and
bounded diagnostics; an empty or truncated diagnostic list is not proof of
success. Cancellation acknowledgement also does not establish that the worker
has stopped. Retain failed artifacts and their diagnostics separately from a
corrected attempt. The [build-job recipe](AGENT_BUILD_JOBS.md) shows bounded
polling and separate observation after an authorized cancellation.

If an interactive player is attached, obtain an authorized pause and observe
fresh runtime guards before loading. Running playback rejects the load with
`-32080`; recorded replay forbids intervening runtime mutations even while
paused. Stop only an owned or explicitly authorized replay before changing it.

Explicitly [replace CoreCLR gameplay](MANAGED_GAMEPLAY.md#replace-game-code)
using `runtime.gameplay.load`, a new receipt, the observed tick/gameplay revision
and required structural guard. Candidate initialization runs before compatible
migration. Inspect the actual loaded image and game state afterward. Native AOT
images require separate processes; this is not an AOT hot-reload recipe.
Publication verification establishes artifact bytes, not gameplay correctness.

## Prove behavior and observe presentation

Exercise the changed behavior through native inputs, queries and compiled
callbacks. For the door example, moving the controller and interacting at the
chosen distances proves more than writing an “opened” field. Include the
relevant negative case and verify retained state. Complete checkpoint and
fresh-process continuation claims need their own full-state checks; selected
observation rows cannot establish them.

For a [shared live player](LIVE_PLAYER.md), retain the start receipt and wait for
actual readiness or terminal failure. If stable inspection needs a pause, obtain
authorization from the owner and confirm the current runtime boundary afterward.
Wait for presentation of updated state, then capture using the actual player,
control revision, session and tick into a fresh output path. Retain metadata with
the image. Readiness, a semantic pass and an image are separate evidence.
Desktop viewports use their own capture and lifecycle operations. Native Windows
presentation needs an accessible desktop and Vulkan; API access alone does not
establish visual or physical-input correctness.

Stop presentation and inspect its terminal report when you own that lifetime.
Stop the runtime and shut down the host only when authorized. Closing a connected
client detaches from its [shared owner](SHARED_SESSIONS.md). The Python client's
owned-process cleanup covers its direct child; it is not an arbitrary descendant
process-tree supervisor.

Report the observed behavior, retained-state checks, artifact identities,
presentation and cleanup separately. Mark timing misses and incomplete checks
explicitly. A completed compile, loaded artifact or recovered receipt is one
step in this loop, not proof that the whole task succeeded.
