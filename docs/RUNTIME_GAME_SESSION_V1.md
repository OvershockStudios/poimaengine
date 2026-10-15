# Runtime game-session v1

<!-- SPDX-License-Identifier: Apache-2.0 -->

This contract gives session clients a stable boundary for inspecting and
advancing a running game, activating compiled UI controls and using external
checkpoints. Combine it with [authoring-core v1](AUTHORING_API_COMPATIBILITY.md)
for authored edits. It does not freeze the entire engine or make the engine
Alpha-ready.

## Discover the supported boundary

The [release manifest](contracts/runtime-game-session-v1.release.json) pins the
[request baseline](contracts/runtime-game-session-v1.json) and
[successful-result baseline](contracts/runtime-game-session-responses-v1.json).
The selected methods are:

| Family | Methods |
| --- | --- |
| Runtime boundaries | `runtime.status`, `runtime.inspect`, `runtime.entity`, `runtime.step` |
| Typed state | `runtime.components`, `runtime.component.get`, `runtime.component.query`, `runtime.observe`, `runtime.gameplay.inspect` |
| Spatial queries | `runtime.raycast`, `runtime.hit_test` |
| Logical UI | `runtime.ui.inspect`, `runtime.ui.activate` |
| Gameplay save tickets | `runtime.save.status`, `runtime.save.result` |
| External storage | `save.status`, `save.inspect`, `save.write`, `save.load` |

Full and focused `world.describe` replies carry
`runtime_game_session_contract`: ID `poima.runtime-game-session`, version `1`,
status `stable`, selected method names and baseline filenames. Discovery's
`schema_revision`, the engine version and this contract's major version are
independent. The header is reserved metadata, not a selectable section.

Read `availability` as well as the identity. `implemented` describes compiled
backends; `qualified` additionally requires a supported target and owner
context. `target`, `backends`, `profile` and `reason` identify that boundary.
These fields do not say that a runtime is active, a module is trusted, storage
is configured, or a particular call is currently authorized.

The initial qualified target is **Windows x86_64**, in three contexts:

| Profile | Owner |
| --- | --- |
| `source_world` | Standalone authoring; `session_scope` is absent. |
| `source_shared_player` | Shared-headless authoring. The name does not require an attached player. |
| `readonly_game` | Shared-headless exported game; authored bundle content is immutable. |

Readonly games can still advance runtime state, execute logical controls and
save outside their bundle. Shared-editor and other targets retain their
discovered APIs but are not qualified by this release. Linux authoring-core
support is separate. A backend flag is not a claim that both native and CoreCLR
gameplay are loaded or required for every query. The exported-game profile
additionally requires the native gameplay backend.

Runtime creation, module admission, storage configuration and deployment are
separately discovered operations. In particular `runtime.start`, `runtime.stop`,
`runtime.gameplay.load_native` and `save.configure` remain outside this contract.
A [game service](GAME_SERVICE.md) can perform admission before attachment.

## Requests, reads and controls

The request baseline preserves complete native schemas, constraints and
defaults. Stable base behavior includes ordinary move/look/jump/use and named
actions; kinematic motion, sound and regular animation/layer commands; registered
typed components and bounded collections; logical UI; and checkpoint operations.
The release manifest explicitly identifies experimental request branches:
step IK, lens, stance and root-motion commands, and arbitrary save-upgrade plans.
Their discovery is retained rather than silently removed. Other APIs and
rendering, physical input, C# source/binary ABI and save-file formats have their
own contracts.

Use actual returned session, tick and revision guards. `runtime.entity` permits
an omitted tick; `runtime.inspect` and `runtime.components` accept a session
without a tick. Component query and UI page limits default to 64. Pages are
sorted, with exclusive `after` cursors. UI continuation requires the matching
`ui_revision`; component continuation can be pinned with `structure_revision`.
An explicit empty selector differs from omission. Joined observation defaults,
selected keys, schema inclusion and typed decimal-text int64 values retain
their documented meanings. See [runtime observation](RUNTIME_OBSERVATION.md).

Spatial queries inspect committed collision geometry at an explicit tick.
They return a nullable closest hit, with entity, fraction, distance, position,
nullable normal and nullable triangle. Hit tests additionally report the
paired nullable damage-region name and region entity. `ignore` excludes the
supplied entities. There is no `structure_revision` parameter on these two
methods. Rendered meshes without a collision representation are not implicitly
pickable; these calls do not implement damage or ballistics.

Compiled `runtime.ui.activate` runs Control without advancing Tick and checks
logical eligibility and supplied guards. Step and Control receipts distinguish
the initiating boundary from the current boundary. A serviced load can replace
the session and tick. Validate historical replies against their original
requests, not a later inspection. A rejected callback preserves the published
native state covered by its transaction; trusted code's external side effects
are not rolled back. Reads are not a multi-call atomic snapshot. Mutations
require no attached player or a paused interactive player, and are rejected
during replay. See [logical UI](GAME_UI.md).

## Saves and bounded retry safety

Storage configuration generation, slot generation and gameplay operation epoch
are different identities. Use an ordinary external save root. Capture and
restore use the [save system's](SAVE_SYSTEM.md) native guards and trusted image
requirements. This contract preserves same-image restoration; arbitrary schema
evolution requires an explicit, separately qualified
[upgrade plan](SAVE_UPGRADES.md).

`allow_recovery` on load and `acknowledge_recovery` on write default to false
and require literal JSON Booleans. Inspection never grants consent. Loading a
verified previous payload requires explicit `allow_recovery:true`, leaves the
slot intact, and installs a new session/epoch. Continuing a damaged slot with a
new healthy write requires explicit acknowledgement. Exact restored state
before callbacks differs from globals changed by the subsequent lifecycle.

Accepted Step and Control operations have separate 32-entry owner-local receipt
rings. Load and storage-configuration outcomes share a 32-entry owner-local
pool: reconfiguration can evict a load receipt. Rejected requests do not all
receive replay receipts. Save writes retain 32 durable commitment receipts per
slot. A write retry uses the
same operation identity and content with the current storage routing generation;
reattaching or starting a new owner does not authorize publishing it again.
The replay reports current routing and the original committed payload identity.
Changed retained request reuse is rejected. These bounds are not unbounded
exactly-once execution, and eviction is not permission to retry blindly.

The gameplay terminal ledger retains at least 64 outcomes. Ticket states retain
their meanings: 0 Expired, 1 Queued, 2 Resolving, 3 Succeeded, 4 Failed.
Unresolved publication must remain distinguishable from a terminal failure.
Result reads are nonconsuming; unknown or evicted tickets may expire. An
accepted callback can commit its state and later report a storage failure.
Historical pending replies may refresh save completion metadata. A timeout
does not establish failure or make a fresh mutation safe. See
[gameplay save operations](GAMEPLAY_SAVES.md).

## Validation and compatibility

The Python validator is opt-in and returns the original result unchanged:

```python
from poima_client.game_session_responses import validate_game_session_result

params = {"session_id": session, "tick": tick, "type": component_type}
result = client.call("runtime.component.query", params)
validate_game_session_result("runtime.component.query", result, params,
                             {"profile": "readonly_game"})
```

Pass trusted `component_schemas` and `gameplay_schema` context when validating
dynamic typed values. Response-only validation without the original request or
schema context is weaker. No schema is fetched automatically. The validator
does not change normal `WorldClient.call`, retry, reconnect, normalize results
or establish that an operation was available. `ResponseContractError` can mean
that a successful mutation returned an incompatible result; inspect state
before choosing recovery. The frozen packaged manifest is independent of the
separate [prospective 21-method validator](WORKFLOW_RESPONSE_CONTRACT.md).

Request compatibility preserves accepted old inputs, omission behavior and
defaults. Result compatibility preserves required fields, types, bounds and
meaning, including transitive nested definitions. Exact selected maps cannot
gain unsolicited fields. Additive diagnostics are allowed where the result
schema permits them. Frozen enum meanings cannot change silently; a new value
needs compatibility review and explicit extension negotiation or a new major.
The compatibility scripts conservatively reject unsupported or unprovable
schema changes. Static schema checks do not replace native behavior checks.

Check an actual full discovery against the release, then check a proposed
producer result manifest separately:

```sh
python scripts/check_game_session_api.py --profile readonly_game \
  --candidate discovery.json \
  --release docs/contracts/runtime-game-session-v1.release.json --report requests.json
python scripts/check_game_session_responses.py \
  --baseline docs/contracts/runtime-game-session-responses-v1.json \
  --candidate proposed-responses.json --report responses.json
```

Request checking without `--release` is explicitly schema-only. A detached
projection cannot prove that an actual host advertised the qualified identity.
An unchanged known recursive result-definition closure can be compared exactly;
changed recursive or unsupported reference semantics remain unproven.

The error promise is condition-specific, not a freeze of message wording,
`error.data`, or precedence when several inputs are wrong:

| Condition | Code |
| --- | --- |
| Malformed parameters | `-32602` |
| Unsupported build | `-32003` |
| Missing runtime entity | `-32004` |
| Stale tick, revision or save CAS | `-32009` |
| Changed retained operation reuse | `-32010` |
| Runtime session mismatch | `-32030` |
| Compiled callback failure or ineligible control | `-32040` |
| Storage, restore or recovery denial | `-32070` |
| Player/replay scope restriction | `-32080` |

Save operation session conflicts can use `-32009`, rather than the runtime
read guard's `-32030`. Each method's documented prerequisites still apply.
Qualification records distinguish portable checks, retained consumers, native
contexts and independent games. None imply physical-input, clean-install,
graphics-quality or representative-performance qualification.
