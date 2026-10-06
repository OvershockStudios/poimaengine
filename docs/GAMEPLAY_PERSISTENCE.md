# Persistent gameplay field metadata

This development API assigns stable identities and literal defaults to C# global state fields. **The annotation alone does not enable cross-version loading:** ordinary restores still require the saved backend, image, type and complete schema to match the trusted game. Changed saves require an [explicit host-selected upgrade](SAVE_UPGRADES.md).

## Declare state

```csharp
using Poima;

[GameplayPersistence(1)]
public struct GameState
{
    [GameplayField("00000000000000000000000000000001", Default = "0")]
    public int Collected;

    [GameplayField("00000000000000000000000000000002", Default = "3")]
    public int Goal;
}
```

The state must still satisfy the normal `Game<TState>` layout and scalar-field rules. Opting in requires a `GameplayField` identity on every state field. IDs are nonzero, lowercase, 32-character hexadecimal strings and must be unique within the state. Keep an ID when renaming or reordering its field; use a new ID for a different piece of persistent data.

The annotation's revision is an author-controlled integer from 1 through 2,147,483,647. It is separate from engine versions, service ABI epochs and the metadata format version. Increasing it does not authorize an upgrade by itself.

`Default` is a constant string describing the field's typed value. Omission means typed zero. Supported kinds are `int`, `long`, `float`, `double` and `EntityId`; floating defaults must be finite, and an entity default must be the null handle. The emitted schema uses an integer for `int`, a lossless decimal string for `long`, numeric floating values and 32 zero hex digits for a null entity. Floating zero is normalized to positive zero.

These are persistence defaults, not initialization code. They do not assign ordinary game state or replace `Initialize`. A retained saved value must not be overwritten merely because its declared default changes.

For this metadata format, `GameplayField.Name` may be omitted, null, or equal to the actual C# field name; an alternate name is rejected. Nonempty `Unit` is also rejected. This avoids silently losing metadata that the format cannot represent. Component authoring retains its own field-label and unit contract.

## Schema and compatibility

The existing layout schema receives an optional object:

```json
"persistent": {
  "format": "poima.gameplay-persistence",
  "version": 1,
  "revision": 1,
  "fields": [
    {"id": "00000000000000000000000000000001", "name": "Collected", "kind": "int32", "default": 0},
    {"id": "00000000000000000000000000000002", "name": "Goal", "kind": "int32", "default": 3}
  ]
}
```

Entries are ordered by stable ID and must account for every layout field exactly once with matching name and kind. CoreCLR and Native AOT generation share the metadata encoder. Reading annotations does not invoke attribute constructors or gameplay `Initialize`.

Without `GameplayPersistence`, the emitted schema remains unchanged. Existing field annotations alone do not opt a state into persistence metadata.

A Native AOT artifact carrying this object declares `gameplay_persistence_v1` in addition to `baseline_v7`. Runtime selection, export and bundle inspection check that requirement. This feature describes metadata support, not save-migration support. Service epoch 7, its 176-byte baseline and call ABI 1/80 bytes remain unchanged. An older runtime must reject an unsupported artifact rather than ignore its metadata.

Native field mapping and strict upgrade-plan parsing are connected to the external world-service load path in development. Automatic packaged-game upgrade policy remains unfinished. Development reload continues to use its documented compatible-field rules; stable persistence IDs are not a new reload migration mechanism.

## Native upgrade primitives

`poima::save_upgrades::map_global_scalars` validates both gameplay schemas and complete source values, then maps explicitly preserved, retired and defaulted field IDs. Legacy sources require an explicit, complete old-name-to-ID mapping. Retained fields keep their values, including when names or declared defaults change. Scalar kinds cannot change.

`map_component_scalars` applies the same policy to one existing component instance. It decodes compact values in the source schema's canonical field order and produces the target payload plus both compact and field-ID-keyed JSON. Component type IDs, retained kinds and retained units must match. Instance membership and live entity references require separate runtime validation.

`parse_plan` accepts bounded `poima.save-upgrade` version 1 JSON and an expected SHA-256 of its exact bytes. The plan binds source and target world/content, backend, module identity/type, executable image and complete schema hashes. Source and target world, backend, module identity and type must match. Operations and component entries use sorted, unique stable IDs; duplicate JSON keys, unknown members, overlapping operations and excessive resource counts are rejected. `require_edge` compares all bound identities with independently obtained identities. A matching hash proves byte identity, not permission to load code.

These mapping APIs do not open files, execute gameplay, modify slots or activate a runtime. The owner validates original source data, approved authored changes, actual source/target schemas and code, and the complete transformed target snapshot before activation. Component fingerprint equality alone is insufficient to establish unchanged authoring semantics because names and units are outside that fingerprint. Ordinary `save.load` remains exact; the optional explicit upgrade path and its qualification limits are documented in [Save upgrades](SAVE_UPGRADES.md).

## Qualification

[Native primitive results](evidence/m2-save-upgrade-primitives.json) cover Windows and Linux field mapping (86 rejection cases per platform) and plan parsing (60 rejection cases per platform). Run `poima-save-upgrade-test` and `poima-save-upgrade-plan-test` for those focused checks. They do not exercise whole-world migration.

[Recorded results](evidence/m2-gameplay-persistence-metadata.json) separate schema validation, compiled metadata, old-binary compatibility and package checks. `poima-gameplay-metadata-test` runs pure schema tests with no arguments; `--native DESCRIPTOR` runs the published compiled fixture. The CoreCLR form takes `HOSTFXR BRIDGE ASSEMBLY TYPE`. `PublishedPersistentGame` additionally checks exact snapshots and subsequent ticks; the trap fixtures check initialization separation.

The managed harness in `tests/gameplay_persistence` takes `DOTNET GENERATOR_DLL OUTPUT_DIRECTORY` and compares real bridge/generator output. Build it against a matching bridge/SDK using its `BridgeDirectory` property. The separate `tests/gameplay_persistence_fixture` project can be published with the ordinary native publisher and type `PublishedPersistentGame`. These are small qualification fixtures, not a complete save-upgrade workflow.
