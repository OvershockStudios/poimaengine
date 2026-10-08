# Authoring API compatibility

Poima is preparing an **authoring-core v1** contract. It is a candidate, not a
released stability guarantee. The checked-in baseline records request schemas
from a qualified native host; behavioral qualification is a separate requirement.

## Scope

The candidate covers `world.describe`, `world.inspect`, `world.history`,
`world.transact`, `world.undo`, `world.redo`, `entity.get`, `entity.query` and
`entity.world_transform`. Mutations cover entity creation, renaming, reparenting
with `keep_local`, deletion and setting `Transform`. Component selections cover
`Transform`; other components remain experimental.

These operations act on the **authored world**. They do not patch a running
simulation. Rendering, gameplay SDK/ABI, runtime snapshots and saves, other
component types, and editor-specific controls are outside this contract.
Discovery's `schema_revision` describes the evolving catalog; it is not this
contract's major version. Build capabilities and read-only/shared-session
metadata may legitimately differ.

## Compatibility gate

Save the native `world.describe` result, or its JSON-RPC response envelope, to a
JSON file from an **authoring** session. Read-only runtime sessions intentionally
omit mutation methods and are qualified separately. Project authoring discovery
to the candidate scope and compare it with the baseline:

```sh
python3 scripts/project_authoring_api.py --discovery discovery.json --output scoped.json
python3 scripts/check_authoring_api.py --baseline docs/contracts/authoring-core-v1.candidate.json --candidate scoped.json --report compatibility.json
python3 tests/authoring_api_compatibility.py
```

The gate rejects removed requests, required-field additions, narrowed accepted
values and changed defaults. It ignores descriptive prose. It accepts only
additive changes it can prove preserve the baseline's accepted requests.
Unproven changes require review; this is deliberately not a general JSON Schema
subsumption solver. A passing request check does not establish compatible
responses, error codes, persistence, timing or simulation determinism.

The projection excludes mutation branches outside the contract. It retains the
selected branches and their constraints, so removing a selected operation or
tightening its schema still fails the gate. Candidate component selectors retain
their full constraints; the baseline requires only `Transform` to remain valid.

## Behavioral requirements before release

- Preserve the caller's JSON-RPC ID and version, documented result fields and
  error codes. Response additions must not require callers to change.
- Mutations use a revision guard and commit atomically. A valid commit advances
  the revision once; rejected edits and previews leave the document, history and
  retry receipts unchanged. A preview reports a candidate revision, not the
  current committed revision.
- An exact retry within advertised receipt retention returns its original
  result, marked replayed, even after intervening edits or reopening. Reusing its
  request ID for different parameters fails. Receipts are bounded, not permanent.
- Query pages sort by stable entity ID, use an exclusive cursor and require a
  revision when continuing a page. Omitted parent means all entities; null means
  roots; a parent ID means its children.
- Transforms use meters, a right-handed Y-up world, normalized XYZW quaternions
  and positive scale. World matrices are column-major and preserve hierarchical
  shear. Entities receive an identity transform by default.
- Undo/redo preserve restored identities, advance the current revision and
  remain bounded, session-local history. Reopening clears the stacks. A new
  committed edit invalidates redo.
- Read-only sessions hide and reject mutation methods. Capability differences
  must remain discoverable rather than silently accepting unavailable work.

The native [world](../tests/world_contract.py) and
[history](../tests/world_history_contract.py) suites exercise these behaviors.
The candidate cannot become a released contract from a schema comparison alone.
Until that release, projects should pin an engine revision and retain their own
behavioral checks.
