# Persistent authored-world service

`poima world <path>` opens a native, headless editing session. The parent directory must already exist. Input and output use UTF-8, one JSON-RPC 2.0 object per line. Flush each request; responses are flushed immediately. EOF or `session.close` ends the session. This service owns persistent authored entities, hierarchy and built-in components. It can evaluate world transforms and capture an authored scene through the optional Vulkan preview; the optional [runtime](RUNTIME.md) adds physics and controller stepping without rewriting authored state. See [scene capture](SCENE_CAPTURE.md).

People using Poima’s desktop editor or CLI and external agents use the same authoritative operations. The editor is a first-class part of Poima; this shared implementation keeps human and automated edits consistent. `WorldSession` in `poima/world.hpp` exposes an in-process client with owned authored/runtime snapshots and an external inspection camera; the CLI adapts this service to stdin/stdout. Clients serialize access within a session. `serve` and editor `--endpoint` now expose [shared local sessions](SHARED_SESSIONS.md) through `connect`. An MCP adapter remains future work. Discovery is available through `poima schema world`, then `world.describe` inside a session.

Schema revision 21 added `runtime.status` and scope-aware shared-session discovery. Schema revision 20 added `world.history`, `world.undo` and `world.redo`. Undo/redo use `request_id` and `base_revision`, advance the revision, preserve known entity identities and store durable retry receipts. History is session-local, capped at 32 edits and 16 MiB of compact serialized entity snapshots. A new committed edit clears redo. Failed edits/previews preserve history. See the [history contract and native API](EDITOR.md#shared-service-and-history) for limits and restart behavior.

## Example

Start `./build/headless/poima world build/example.world.json`, then send these lines. The example expects a new document:

```json
{"jsonrpc":"2.0","id":1,"method":"world.inspect"}
{"jsonrpc":"2.0","id":2,"method":"world.transact","params":{"request_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","base_revision":0,"ops":[{"op":"entity.create","id":"11111111111111111111111111111111","name":"Harbour"},{"op":"entity.create","id":"22222222222222222222222222222222","name":"Dock","parent":"11111111111111111111111111111111"}]}}
{"jsonrpc":"2.0","id":3,"method":"entity.query","params":{"revision":1,"parent":"11111111111111111111111111111111"}}
{"jsonrpc":"2.0","id":4,"method":"entity.get","params":{"id":"22222222222222222222222222222222","component":"Transform","revision":1}}
{"jsonrpc":"2.0","id":5,"method":"session.close"}
```

The successful transaction returns revision 1, `committed: true`, `replayed: false` and the changed IDs. Every committed transaction persists before reporting success. An unopened document is empty at revision 0; it gets its persistent world ID and file on the first commit. Opening and inspecting alone can create a lock sidecar but does not create the world document. Its provisional world ID can change on reopening before that first commit.

## Operations

| Method | Behavior |
| --- | --- |
| `world.describe` | Parameter schemas, Transform schema, limits and invariants. |
| `world.inspect` | World identity, revision, entity count, persistence and coordinate convention. |
| `entity.get` | One entity, or its selected built-in component; optional revision guard. |
| `entity.world_transform` | Evaluated column-major world matrix; optional revision guard. |
| `world.lighting` | Resolved enabled lights, ambient fill, exposure and fallback status; optional revision guard. |
| `world.capture` | Capture an exact authored revision using a selected Camera; optional graphics build. |
| `entity.query` | Compact, ID-sorted hierarchy pages. Omit `parent` for all entities, use null for roots or an ID for direct children. |
| `world.transact` | Validate a complete operation batch, preview it or persist it atomically. |
| `session.close` | Return a response and close; valid as a notification too. |

Transactions support `entity.create`, `entity.rename`, `entity.reparent`, `entity.delete`, `component.set` and `component.remove`. `world.describe` supplies their exact required fields. Unknown fields and component types are rejected. Create initializes a mandatory Transform with identity rotation/scale and zero position. Reparenting requires explicit `mode: "keep_local"`; preserving world pose is not implemented. Delete requires an explicit `recursive` boolean, and rejects nonrecursive deletion of a parent with children. Deleted IDs remain retired for public creation; known undo history can restore the original entity.

Entity IDs and transaction `request_id` values are caller-generated 32-character lowercase hexadecimal strings, such as a UUID without hyphens. Entity identity is scoped to its world. These transaction IDs differ from the JSON-RPC envelope's response-correlation `id`.

Transforms use a right-handed, Y-up coordinate convention in meters. Position/scale have three numbers, and rotation is a normalized XYZW quaternion (squared-norm tolerance 0.000001). Scale must be positive. All numbers must be finite and have absolute value at most one billion. Names contain 1–256 UTF-8 bytes. The final hierarchy must have existing parents and no cycles; a child may precede its parent creation in the same batch.

## Iteration, conflicts and retries

Read a revision, submit that revision as `base_revision`, and inspect only the entities needed for the next edit. Transactions return changed IDs rather than echoing the full scene. `preview: true` validates the proposed state and returns the proposed next revision with `committed: false`; it neither changes the current revision nor stores a receipt. Whole-document size is enforced during commit; a preview is not a storage-capacity guarantee.

The last 128 committed transactions retain their normalized parameters and original results in the document. Retrying an identical transaction after a lost response or process restart returns that original result with `replayed: true`. It does not repeat the edit or rewind current state. A reused ID with changed parameters fails. An evicted receipt cannot replay; its stale base revision still prevents applying the old edit again. This is a bounded retry window, independent of the session-local undo stack.

Queries can filter by built-in component type. They default to 64 results, up to 256. Continue using the returned `next_after` value as `after` and the same returned `revision`; null means no more results. Continuation requires a revision so an intervening edit produces a conflict instead of silently mixing snapshots. Entities are sorted by ID, not presentation order. Hierarchy queries return names, parents and component type names; get the selected component only when needed.

## Storage and recovery

The version-1 `poima.authored-world` JSON document contains entities, retired IDs and retry receipts. The service owns four sidecar names: `.lock`, `.pending`, `.previous` and `.previous.pending`. Do not edit or repurpose them while a session runs.

A cooperative OS lock allows one writer session per document. An empty `.lock` file can remain after closing; file existence does not indicate ownership. Windows and Linux locking were tested separately, not simultaneous cross-OS access through WSL. Direct external editors do not participate in the lock. The service checks file bytes before committing and rejects detected external changes, but does not promise synchronization against concurrent uncooperative writers or aliasing through hard links.

Commit serializes and flushes a staged candidate, publishes the previous primary snapshot as `.previous`, then atomically replaces the primary. In-memory state changes only after publication. Storage failure before primary replacement preserves the current world. An interrupted session can leave staging files; startup ignores them. Power-loss directory durability and interruption at every filesystem instruction are not qualified.

A malformed primary is rejected without silently replacing it with a backup. To recover, close writers, preserve the bad file for diagnosis and explicitly restore `.previous` over the primary. The backup is one prior snapshot, not a history. Opening a valid document reconstructs its authored state and receipts.

## Limits and diagnostics

The initial service allows 10,000 live entities, 256 operations per transaction, a 16 MiB world document including receipts/retired IDs, 1 MiB request lines, and JSON nesting up to 64. Revisions are safe JSON integers through 2^53−1. Transactions copy and validate the document; large-world performance, long-lived deletion histories and streaming are not qualified. These limits bound an initial implementation, not final engine capacity.

Requests use JSON-RPC string/integer/null IDs. Notifications execute without responses, including errors; use requests for acknowledged mutations. JSON-RPC batch arrays are unsupported. Duplicate object keys are rejected. Method parameter schemas are provided; result schemas are not yet published.

| Code | Meaning |
| --- | --- |
| `-32700` | Malformed JSON, duplicate keys, excessive nesting or oversized request line. |
| `-32600` | Invalid JSON-RPC envelope or unsupported batch. |
| `-32601` | Unknown method. |
| `-32602` | Invalid parameters, hierarchy, transform or other validation failure. |
| `-32003` | Scene renderer is not built. |
| `-32004` | Missing entity or component. |
| `-32009` | Stale revision or detected external file change. |
| `-32010` | Transaction ID reused with different parameters. |
| `-32020` | Scene rendering/capture failed. |
| `-32000` | Storage/internal operation failure; details on stderr. |

Per-request failures leave the session available. Startup failures use the CLI's one-shot error envelope and exit 4. Valid EOF/close exits 0. Authoring needs no renderer, graphics driver or Python runtime. The optional capture operation needs the graphics build and a native display.

## Verification and remaining work

`tests/world_contract.py` drives the compiled process using Python's standard library. The suite contains nine cases; Windows interoperability skips abrupt process termination, while the other cases cover Unicode filenames and content, persistence, previews, rollback, hierarchy pagination/deletion, retry eviction, malformed requests, write failures, writer exclusion and explicit backup restore. Abrupt termination and lock release are tested on Linux; the WSL harness skips that case for Windows. [Initial service evidence](evidence/m1-world-service.json) and [expanded scene evidence](evidence/m2-scene-capture.json).

This is the initial M1 authoring foundation. The optional EnTT/Jolt runtime now provides simulation ECS storage, fixed ticks and live snapshots through additional runtime commands. Shared native sessions, undo/redo and an initial native editor now exist. Custom component schemas, C# component bindings, prefabs, mature editor workflows remain unimplemented. Shared local multi-client transport now exists. M0 foundation experiments also remain open; this delivery does not complete either milestone.

The [continuous player](PLAYER.md) is exposed as `runtime.play` in schema revision 4. It currently blocks this connection while the window runs, retains live state on return, and shares runtime retry receipts with `runtime.step`. Shared desktop clients use `desktop.capture` with a Scene/Game target and `desktop.play.*`; the legacy ImGui frontend exposes `editor.capture` and its own editor controls. Standalone continuous-player attachment remains future work.

Schema revision 7 adds [static asset import, inspection and instantiation](ASSETS.md), plus `StaticMesh` and `PbrMaterial`. Keep the `<world>.assets` directory with the world document. Import writes cooked assets without changing the authored revision; instantiation uses normal transactions.

Image imports and effective material inspection are available through `asset.image.import`, `asset.image.inspect` and `entity.material`; per-object maps use `PbrTextures`. See the [material authoring contract](MATERIAL_AUTHORING.md) for inheritance, null overrides, resource-resolution errors and frozen runtime behavior.

Schema revision 8 adds [authored lighting](LIGHTING.md): `Light` and `LightingEnvironment`, plus `world.lighting` and `runtime.lighting`. Captures/player results include resolved lighting. A scene with any authored lighting suppresses the old preview fallback, including when its lights are disabled.

Schema revision 9 adds [shadow settings and budgets](SHADOWS.md) to Light and LightingEnvironment. Resolved lighting reports effective settings, view count and depth-texel storage.

Schema revision 10 adds `culling` and `profile` controls to world/runtime capture and runtime play, with [draw counters and CPU/GPU timing](RENDER_DIAGNOSTICS.md) in their results.

Schema revision 11 adds `BoxCollider.motion: "kinematic"`, `motions` on runtime steps/replay segments, `runtime.raycast`, and live motion fields on `runtime.entity`. See [physics interactions](PHYSICS_INTERACTIONS.md).

Schema revision 12 adds `runtime.gameplay.load`, `.inspect`, `.edit` and `.collect`, plus a first-tick `use` input on steps and replay segments. The optional C# module has its own gameplay revision and shares runtime retry receipts; it does not add authored gameplay components or persistent game saves. See the [managed gameplay contract](MANAGED_GAMEPLAY.md).

Schema revision 13 adds `AudioEmitter`/`AcousticMaterial` components, `asset.audio.import`/`.inspect`, and authored/runtime `audio.inspect`/`audio.capture`. The optional Steam Audio backend renders frozen direct-path/HRTF WAV observations without advancing gameplay or opening a device. [Native audio contract](AUDIO.md).

Schema revision 14 adds native sound commands to `runtime.step` and player replay segments, `runtime.audio.voices`, `runtime.audio.replay`, and the optional `runtime.play` audio switch. See [sound-event contracts](AUDIO_EVENTS.md) for bounds, partial-progress behavior and receipt semantics. Frozen audio capture remains a separate observation.

Schema revision 26 adds `runtime.gameplay.load_native` for an inventoried Native AOT artifact, with existing runtime tick/revision guards and retry semantics. An optional expected descriptor hash binds loading to previously inspected metadata. [Native gameplay contract](NATIVE_GAMEPLAY.md).

Schema revision 27 adds `save.status`, `save.configure`, `save.inspect`, `save.write` and `save.load`. Saves use an explicit external root, persisted write receipts and guarded staged replacement without changing the authored document. See the [save-slot contract](RUNTIME.md#durable-save-slots) for examples, frozen content binding, recovery and current limits.
