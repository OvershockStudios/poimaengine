# Compiled component fixture

`Poima.Tests.ComponentGame` uses generated `Health` and `Interaction` codecs against the native component store. It has no managed entity store or per-tick reflection. The manifest contains all five supported field kinds and stable IDs; the deliberately different C# declaration order tests field-ID wire ordering.

Build with the repository's pinned .NET SDK:

```sh
dotnet build tests/components_gameplay/Poima.ComponentGame.csproj -c Release -o build/components-fixtures/base
dotnet build managed/Poima.NativeGame.Generator -c Release -o build/components-extractor
dotnet build/components-extractor/Poima.NativeGame.Generator.dll --components build/components-fixtures/base/Poima.ComponentGame.dll build/components-fixtures/base/game.poima-components.json
```

`--components` reads PE metadata without loading the assembly. The game project explicitly imports `managed/Poima.Components.targets`; that source generator uses the compiler assemblies supplied by the build SDK, with no NuGet analyzer download. `GameplayField.Default` is the native authoring default, not a change to C# struct zero-initialization.

Build variants using `-p:ComponentVariant=COMPONENT_REORDER`, `COMPONENT_LABELS` or `COMPONENT_INCOMPATIBLE`, into separate output directories. The first changes CLR field order, the second display labels/units, and the third changes a semantic default. Only the third should be incompatible with the original native schema.

The `Mode` global selects these Tick behaviors:

| Mode | Behavior |
| --- | --- |
| 0 | Query and read, without writes |
| 1 | Decrement health and increment score; assert reads still see the committed value inside Tick |
| 2 | Queue a duplicate write to the same instance |
| 3 | Throw after queuing a write |
| 4 | Throw on odd ticks, after queuing a write (use an even starting tick to test whole-batch rollback) |
| 5 | Attempt to encode NaN |
| 6 | Queue an unresolved entity reference |
| 7 | Read an unknown entity |
| 8 | Queue a C# save to `components-game`, then observe its ticket |
| 9 | Queue a C# load from that slot, then observe its ticket |
| 10 | Assign an unresolved global entity reference after queuing a component write |
| 11 | Assign that unresolved global reference on odd ticks, testing whole-batch rollback |

`Selected` defaults to the first entity in the sorted Health query. The fixture queries in pages of two and also checks liveness and an optional Interaction component. `tests/components_gameplay_contract.py` drives the real engine and durable save APIs; `tests/managed_service_abi` independently checks callback wire transfer; `tests/component_generator_contract.py` checks diagnostics and metadata-only extraction.

The native lifecycle harness uses this same compiled module for writes, later-tick exceptions and save intents while the host schedules structural edits:

```sh
cmake --build --preset runtime-headless --target poima-runtime-lifecycle-gameplay-test
./build/runtime-headless/poima-runtime-lifecycle-gameplay-test HOSTFXR BRIDGE COMPONENT_GAME_ASSEMBLY COMPONENT_MANIFEST
./build/runtime-headless/poima-runtime-lifecycle-gameplay-test --native COMPONENT_GAME_DESCRIPTOR COMPONENT_MANIFEST
```

The uppercase arguments are paths to matching built artifacts. These checks cover atomic component writes/removal, next-tick query visibility, full-batch rollback, save-ticket continuity and v3 restoration. They do not expose a C# spawn API. [Qualification evidence](../../docs/evidence/m2-runtime-lifecycle-batch.json).
