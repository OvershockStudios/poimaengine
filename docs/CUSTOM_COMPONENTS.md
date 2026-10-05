# Custom C# gameplay components

Custom components attach typed gameplay data to entities. The native world owns that data; C# reads copies and queues writes through generated accessors. The Inspector and world service edit the same authored fields. This is an initial scalar component API, with fixed membership during Play.

Use the matching engine, `Poima.Gameplay` SDK and managed bridge. The component API requires services ABI **5 (120 bytes)**; rebuild game assemblies and republish Native AOT artifacts. Call ABI remains 1. Existing worlds without custom schemas retain their version 1 format.

## Declare a component

Reference `Poima.Gameplay` and import `managed/Poima.Components.targets` in the game project. The import enables the source generator shipped in this repository; use the pinned SDK described in [Build](BUILD.md). The [component fixture project](../tests/components_gameplay/Poima.ComponentGame.csproj) shows the project setup.

Create a declaration with fresh persistent IDs instead of assigning them by hand:

```sh
python3 scripts/create_gameplay_component.py \
  --name Health --namespace MyGame.Components --output path/to/Health.cs \
  --field Current:float32:100 --field Maximum:int32:100
```

The destination directory must exist; existing files are never overwritten. Repeat `--field Name:kind:default` for `int32`, `int64`, `float32`, `float64` or `entity` (an entity default is `00000000000000000000000000000000`). Omitting `--field` creates `Value:float32:0`. Keep the generated IDs when editing or renaming that declaration; rerunning the tool creates a different type.

```csharp
using Poima;

[GameplayComponent("741d2ce69d5e4cd8a12493c7af70bf1d")]
public partial struct Health
{
    [GameplayField("5c59c8d83a924a1aaa4d321acbc3190b", Default = "100", Unit = "hp")]
    public float Current;
}
```

Generate a fresh stable 128-bit ID for each component type and each field. Keep it when renaming the declaration. IDs are 32 lowercase hexadecimal digits without separators; zero is reserved. Public mutable fields support `int`, `long`, `float`, `double` and `EntityId`. The generator emits the descriptor, wire codec and assembly metadata. No reflection or JSON runs inside these generated accessors.

`Name` on either attribute changes its display label; `Unit` labels a field. Defaults are invariant text in source attributes and typed values in the generated manifest. Entity defaults must be unset. **Attribute defaults initialize authored component data, not a C# `new Health()` or `default(Health)` value.** Read an existing native instance before changing selected fields.

Declarations currently belong to the selected game assembly. Merging manifests from referenced component libraries is not implemented.

One module remains a `Game<TState>`. Its global state is useful for module-wide counters and references; per-entity data belongs in components:

```csharp
public struct GameState { public int Ticks; }

[GameModule("example.health")]
public sealed class HealthGame : Game<GameState>
{
    public override void Tick(ref GameState state, GameContext context)
    {
        ++state.Ticks;
        Span<EntityId> page = stackalloc EntityId[64];
        EntityId after = default;
        int count;
        while ((count = context.Query<Health>(page, after)) != 0)
        {
            for (int i = 0; i < count; ++i)
            {
                var entity = page[i];
                var health = context.Get<Health>(entity);
                health.Current = Math.Max(0, health.Current - 1);
                context.Set(entity, in health);
                after = entity;
            }
        }
    }
}
```

Queries return sorted stable entity IDs after an exclusive cursor. Pages contain 1–256 IDs. `TryGet<T>` returns false for an existing entity without that component; unknown entities are errors. `IsAlive` returns false for unknown or unset IDs. A runtime freezes schemas, entities and component membership at Play; spawning entities or adding/removing live components is not part of this slice.

Reads observe committed data. `Set` queues a complete replacement after the C# callback and before physics. Reads later in that callback still see the prior values. Writing the same entity/type twice in one tick rejects the batch. Aggregate changes in a local struct, then call `Set` once. Component writes participate in runtime rollback, including failure on a later tick of an explicit multi-tick request. Automatic editor playback commits one tick at a time, retaining earlier successful ticks.

## Import and author

Build the game, then extract its manifest without executing game code:

```sh
.cache/toolchains/dotnet-10.0.401/dotnet run \
  --project managed/Poima.NativeGame.Generator -c Release -- \
  --components /absolute/path/MyGame.dll /absolute/path/game.poima-components.json
```

While stopped, use the Inspector's Custom components section to select the manifest, import its schemas, then add a declared component to the selected entity. Edit its fields and Apply. Add, Remove, Apply and schema import use the shared world transaction/history system. A stale draft must be reloaded before applying it.

For agents, `component.schema.import` accepts the manifest inline with `request_id` and `base_revision`. `component.schemas` returns canonical schemas. Entity component keys are `game:<type-id>`; their values are complete objects keyed by field ID. Use ordinary `component.set`/`component.remove` operations inside `world.transact`. Nonzero entity references must resolve in the final candidate world; deleting a referenced entity requires repairing those references in the same transaction.

Import upgrades the authored document to version 2. Schemas may change labels and units while retaining their fingerprint. Removing a schema requires removing all instances first; retired type IDs cannot be assigned to new types. Undo/Redo can restore their own retained history. Component fields and schemas are part of packaged world content.

## Inspect and edit Play state

While paused, select an entity and load its live component into the Inspector. This is a separate draft from authored data; applying it changes the current runtime only. Stop discards runtime changes unless saved through the game's save system. The live Inspector currently uses the authored object selection; inspecting a restored object deleted from the authored world requires the service API.

The service exposes `runtime.components`, `runtime.component.get`, `runtime.component.query` and `runtime.component.edit`. Runtime reads are guarded by session and tick. An edit also requires `request_id` and `expected_revision`, referring to the component revision returned by a read. It is independent of the gameplay module revision. Successful edits retain bounded retry receipts. The desktop rejects manual runtime component edits while playing.

`desktop.inspect.components` exposes only `active`, `session_id`, `tick` and `revision`; polling does not serialize all component values. The editor's live type picker uses the frozen runtime registry, including after restoring a save whose membership differs from current authored content.

## Reload, save and ship

Schema fingerprints cover stable type/field IDs, kinds, canonical defaults and version. Labels, units and C# field order are excluded. Compatible code reload preserves native component storage. Changing a kind, default or field membership rejects replacement; general schema migration is not implemented. The loaded module must declare every instantiated component type, and its declared types must exist in the frozen runtime.

Saves with custom schemas use runtime snapshot version 2 and include component revisions and exact typed payloads. Saves still require their exact supported content/module binding; compatible hot reload does not imply cross-build save migration. External Save/Load requests against an active custom-component runtime require `expected_component_revision` in addition to existing guards. Failed restores preserve the current runtime.

[Native AOT publication](NATIVE_GAMEPLAY.md) includes a hashed `game.poima-components.json` metadata payload. Artifact inspection validates it against the descriptor's declarations. Authoring manifest extraction does not load the game assembly into a runtime. Starting or reloading trusted gameplay can run constructors; these are outside engine rollback.

## Current bounds

A world permits up to 64 component schemas, each with 1–32 scalar fields. Instances use canonical 16-byte cells per field, up to 512 bytes. The runtime permits 32,768 total instances and 16 MiB of payloads. A tick accepts up to 4,096 component writes and 2 MiB of staged data. The shared manifest limit is 512 KiB.

Floating fields must be finite; wire padding is zero and negative zero normalizes to positive zero. JSON represents `long` as a canonical decimal string to retain all 64 bits. Components do not yet support strings, arrays, dictionaries, nested objects, inheritance or arbitrary managed references.

C# queries and generated reads do not allocate payloads. Writes allocate staged payloads and a sparse rollback backup the first time each instance changes in a batch. These limits are validation bounds, not a claim that a maximum-size gameplay workload meets a frame budget.

## Qualification

The [0.0.37 evidence record](evidence/m2-custom-components.json) binds the qualified sources, Linux/Windows binaries and editor package to native, C#, reload, save, Inspector and relocated compiled-game checks. It also records resource bounds, isolated storage measurements and unqualified deployment/UI cases.
