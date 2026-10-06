# Compiled component save upgrades

These two distinct games retain the same module identity and type. The target renames a stable field, retires another, adds a literal default, and changes declared defaults on retained fields. Source gameplay first changes both authored and template-spawned instances, so restoring authored defaults cannot accidentally satisfy the test. The target reads and writes migrated values through generated component accessors.

Build each project into a separate output tree using the repository's pinned .NET SDK:

```sh
dotnet build tests/fixtures/save_evolution_components/source/Poima.SaveEvolutionGame.csproj -c Release --artifacts-path build/save-evolution/source -m:1
dotnet build tests/fixtures/save_evolution_components/target/Poima.SaveEvolutionGame.csproj -c Release --artifacts-path build/save-evolution/target -m:1
dotnet build managed/Poima.ManagedBridge/Poima.ManagedBridge.csproj -c Release --artifacts-path build/save-evolution/tools -m:1
dotnet build managed/Poima.NativeGame.Generator/Poima.NativeGame.Generator.csproj -c Release --artifacts-path build/save-evolution/tools -m:1
```

Extract manifests from the actual compiled assemblies; do not substitute hand-written schemas:

```sh
dotnet build/save-evolution/tools/bin/Poima.NativeGame.Generator/release/Poima.NativeGame.Generator.dll --components build/save-evolution/source/bin/Poima.SaveEvolutionGame/release/Poima.SaveEvolutionGame.dll build/save-evolution/source-components.json
dotnet build/save-evolution/tools/bin/Poima.NativeGame.Generator/release/Poima.NativeGame.Generator.dll --components build/save-evolution/target/bin/Poima.SaveEvolutionGame/release/Poima.SaveEvolutionGame.dll build/save-evolution/target-components.json
```

Create source and target configuration JSON files with `hostfxr`, `bridge`, `assembly` and `type`. Select the host platform's `hostfxr` library, the built bridge, the corresponding source/target assembly and type `Poima.Tests.SaveEvolutionGame`. Paths may be absolute or relative to the configuration file. For Native AOT, publish each project separately using `scripts/publish_native_gameplay.py` and use `{ "descriptor": "path/to/native-gameplay.json" }` instead. Native AOT source and target run in separate engine processes.

Run with Python native to the engine's OS:

```sh
python tests/save_evolution_components.py --binary path/to/poima --source-config build/save-evolution/source-config.json --target-config build/save-evolution/target-config.json --source-manifest build/save-evolution/source-components.json --target-manifest build/save-evolution/target-components.json --output build/save-evolution/result
```

The output directory must not already exist. The runner creates real source/target worlds, stores a source checkpoint, proves the initialization trap works, checks exact-load rejection and explicit upgrade/retry, runs target callbacks, then saves and resumes in a fresh process. It records artifact/source hashes and RPC evidence. A passing run qualifies only that host, backend and fixture; it is not a benchmark or a promise of arbitrary migrations.
