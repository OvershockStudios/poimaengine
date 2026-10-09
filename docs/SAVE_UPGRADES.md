# Explicit save upgrades

The external world service can load an older checkpoint into a changed compiled game through one explicitly selected source-to-target upgrade plan. Ordinary restores remain exact. C# gameplay load requests and editor save controls do not yet select upgrade plans automatically.

The contract below includes hierarchical checkpoints and bounded component arrays. Windows/Linux CoreCLR and Native AOT retained-save checks are recorded in the [hierarchical upgrade evidence](evidence/m2-hierarchical-save-upgrades.json). Check [implementation status](IMPLEMENTATION_STATUS.md) for their exact scope and limits. The earlier [scalar integration evidence](evidence/m2-save-upgrade-integration.json) covers its recorded Linux CoreCLR fixtures; it does not establish the newer hierarchy/array workflow or other backends. Explicit upgrades are not a compatibility promise for arbitrary game updates.

## Supported changes

Global state uses [persistent field IDs](GAMEPLAY_PERSISTENCE.md). A retained ID can change its C# name or layout position, but not its scalar kind. New IDs receive the target's literal persistence defaults; changed defaults never overwrite retained saved values. Removed fields require explicit retirement. A legacy source without persistence metadata requires a complete, explicit old-name-to-ID mapping.

Custom components support scalar schema 1 and bounded-array schema 2. Stable type IDs and instance membership remain unchanged. Retained scalar kinds/units and array element kinds/units cannot change. Fields may be renamed or reordered while retaining their persistent IDs. New scalar fields use approved target defaults, new entity fields default to null handles, and new arrays start empty. Removed scalar or array fields need explicit retirement. See [component collections](COMPONENT_COLLECTIONS.md) for their declared layouts and bounds.

Approved mapping covers ordinary authored entities, legacy root-prop templates and every root/child in a hierarchical recipe. Recipe-local entity references retain their local IDs; this does not allocate instances or rebase handles. Saved component values and ordered live-reference arrays come from the checkpoint, not new authored defaults. Complete source-runtime validation checks references even in fields being retired, before mapping can remove them.

Runtime snapshot versions 1–6 are supported within their existing format restrictions. Version-6 instance maps, initial root transforms, allocator lineage, native physics, animation clocks/layers/history, sound, UI and control sequence are preserved. A restored session and save epoch are new owner identities; they are not persistent object handles.

World ID, backend, module identity and gameplay type remain the same. Built-in components, entity topology, recipe roots/member IDs/names, assets/provenance and UI definitions stay exact. Only approved component declarations/values, gameplay schema/image identities and the authored revision may change. Numeric conversions, conversion callbacks, upgrade chains, added/removed component types or instances, geometry changes and general world rebasing are unsupported.

## Plan contract

`poima.save-upgrade` accepts versions 1 and 2, is bounded to 1 MiB, and rejects duplicate keys, unknown members and excessive nesting or field counts. Version 1 retains its original plan shape and forbids `array_capacity`. Version 2 permits explicit array-capacity authorizations; selecting it alone does not approve resizing. Existing scalar-only native mapping APIs retain their strict scalar contract.

| Member | Meaning |
| --- | --- |
| `format`, `version` | `"poima.save-upgrade"`, integer `1` or `2`. |
| `id` | Nonzero lowercase 32-hex plan identifier. |
| `source`, `target` | Exact identities, each containing `world_id`, `content_sha256`, `backend`, `module_identity`, `type`, `image_sha256`, `schema_sha256`. |
| `global` | `preserve`, `retire`, `default` lists of stable global field IDs. |
| `legacy_global_ids` | Optional source-only list of `{name,id}` records; required for legacy sources and forbidden for sources already carrying stable IDs. |
| `components` | Entries containing `id`, `source_fingerprint`, `target_fingerprint`, and the three mapping lists; version 2 also permits `array_capacity`. Unlisted types must have unchanged complete schemas. |

Each mapping list is sorted by ID, unique and disjoint from the others. Every source field is preserved or retired; every target field is preserved or defaulted. Retirements name source-only IDs and defaults name target-only IDs. Component entries and legacy records are sorted by stable ID. The limits are 128 global fields, 32 fields per component and 64 component types.

Hashes are lowercase SHA-256. `schema_sha256` binds the complete schema serialized through the engine's JSON serializer, including layout and persistent/component metadata. `content_sha256` is the owner's frozen world-and-asset identity, not the hash of the world JSON file. Component fingerprints alone do not establish unchanged labels or units; those are checked separately. Do not infer an identity from a version number or filename.

### Authorize a capacity change

Inside a version-2 component entry, authorize each changed **preserved array**:

```json
"array_capacity": [
  {
    "id": "11111111111111111111111111111111",
    "source_capacity": 2,
    "target_capacity": 4,
    "overflow": "reject"
  }
]
```

This is a component-entry fragment. Its field ID and capacities must match the actual source/target declarations. Records are sorted by ID and unique; capacities are integers 1–31 and must differ. `overflow` is exactly `reject`. Missing, redundant or wrong-capacity permissions reject, as do permissions naming scalar, added or retired fields.

Growing preserves length and order. Shrinking succeeds only when every source array being mapped fits, including authored/template values and saved instances; it never truncates entries. Inactive target storage is zeroed. Mapping uses each field's actual layout, preserving fields after an array as well as array contents. All source values are validated before retirement, so an invalid removed field cannot be hidden by the upgrade.

## Loading

Add an `upgrade` selection and explicit non-null `gameplay` selection to an otherwise ordinary guarded `save.load` request:

```json
"upgrade": {
  "path": "upgrades/release-2.json",
  "expected_sha256": "<SHA-256 of the exact plan file bytes>"
}
```

This is a request fragment; retain all [save-load guards](RUNTIME.md#durable-save-slots) and use current observations. `revision` guards the current authored target world. Runtime session/tick/gameplay/component/structure/UI/control guards still protect a live replacement. Pending gameplay saves must be resolved first. Relative plan paths resolve beside the authored world.

The host chooses the plan and executable. Saved bytes never choose paths or grant authority. A plan hash verifies bytes; it is not a signature or authorization policy. The original saved document, asset closure and complete runtime snapshot are validated against the trusted source before transformation. Target code is checked against the exact edge, the mapped snapshot passes the ordinary target restore checks, and the replacement is published only after result and retry data are prepared. Pure document/snapshot mapping helpers alone do not establish native runtime validity.

`Initialize` is skipped. Trusted CoreCLR constructors/static initializers may still run during metadata inspection, and their external effects cannot be rolled back. The same registered CoreCLR instance is consumed by restore. Native AOT process-level image pinning remains in effect; changing a pinned image requires a fresh process.

The result includes an `upgrade` report with plan, content, image and schema digests plus field/instance counts. It does not dump saved values. A retained exact request retry returns its receipt without rereading the plan or activating again, even if the plan file has since been removed. Different parameters under the same retained request ID reject.

Recovery remains separate consent: `allow_recovery:true` permits the store-selected prior generation. The plan must match that selected checkpoint. Results distinguish the manifest generation from `selected_generation`; last-restore metadata records the selected generation.

Successful loading does not overwrite the original slot or current authored document. Explicitly save a new generation or slot when ready to make the upgraded state durable. Snapshot and storage work remain synchronous.

## Task: upgrade a retained hierarchical game

Use this sequence from either a human-operated CLI or an agent. The [retained instance fixture](../tests/fixtures/instance_save_evolution/README.md) supplies concrete build/configuration commands and a verifier for an actual compiled source/target pair. It requires the earlier fixture's retained source world, cooked closure, compiled module and save slot; it does not recreate that historical source.

1. **Retain the source.** Keep the exact world, adjacent cooked asset store, save directory, compiled image/descriptor and schemas together. Verify an ordinary source restore first. Record original file hashes and use working copies for upgrade checks.
2. **Build a separate target.** Keep the frozen native graph and assets. Change only the intended global/component schemas and authored custom values. Generate the target component manifest and keep build/publish outputs separate from the source. Native AOT needs the published library, descriptor and complete inventory.
3. **Inspect identities and live lengths.** Discover `save.load` and its guards, observe the target world/storage, and obtain actual source/target content, image and complete schema hashes. Inspect every array affected by shrink, including template defaults and saved members. Persistent IDs and byte layouts, rather than field names or ordinals, govern preservation.
4. **Select one explicit edge.** Partition all fields into preserve/retire/default lists. Add exact version-2 capacity records for every changed preserved array. Bind legacy global names where required, then hash the complete plan bytes. Do not alter a native graph or asset binding to make the edge pass.
5. **Use a trusted target process.** Native AOT images are process-pinned: launch a fresh process with the target artifact instead of overwriting a loaded image. CoreCLR also requires an explicit matching target module selection. Configure external save storage and resolve pending operations before replacement.
6. **Load with current guards.** Submit the trusted `gameplay` selection and hash-bound `upgrade` fragment above in `save.load`. Use a new request ID and the observed slot generation, target revision and applicable live runtime guards. On an unknown outcome, recover the exact request/receipt before issuing another mutation.
7. **Verify before continuing.** Inspect the upgrade digests/counts, local-to-live maps, array order, trailing scalars, added defaults and immediate native state. Check that `Initialize` was skipped, then advance actual target callbacks and verify continuation. Test a denied edge on a disposable copy and confirm current runtime/world/storage remain unchanged.
8. **Write and reopen deliberately.** Save a new slot or guarded generation after verification. Reopen it in a fresh target owner/process and check continued gameplay. Retain the old slot and source artifacts; successful loading alone does not publish an upgraded checkpoint to storage.

The fixture verifier accepts source/target backend configuration files, a target component manifest and a new output directory. Run it with native Python and paths for the engine's operating system. Its optional genuine compiled shrink profile tests authored-array overflow; that result must not be described as runtime-only overflow. Optional Windows captures compare the source and target at the same saved camera/tick. Neither captures nor a small fixture establish physical input or game-scale performance.

## Failure and resource boundaries

Failed preparation preserves the committed current runtime, authored world and original slot. Trusted CoreCLR constructor/static-initializer effects remain outside rollback. Resolve ambiguous storage publication through its durable receipt; do not infer that a reported I/O error permits a fresh unguarded write.

Existing bounds apply independently to source and target: 16 MiB authored documents, 64 MiB snapshots, 64 component types, 32 fields and at most 512 bytes per component payload. Array capacity is 1–31. A catalog permits 256 recipes, 1,024 members per hierarchical recipe, 4,096 members across all recipes and 2 MiB total template custom payloads. A larger target layout must still fit its own payload budgets. Frozen authored history arrays must already be empty; runtime allocator/structural history is separate preserved checkpoint state.

## Tests

The native `poima-save-upgrade-test`, `poima-save-upgrade-plan-test`, `poima-save-upgrade-document-test` and `poima-save-upgrade-snapshot-test` exercise mapping, plan guards, allowed authored changes and snapshot preservation. They do not establish executable migration by themselves.

`tests/save_evolution_game.py` consumes a separately preserved, verified source baseline and a real compiled target. See its [fixture instructions](../tests/fixtures/save_evolution_game/README.md). `tests/save_evolution_components.py` consumes separately compiled source/target modules and their manifests. These integration runners exercise actual callbacks, explicit saving and fresh-process continuation. Keep source/target build outputs separate and retain the original artifacts when testing historical compatibility.

[Hierarchical save evolution](../tests/instance_save_evolution.py) adds retained version-6 instances, bounded arrays, explicit capacity permissions, source-reference validation before retirement and compiled continuation. Its [instructions](../tests/fixtures/instance_save_evolution/README.md) distinguish backend/process requirements and exact observations. Consult implementation status for completed qualification; a checked-in runner or passing native mapper does not qualify every backend.
