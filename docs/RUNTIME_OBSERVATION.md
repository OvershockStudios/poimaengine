# Coherent runtime observation

`runtime.observe` reads selected live entity state and native custom-component
values in one owner-serialized request. Added in **0.0.54 development**, discovery
**schema revision 51**, it has native Linux/Windows protocol qualification and a
Windows replay comparison. It remains outside the released [authoring-core v1
contract](AUTHORING_API_COMPATIBILITY.md).

Discover the available request schema before using it:

```json
{"jsonrpc":"2.0","id":1,"method":"world.describe","params":{"view":"method","name":"runtime.observe"}}
```

The operation needs an active simulation. It reads committed runtime state,
including spawned entities, rather than the frozen authored hierarchy. It does
not advance a tick, invoke gameplay callbacks, change values, service saves,
render, or retain a snapshot or observation cache. Gameplay values and logical UI
contents remain available through `runtime.gameplay.inspect` and
`runtime.ui.inspect`; this operation returns their revision tokens only.

## Select entities and component fields

Every request requires `session_id` and an exact `tick`, plus nonempty `ids` or a
`query`. Select known entities with `ids`, obtain a sorted page through `query`,
or combine both. Each selected entity is returned once, in ID order. An empty
selection cannot be used for a header-only read.

`components` selects up to four registered native custom-component types for
the returned entities. Types and field IDs are **32 lowercase hexadecimal
digits**, without the authored `game:` prefix. Field selection uses stable field
IDs, not C# names or Inspector labels. Omitting a selector's `fields` returns all
its fields; `fields: []` reads membership only. A present component then returns
`{}`, while absent membership returns `null`. Unknown entities, types or fields
are errors; the operation does not return partial success.

This illustrative request assumes the named entities and types exist in the
active runtime at tick 12:

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "method": "runtime.observe",
  "params": {
    "session_id": "10000000000000000000000000000001",
    "tick": 12,
    "ids": ["b1000000000000000000000000000001"],
    "query": {"type": "a2000000000000000000000000000001", "limit": 32},
    "entity_fields": ["world_matrix", "layout", "yaw", "pitch"],
    "components": [
      {"type": "a1000000000000000000000000000001", "fields": ["a1000000000000000000000000000002"]},
      {"type": "a2000000000000000000000000000001", "fields": ["a2000000000000000000000000000002"]}
    ]
  }
}
```

Native values retain their existing encodings: int64 values use lossless decimal
strings, entity references use native IDs, and bounded collections retain their
order. This operation does not convert them into display text or managed objects.

`entity_fields` is a unique selection from `world_matrix`, `layout`,
`local_transform`, `animation`, `motion`, `kinematic_target`,
`motion_remaining_ticks`, `velocity`, `has_body`, `is_character`, `ground`, `yaw`
and `pitch`. Its default is `world_matrix`, `layout`, `velocity`, `has_body`,
`is_character`, `ground`, `yaw` and `pitch`. Matrices retain the existing
column-major representation. Request motion or animation details explicitly when
comparing a checkpoint or debugging transitions.

## Read the response and preserve guards

The response carries one common observation header: `session_id`, `tick`,
`structure_revision`, `component_revision`, `gameplay_revision`, `ui_revision`
and `control_sequence`. It also reports `authored_revision`,
`current_authored_revision` and `source_stale` for the runtime's frozen source.

Each row under `entities` contains `id`, the selected `state` fields, and a
`components` object keyed by requested type ID. `component_types` reports each
selected type's `type` and `fingerprint` once. `include_schemas: true` adds its
`schema`; the default is false, so each row does not repeat schema metadata.
`query` is `null` when omitted, otherwise it contains `type`, the page's sorted
`entities` IDs and `next_after`.

Optional request pins `structure_revision`, `component_revision`,
`gameplay_revision`, `ui_revision` and `control_sequence` must match exactly.
An initial read can omit them and obtain the current tokens. A supplied stale
pin rejects the complete read.

Ticks do not identify every runtime change. Component edits, logical UI edits
and compiled controls can change state without advancing physics. For a query
continuation, pass `query.after` together with **both** `structure_revision` and
`component_revision` from the prior observation, and retain the same session and
tick. The cursor is exclusive and need not name an existing entity. A final page
returns `next_after: null`, including a full page when there is no further match.

These tokens are guards, not retained snapshots. Another client or the playback
owner can change the runtime between requests. A continuation or follow-up edit
then fails its relevant guards; inspect again and make a new decision. The
operation does not automatically replay a read or repair a stale request.

## Bounds and availability

| Selection | Bound |
| --- | --- |
| Explicit entity IDs | 32 unique IDs |
| Component-membership query | One page; limit 1–64, default 32 |
| Combined returned entities | 96, sorted and deduplicated |
| Entity-state fields | 13 unique known fields |
| Selected component types | Four unique types |
| Fields per component selector | 32 unique stable IDs |
| Serialized result | 1 MiB of UTF-8 result JSON, excluding the JSON-RPC envelope |

Malformed selectors, unknown fields, duplicate selections and excessive bounds
are errors. A result exceeding the byte budget fails rather than silently
truncating text, arrays or selected fields. No retry receipt or mutation
`request_id` is needed.

The read is intended for standalone sessions, shared headless/editor clients and
packaged read-only runtime worlds. Use discovery and `runtime.status` to check
the current build and active session. Existing runtime inspection methods keep
their contracts. See [runtime simulation](RUNTIME.md), [custom
components](CUSTOM_COMPONENTS.md) and [shared sessions](SHARED_SESSIONS.md).

## Reproduce the replay comparison

The [retained Workshop Relay fixture](evidence/fixtures/agent-workshop/README.md)
provides source and Windows CoreCLR build instructions. Build the game and engine
first, then run native Windows Python with the same inputs as its public replay:

```powershell
python tests/agent_workshop_observation_compare.py `
  --engine build/windows-runtime/poima.exe `
  --world docs/evidence/fixtures/agent-workshop/world.json `
  --manifest docs/evidence/fixtures/agent-workshop/manifest.json `
  --assembly path/to/WorkshopRelay.dll `
  --hostfxr path/to/hostfxr.dll `
  --bridge path/to/Poima.ManagedBridge.dll `
  --source docs/evidence/fixtures/agent-workshop/game/Relay.cs `
  --source docs/evidence/fixtures/agent-workshop/game/WorkshopRelay.csproj `
  --output build/workshop-observation-comparison
```

Use a new output directory. The driver runs the public replay once with existing
individual reads and once with `--observe`. The compiled game, routes, checks and
checkpoint comparisons are unchanged; joined reads use no observation cache.
Local logs include paths and actual calls. Its `public-summary.json` contains an
allowlisted summary suitable for review.

The recorded Windows pair makes **2,549** versus **1,292** native RPCs, a **49.3%**
reduction. Compact reserialized request bodies total 352,169 versus 310,916 bytes
(11.7% less); result bodies total 1,654,466 versus 1,374,747 bytes (16.9% less).
Thirteen game checks, thirteen route events and six complete checked snapshots
match. Only opaque `session_id` values are normalized. Both modes independently
finish original play, compiled Load continuation and fresh-process continuation,
and close their two owned native processes with exit code zero.

Byte counts use compact Python UTF-8 reserialization of `{method,params}` and
parsed native results, excluding RPC envelopes and framing. They are not exact
native wire sizes. This provider-free pair does not measure model tokens,
latency, FPS or general agent success. Nine protocol tests pass on Linux and
Windows; shared/read-only scope checks and an actual authoring-only build also
pass. The 96-row bound is exercised with small values; byte-budget overflow and
stale nonzero compiled gameplay/control pins remain unqualified. The retained
game's documented limits still apply. [Evidence](evidence/m2-runtime-observation.json).
