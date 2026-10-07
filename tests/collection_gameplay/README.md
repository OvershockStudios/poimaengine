# Compiled collection gameplay fixture

Acceptance test for compiled collection gameplay; qualification evidence identifies the backends actually run. Build against matching SDK/generator and extract its actual component manifest. No rendering required. Inventory combines capacity4 entity/int buffers plus a scalar. Parallel quantities deliberately cover two buffer kinds, not a recommended general inventory architecture.

The world has one authored owner (ID1) with empty Inventory. First real Tick spawns four plain item roots through template201, retaining their returned handles in gameplay state. No removable test item is an authored entity. Mode1 fills owner inventory with these actual handles and quantities10..13; fifth append reports capacity failure without changing either array. Queued writes must remain invisible to reads until commit.

Modes2/3/4/7/10 exercise dangling handle, post-write exception, invalid index, and destruction without repairing the first/last collection references. Modes7/10 clear their corresponding global references in the attempted transaction so a component-reference failure cannot be confused with the global field validator. The runner requires the component-reference diagnostic for2/7/10. Failed ticks and a later-tick batch failure (Mode9) must preserve complete world-save payload bytes, compared through separate probe slots.

Template200 has empty Items and nonempty Quantities=[2,5]. Mode5 validates those real generated GetTemplate values, spawns a bag, and queues its complete Inventory with the actual first/last spawned item handles. The published bag therefore has matching arrays and valid references, while the immutable recipe contains no runtime-only handles. Mode6 removes first/last handles and aligned quantities from every inventory, clears matching global fields and despawns those roots atomically. Remaining owner order is middle handles/quantities11,12; bag arrays are empty.

Durable proof saves nonempty inventory + spawned bag, exits and exact-loads the same artifact in a new process. An environment trap is explicitly proven by a failing ordinary Initialize before stopped exact restore; restored gameplay must advance both native and managed ticks. After repair, save generation2, preserve generation1 payload hash, and restore/continue in another process. Template/world authored bytes must stay unchanged.

Run with native-host Python and a fresh output directory:
`python tests/collection_gameplay_contract.py --binary ENGINE --config GAME_CONFIG.json --manifest ACTUAL_COMPONENT_MANIFEST.json --output NEW_DIRECTORY`.
Config selects explicit CoreCLR hostfxr/bridge/assembly/type or NativeAOT descriptor paths, resolved relative to the config. Evidence records artifact/source hashes, owned RPC traces, failures and cleanup; pass is set only after clean child exits. No capability claim before actual qualification.

## Build and run

Build this project, `managed/Poima.ManagedBridge`, and `managed/Poima.NativeGame.Generator` with the matching pinned SDK, using isolated artifacts:

```sh
dotnet build tests/collection_gameplay/Poima.CollectionGameplay.csproj -c Release --artifacts-path build/collection-check -m:1
dotnet build managed/Poima.ManagedBridge/Poima.ManagedBridge.csproj -c Release --artifacts-path build/collection-check -m:1
dotnet build managed/Poima.NativeGame.Generator/Poima.NativeGame.Generator.csproj -c Release --artifacts-path build/collection-check -m:1
dotnet build/collection-check/bin/Poima.NativeGame.Generator/release/Poima.NativeGame.Generator.dll --components build/collection-check/bin/Poima.CollectionGameplay/release/Poima.CollectionGameplay.dll build/collection-check/components.json
```

Create a configuration JSON with `hostfxr`, `bridge`, `assembly` and `type:"Poima.Tests.CollectionGameplay"`. Select the current platform's hostfxr library and the built bridge/game assemblies. Paths resolve relative to the configuration file unless absolute. Then run Python native to the engine's OS:

```sh
python tests/collection_gameplay_contract.py --binary path/to/poima --config build/collection-check/config.json --manifest build/collection-check/components.json --output build/collection-check/result
```

The output directory must be new. The runner retains RPCs, source/artifact hashes, original saves and terminal results. Native AOT can instead select a separately published artifact through `{"descriptor":"path/to/native-gameplay.json"}`; that option is not evidence that the backend was tested. Linux CoreCLR is currently qualified in [the recorded development evidence](../../docs/evidence/m2-component-collections.json).
