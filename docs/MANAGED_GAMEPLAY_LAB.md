# C# gameplay development experiment

The engine now has an [initial integrated C# gameplay API](MANAGED_GAMEPLAY.md). This document preserves the separate lab and its original measurements.


This M0 fixture runs C# game code inside a native C++ process on Windows. It evaluates the accepted C# gameplay direction without replacing the engine core or claiming a production gameplay SDK. The standalone lab is separate from the main `poima` CLI and renderer.

## What runs where

The C++ launcher starts the selected CoreCLR runtime through Microsoft's `hostfxr` API and invokes a stable C# bridge. A separate C++ kernel owns the entity buffers and supplies a counted native arithmetic service. The bridge controls this fixture's ticks, migration transactions, checkpoints and commands; the production world model is still to be built.

Each game DLL gets a collectible `AssemblyLoadContext`. The game contract is shared from the stable bridge's assembly context, so old game types do not become persistent world types. Assemblies load from bytes and do not lock the compiler's output file. Game methods borrow spans into native buffers; no work, callbacks or pointers may outlive those calls in this fixture.

The example deliberately exposes low-level spans and native services to test the boundary. The eventual user-facing C# SDK should hide this plumbing behind components, entities and ordinary gameplay operations. This is not UnityEngine compatibility and does not execute Unity C# assets unchanged.

## Build on the tested WSL/Windows setup

Requirements: CMake/Ninja, the workspace LLVM-MinGW compiler, and an existing Windows .NET 10 SDK/runtime. This experiment does not install or upgrade the user's .NET installation. The measured setup uses SDK **10.0.204**, host headers from pack **10.0.8**, and runtime/hostfxr **10.0.11**. They are recorded inputs, not a claim that all .NET versions are qualified.

From WSL at the repository root:

```sh
cmake -S experiments/managed_gameplay -B build/managed-native-windows -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/windows-llvm-mingw.cmake" \
  '-DPOIMA_DOTNET_HOST_HEADERS=/mnt/c/Program Files/dotnet/packs/Microsoft.NETCore.App.Host.win-x64/10.0.8/runtimes/win-x64/native'
cmake --build build/managed-native-windows --parallel 2
```

From Windows PowerShell in the repository (a WSL UNC path is supported by the tested SDK):

```powershell
dotnet build experiments/managed_gameplay/Bridge/Poima.ManagedLab.csproj -c Release
dotnet build experiments/managed_gameplay/Game/Poima.Lab.Game.csproj -c Release
```

Build outputs under `bin`/`obj` are ignored. No NuGet package dependency is declared; the SDK supplies the framework references. The native kernel does not depend on .NET headers. Only the launcher includes the SDK's MIT-licensed hosting declarations, and it loads the existing runtime dynamically. The lab is not part of engine installation or runtime redistribution yet.

The native executable accepts three positional paths: `hostfxr`, the bridge DLL, and the native kernel library. It finds the bridge runtime configuration beside the DLL. On Windows:

```powershell
.\build\managed-native-windows\poima-managed-lab.exe `
  'C:\Program Files\dotnet\host\fxr\10.0.11\hostfxr.dll' `
  '.\experiments\managed_gameplay\Bridge\bin\Release\net10.0\Poima.ManagedLab.dll' `
  '.\build\managed-native-windows\libpoima_managed_kernel.dll'
```

## Interactive protocol

The process reads one command per line and flushes one compact JSON response per command. Paths occupy the whole remainder of the line after one space; do not add quotation marks inside the protocol. Input/output use UTF-8. This is a lab protocol, not the future project/session JSON-RPC service.

| Command | Effect |
| --- | --- |
| `load PATH` | Load a C# game DLL, check its ABI/layout and initialize or migrate staged native storage. Publish only after successful preparation. |
| `step N` | Run 1–10,000 ticks on staging storage, then commit the batch. A managed exception discards staged writes. |
| `inspect` | State hash, identities, first/last entity, schema, native allocation count, retired-context count, memory and last hot-loop measurements. |
| `save PATH` / `restore PATH` | Canonical checkpoint compatible with the native fixture, including validation and staged schema migration. |
| `collect` | Explicit diagnostic GC/unload check. Reports contexts still rooted after bounded collection attempts. |
| `release-probe` | Clear the deliberately retained reference used by the lifetime-diagnostic test. Test-only operation. |
| `quit` | Dispose native state, retire the active assembly, collect for diagnostics and report the resulting counts. EOF also disposes state. |

The `collect` command forces collection only to test whether an assembly can be unloaded. It is not a proposed per-edit or per-frame scheduling policy. CoreCLR collection remains cooperative. Arbitrary user-created threads, event subscriptions or native crashes are outside this experiment's lifetime contract. Post-publication runtime failures during retirement do not promise rollback to an old assembly; the staged build/ABI/migration/update failures are the qualified rollback paths.

## Measurement and acceptance checks

From WSL:

```sh
python3 scripts/measure_managed_gameplay.py \
  --launcher build/managed-native-windows/poima-managed-lab.exe \
  --bridge experiments/managed_gameplay/Bridge/bin/Release/net10.0/Poima.ManagedLab.dll \
  --kernel build/managed-native-windows/libpoima_managed_kernel.dll \
  --hostfxr '/mnt/c/Program Files/dotnet/host/fxr/10.0.11/hostfxr.dll' \
  --dotnet '/mnt/c/Program Files/dotnet/dotnet.exe' \
  --windows-interop --output build/managed-evidence-windows --warm-edits 40
```

Use `--warm-edits 2` for a shorter correctness run. It still exercises 100 reloads and the failure cases. The harness copies source into a fresh build directory, changes source text for each edit, compiles while the previous game keeps ticking, loads the new DLL and verifies changed behavior with preserved state. It checks canonical C++/C# state equality, failed builds/ABI checks/migrations/updates, corrupt saves, a 32→40-byte layout change, rejected downgrade and old-checkpoint migration.

One variant deliberately retains a game instance in the stable contract assembly. The harness requires the unload diagnostic to detect that context, then verifies it disappears after releasing the reference. This distinguishes actual lifetime checking from an unconditional success counter. Every ordinary reload must leave no retired context alive after the explicit diagnostic collection. Native live bytes must equal the active layout's storage and reach zero on shutdown.

Timings distinguish build, reload, first correct logical observation and diagnostic collection. Build timing includes driving the old game and polling the compiler. The end-to-end observation includes source editing, DLL copying, Windows path conversion and protocol overhead; it is not a rendered-frame measurement. Each edit's forced diagnostic collection is recorded separately. The isolated hot-loop allocation sample excludes staging allocation, snapshots and JSON I/O, and must not be described as a zero-allocation engine or game.

Source and outputs live on WSL storage in the measured setup while the SDK and runtime execute on Windows. Record that cross-filesystem cost. The native callback includes an atomic counter, unlike the C++ baseline's non-atomic counter, so the hot-loop values are not a controlled C++ versus C# speed comparison.

See [recorded C# evidence](evidence/m0-managed-gameplay.json). This fixture qualifies development behavior only. The separate [Linux shipping experiment](MANAGED_SHIPPING_LAB.md) now compares CoreCLR and Native AOT execution with the same game code. The later [runtime integration](MANAGED_GAMEPLAY.md) qualifies a small Linux collectible-reload fixture. Windows AOT, console backends, the full C# SDK and production job/callback/resource retirement remain separate work. Native AOT shared libraries must not be unloaded through the native DLL reload lab.
