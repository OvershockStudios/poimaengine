# Explicit save upgrades (development)

The external world service can load an older checkpoint into a changed compiled game through one explicitly selected source-to-target upgrade plan. Ordinary restores remain exact. C# gameplay load requests and editor save controls do not yet select upgrade plans automatically.

Current integration evidence covers Linux CoreCLR: a preserved Collection Room save resumes under a renamed/reordered game state with an added default, and a separate two-build component fixture preserves authored and spawned instances through real target callbacks and a fresh-process restore. Windows and Native AOT integration qualification is pending. This is not a production compatibility promise for arbitrary game updates.

[Qualification evidence](evidence/m2-save-upgrade-integration.json) records the exact sources, artifacts, tests and remaining platform limits.

## Supported changes

Global state uses [persistent field IDs](GAMEPLAY_PERSISTENCE.md). A retained ID can change its C# name or layout position, but not its scalar kind. New IDs receive the target's literal persistence defaults; changed defaults never overwrite retained saved values. Removed fields require explicit retirement. A legacy source without persistence metadata requires a complete, explicit old-name-to-ID mapping.

Existing custom component types follow the same rules. Their stable type IDs and instance membership must remain unchanged. Retained kinds and units cannot change. Both authored entities and spawn-template values must match the explicitly approved transformation. Runtime values are mapped from the saved instances, not reset to authored values or new schema defaults.

The first implementation does not support backend changes, numeric conversions, callbacks, upgrade chains, collection resizing, new or removed component types/instances, geometry changes, or general world rebasing. World ID, module identity and gameplay type remain the same. Built-in components, entity topology, templates, assets, animation, sound and UI definitions must remain unchanged. Only approved component declarations/values and the authored revision may change.

## Plan contract

`poima.save-upgrade` version 1 is bounded to 1 MiB and rejects duplicate keys, unknown members and excessive nesting or field counts.

| Member | Meaning |
| --- | --- |
| `format`, `version` | `"poima.save-upgrade"`, integer `1`. |
| `id` | Nonzero lowercase 32-hex plan identifier. |
| `source`, `target` | Exact identities, each containing `world_id`, `content_sha256`, `backend`, `module_identity`, `type`, `image_sha256`, `schema_sha256`. |
| `global` | `preserve`, `retire`, `default` lists of stable global field IDs. |
| `legacy_global_ids` | Optional source-only list of `{name,id}` records; required for legacy sources and forbidden for sources already carrying stable IDs. |
| `components` | Entries containing `id`, `source_fingerprint`, `target_fingerprint`, and the three mapping lists. Unlisted types must have unchanged complete schemas. |

Each mapping list is sorted by ID, unique and disjoint from the others. Every source field is preserved or retired; every target field is preserved or defaulted. Retirements name source-only IDs and defaults name target-only IDs. Component entries and legacy records are sorted by stable ID. The limits are 128 global fields, 32 fields per component and 64 component types.

Hashes are lowercase SHA-256. `schema_sha256` binds the complete schema serialized through the engine's JSON serializer, including layout and persistent/component metadata. `content_sha256` is the owner's frozen world-and-asset identity, not the hash of the world JSON file. Component fingerprints alone do not establish unchanged labels or units; those are checked separately. Do not infer an identity from a version number or filename.

## Loading

Add an `upgrade` selection and explicit non-null `gameplay` selection to an otherwise ordinary guarded `save.load` request:

```json
"upgrade": {
  "path": "upgrades/release-2.json",
  "expected_sha256": "<SHA-256 of the exact plan file bytes>"
}
```

This is a request fragment; retain all [save-load guards](RUNTIME.md#durable-save-slots) and use current observations. `revision` guards the current authored target world. Runtime session/tick/gameplay/component/structure/UI/control guards still protect a live replacement. Pending gameplay saves must be resolved first. Relative plan paths resolve beside the authored world.

The host chooses the plan and executable. Saved bytes never choose paths or grant authority. A plan hash verifies bytes; it is not a signature or authorization policy. The original saved document, asset closure and snapshot are validated before transformation. Target code is checked against the exact edge, the mapped snapshot passes the ordinary target restore checks, and the replacement is published only after result and retry data are prepared.

`Initialize` is skipped. Trusted CoreCLR constructors/static initializers may still run during metadata inspection, and their external effects cannot be rolled back. The same registered CoreCLR instance is consumed by restore. Native AOT process-level image pinning remains in effect; changing a pinned image requires a fresh process.

The result includes an `upgrade` report with plan, content, image and schema digests plus field/instance counts. It does not dump saved values. A retained exact request retry returns its receipt without rereading the plan or activating again, even if the plan file has since been removed. Different parameters under the same retained request ID reject.

Recovery remains separate consent: `allow_recovery:true` permits the store-selected prior generation. The plan must match that selected checkpoint. Results distinguish the manifest generation from `selected_generation`; last-restore metadata records the selected generation.

Successful loading does not overwrite the original slot or current authored document. Explicitly save a new generation or slot when ready to make the upgraded state durable. Snapshot and storage work remain synchronous.

## Tests

The native `poima-save-upgrade-test`, `poima-save-upgrade-plan-test`, `poima-save-upgrade-document-test` and `poima-save-upgrade-snapshot-test` exercise mapping, plan guards, allowed authored changes and snapshot preservation. They do not establish executable migration by themselves.

`tests/save_evolution_game.py` consumes a separately preserved, verified source baseline and a real compiled target. See its [fixture instructions](../tests/fixtures/save_evolution_game/README.md). `tests/save_evolution_components.py` consumes separately compiled source/target modules and their manifests. These integration runners exercise actual callbacks, explicit saving and fresh-process continuation. Keep source/target build outputs separate and retain the original artifacts when testing historical compatibility.
