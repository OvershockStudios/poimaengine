# Authoring-core v1

Poima 0.0.49 releases **authoring-core v1**, a small stability boundary for agents
and tools editing authored worlds. The rest of the engine remains in early
development. A stable authoring contract does not make the engine alpha-ready.

## Scope and discovery

The [release manifest](contracts/authoring-core-v1.release.json) pins the
[request baseline](contracts/authoring-core-v1.json),
[response baseline](contracts/authoring-core-responses-v1.json) and their hashes.
It selects `world.describe`, `world.inspect`, `world.history`, `world.transact`,
`world.undo`, `world.redo`, `entity.get`, `entity.query` and
`entity.world_transform`. Mutations cover entity creation, renaming, reparenting
with `keep_local`, deletion and setting `Transform`. Component selectors cover
`Transform`. Other component types and mutations remain experimental.

The baseline freezes **selected inputs and the behaviors below**, not every
parameter or subsystem reachable through those method names. It retains the
qualified schema46 inputs. Mutation-focused discovery is an additive extension;
it does not replace that baseline. Full discovery catalogs, arbitrary component
and section contents, renderer/runtime APIs, gameplay SDK/ABI, game saves,
world-file formats and editor-specific controls are outside v1. These operations
edit the authored world; they do not patch a running simulation.

A session's `world.describe` result carries `authoring_contract` metadata in full,
catalog, method, component, section and mutation views. Its `id` is
`poima.authoring-core`, `version` is `1`, and `status` is `stable`. It identifies the
selected methods/components/mutations and the session's mode and mutation
availability. Read-only sessions support v1 reads while hiding mutation schemas
and rejecting mutation calls. Mutation discovery itself is unavailable there.
The same boundary applies to standalone, shared-headless and shared-editor
request scopes. Shared-editor scope qualification is a native service check;
it does not qualify desktop presentation or physical input.

`protocol_version`, discovery's evolving `schema_revision` and this contract's
major version are independent. Contract-compatible additions can ship without a
v1 major bump. A breaking selected request, response or behavioral change needs
a new contract major version; it cannot silently retain the v1 claim. Old hosts
without identity metadata remain usable by the Python client, but do not thereby
claim the released contract.

## Request compatibility

Save a native authoring `world.describe` result or its JSON-RPC envelope. Project
it to the selected scope, then check that old requests remain accepted:

```sh
python3 scripts/project_authoring_api.py --discovery discovery.json --output scoped.json
python3 scripts/check_authoring_api.py --baseline docs/contracts/authoring-core-v1.json --candidate scoped.json --report requests.json
python3 tests/authoring_api_compatibility.py
```

The gate rejects removed requests, new required fields, narrowed accepted values
and changed defaults. It accepts only additions it can prove preserve the old
inputs. For an exclusive union extension, every old closed-object branch must
remain unchanged and each added branch must require a field forbidden by every
old branch. Unknown shapes and reference-bearing changes require review. This
proves old-request acceptance, not validity or uniqueness of every new input.

Projection excludes out-of-contract mutations while retaining constraints on
selected branches. References and unfamiliar context prevent pruning. The
historical `.candidate.json` file remains unchanged; the canonical v1 file has
identical selected schemas and a released publication status.

## Response compatibility

The response manifest specifies required shapes and variants for these nine
methods. The [Python client](PYTHON_CLIENT.md) also checks contextual behavior:
requested IDs/read revisions, original-base-plus-one mutation revisions,
preview/history outcomes, focused discovery selectors, sorted query IDs,
filters and exclusive cursors. Additional result fields are allowed. Legacy
retained transaction receipts may lack history metadata; fresh commits must
report their history outcome. A replay revision is not the current revision.

Response evolution runs in the opposite direction to input evolution:
**new producer outputs must fit the old consumer contract**. Use its separate
gate, never the request checker with arguments reversed:

```sh
python3 scripts/check_authoring_responses.py --baseline docs/contracts/authoring-core-responses-v1.json --candidate proposed-responses.json --report responses.json
python3 tests/authoring_response_compatibility.py
```

This is a conservative proof over recognized constraints, not a general JSON
Schema solver. Required result fields must remain required; types, literal
values and bounds must preserve old consumers' expectations. Added fields must
respect the old additional-property policy. Static local definitions are
resolved with bounded, acyclic scope; unsupported reference contexts, changed
unions or unknown constraints require review. A schema check cannot prove field
meaning, error codes or state changes. Native behavioral qualification is also
required. The SDK packages both canonical and historical response resources.

## Behavioral guarantees

- JSON-RPC 2.0 replies preserve valid caller IDs: strings, null or integers
  representable in signed/unsigned 64-bit storage (−2^63 through 2^64−1).
  String IDs are recommended for portable correlation. Revision bounds are
  separate from request-ID bounds.
  Notifications produce no response. Result and error envelopes remain distinct.
  The service is bounded, one request per line, without batches; shared transport
  framing is documented separately in [Shared sessions](SHARED_SESSIONS.md).
- Mutations require a base revision and commit atomically. A successful commit
  advances the revision exactly once. Rejected edits and previews leave authored
  state, history and durable receipts unchanged. Preview reports base plus one
  without reserving that revision, an entity identity or a retry receipt.
- Exact retries within receipt retention return the original result with
  `replayed: true`, even after intervening edits or reopening. Retained receipt
  comparison precedes the stale-revision check. Reusing a retained mutation ID
  with different parameters or another mutation method returns `-32010`.
  Omitted `preview` normalizes to false. Receipt retention is bounded and shared
  across authored mutations: currently the last 128, not 128 per method.
- Query pages sort by entity ID and use an exclusive cursor. Continuation
  requires a revision; stale reads fail rather than silently mix snapshots.
  Omitted parent selects all entities, null selects roots, and an ID selects its
  direct children. The default limit is 64, with an accepted range of 1–256.
  A continuation appears only when another matching entity remains.
- Transforms use meters, a right-handed Y-up world, normalized XYZW quaternions
  (squared-norm tolerance 0.000001), finite numbers with absolute value at most
  one billion and positive scale. World matrices are column-major compositions
  preserving hierarchical shear. Entity creation supplies an identity Transform.
  `keep_local` reparent preserves local values and recomputes the world matrix.
  Numerical comparisons use tolerance; cross-platform bit equality is not promised.
- Undo/redo restore identities and authored state, advancing the current revision.
  History is bounded and session-local: currently 32 entries and 16 MiB across
  both stacks. Oversized edits may skip recording and clear history; results
  report that outcome. Reopening clears stacks, and a new commit invalidates redo.
  Storage rejection does not consume history or a retry ID.
- `changed_ids` is a sorted unique set of affected IDs, not necessarily a minimal
  final-state diff. A world ID is provisional until the first persisted commit;
  reopening an empty unpersisted world may create a different ID.
- Read-only mode denies authored mutations before validating their parameters.
  Capability differences remain discoverable. Limits and policy outside selected
  input bounds remain advertised and bounded; preview is not a disk-capacity or
  crash-durability guarantee.

## Error categories

Codes for these defined conditions are stable. Diagnostic strings and precedence
between multiple unrelated invalid inputs are not. Retained retry before stale
revision, and read-only denial before mutation validation, are explicit exceptions.

| Code | Defined condition |
| --- | --- |
| `-32700` | Invalid JSON or oversized world-service request. |
| `-32600` | Invalid JSON-RPC envelope or unsupported batch. |
| `-32601` | Unknown method. |
| `-32602` | Invalid parameters, hierarchy or Transform. |
| `-32004` | Missing entity/component or unavailable undo/redo history. |
| `-32009` | Stale revision or detected external world-file change. |
| `-32010` | Retained mutation ID reused with different parameters/method. |
| `-32081` | Authored mutation in read-only mode. |
| `-32000` | Storage/internal operation failure. |

Runtime, graphics and scope-specific control errors are outside this table.

## Qualification

The native [world](../tests/world_contract.py),
[history](../tests/world_history_contract.py),
[conformance](../tests/authoring_core_conformance.py),
[identity](../tests/authoring_contract_identity.py) and
[scope](../tests/world_session_native.cpp) checks qualify the boundary alongside
independent response, request-projection and installed-client tests.
[Release evidence](evidence/m2-authoring-core-v1.json) records the exact builds,
commands and limits. Schema comparisons alone cannot release a contract.
