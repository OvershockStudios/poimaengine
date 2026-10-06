# Compiled lifecycle fixture

This small trusted test game drives services ABI 6 through the real C# SDK. It uses a generated `LifecycleLink` component (`111…111`), a linked kinematic template (`444…444`), and a plain dynamic template (`333…333`). The native harness constructs the world and frozen recipes; the C# game requests births, initialized mutual references, motion, cancellation, removal, and a save intent.

Build the fixture and extract declarations without executing it:

```sh
dotnet build tests/managed_lifecycle_gameplay/Poima.ManagedLifecycleGame.csproj -c Release -o build/managed-lifecycle/game
dotnet build managed/Poima.NativeGame.Generator -c Release -o build/managed-lifecycle/generator
dotnet build/managed-lifecycle/generator/Poima.NativeGame.Generator.dll --components build/managed-lifecycle/game/Poima.ManagedLifecycleGame.dll build/managed-lifecycle/game.poima-components.json
cmake --build build/runtime-headless --target poima-runtime-managed-lifecycle-test -j 1
build/runtime-headless/poima-runtime-managed-lifecycle-test /path/to/libhostfxr.so /path/to/Poima.ManagedBridge.dll build/managed-lifecycle/game/Poima.ManagedLifecycleGame.dll build/managed-lifecycle/game.poima-components.json
```

Publish the same source with a matching-host .NET SDK/native linker and test the resulting NativeAOT library:

```sh
python3 scripts/publish_native_gameplay.py --project tests/managed_lifecycle_gameplay/Poima.ManagedLifecycleGame.csproj --type Poima.Tests.ManagedLifecycleGame --output build/managed-lifecycle-native --work build/managed-lifecycle-native-work
build/runtime-headless/poima-runtime-managed-lifecycle-test --native build/managed-lifecycle-native/native-gameplay.json build/managed-lifecycle-native/game.poima-components.json
```

On Windows, use the `build/windows-runtime/poima-runtime-managed-lifecycle-test.exe` target and native Windows paths, including `hostfxr.dll` for CoreCLR. Publication requires a matching Windows SDK/linker. These commands are qualification instructions, not a claim that either platform has already passed.

The five native groups assert publication and later read visibility; reference repair and snapshot continuation; combined host/gameplay ordering; caught invalid spawn and cancel-only cursor behavior; whole-batch rollback after a later managed exception, including native motion and save intent; and the combined 4096-call command limit with canceled births counted. Existing component and lifecycle fixtures remain separate.

`tests/managed_lifecycle_contract.py` exercises the same compiled fixture through JSON-RPC: birth revisions and step retries, stale-edit rejection, compatible CoreCLR reload, gameplay reference repair/removal, durable save restoration and continuation without authored changes. Pass `--manifest`, the CoreCLR `--hostfxr`/`--bridge`/`--assembly` paths (or `--native-descriptor`), and an `--output` directory; use `--windows-interop` when launching the native Windows engine from WSL.
