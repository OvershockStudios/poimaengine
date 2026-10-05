# Native C# gameplay artifacts

Poima's Native AOT route compiles one C# `Game<TState>` into a native shared library. It uses the same engine-owned state and service ABI as [development-time CoreCLR gameplay](MANAGED_GAMEPLAY.md). The development bridge remains the reloadable path; a native game library stays loaded for the lifetime of its player process.

The 0.0.35 integration has recorded Linux and Windows native execution, CoreCLR/native state comparisons and a relocated Windows Vulkan game replay. These are bounded fixture results; clean-machine deployment, consoles and production workloads remain unqualified. See the [checkpoint evidence](evidence/m2-native-gameplay.json) and [implementation status](IMPLEMENTATION_STATUS.md).

The [gameplay save extension](GAMEPLAY_SAVES.md) requires rebuilding with services ABI 4. Older compiled artifacts must be republished.

## Publish

Publishing needs a matching x64 operating system, a .NET 10 SDK and its supported native linker toolchain. On Windows this requires Visual Studio 2022 or later C++ build tools and a compatible Windows SDK; the recorded build used MSVC 14.44 and Windows SDK 10.0.26100.0. The engine can still be cross-built separately with LLVM-MinGW. See Microsoft's [Native AOT prerequisites](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/) and [native shared-library limitations](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/libraries). The Python script is build tooling; the engine does not invoke it when playing a game.

```sh
python3 scripts/publish_native_gameplay.py \
  --project examples/managed/DoorGame/Poima.DoorGame.csproj \
  --type Poima.Examples.DoorGame \
  --dotnet /path/to/dotnet \
  --output build/door-native
```

The output directory must be new. `--rid linux-x64` or `--rid win-x64` selects a matching host target; cross-OS Native AOT publication is not supported. `--work` retains build intermediates in a separate new directory. Project builds execute trusted build tasks and dependencies.

Build-time inspection generates a direct typed factory and state-layout assertions. The native entry calls the selected game and state types directly. It does not discover or load managed game assemblies at runtime. The generator currently requires public, non-generic top-level game and state types, a public parameterless game constructor, a stateless game class and supported unmanaged state fields. Nested types are not supported. Arbitrary reflection, dynamic code, managed plugin loading and dependency publication are not supplied by this route. Unexpected runtime publish payloads reject until an explicit packaging policy exists.

Native AOT includes runtime services, including garbage collection. It is native compiled gameplay, not a guarantee of zero allocation or absence of a runtime. The module reports its actual dynamic-code support and compilation flags; the loader requires both to be false.

## Artifact contents

A dedicated artifact directory contains `native-gameplay.json`, one library, and the included dependency notices. Intermediate IL assemblies, debug symbols, generated source and build logs belong outside that directory.

The descriptor declares the exact engine version, Linux/Windows x86_64 target, call ABI 1, services ABI 4, fixed `poima_gameplay_entry` export, game identity/type and complete state schema. Its payload inventory records relative paths, byte counts, SHA-256 hashes and roles (`library`, `dependency`, `notice`).

Read-only inspection validates the schema, file inventory and native image headers without executing code. It accepts foreign-target metadata for export workflows. Actual loading additionally requires the running engine's platform, generated schema and native diagnostics to agree. Hashes check integrity against the supplied inventory; they are not signatures or an authenticity guarantee.

The artifact inventory is bounded to 256 files, 256 MiB per payload, 1 GiB total and a 1 MiB descriptor. The state schema allows at most 128 supported fields and 64 KiB of state. Portable path rules reject traversal, symlink/reparse content, case collisions and unlisted files. Native image-header checks do not prove that a library can execute or that all system dependencies are available.

## Load through the world service

After `runtime.start`, submit:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "runtime.gameplay.load_native",
  "params": {
    "session_id": "<runtime session ID>",
    "request_id": "<fresh request ID>",
    "expected_tick": 0,
    "expected_revision": 0,
    "descriptor": "gameplay/native-gameplay.json",
    "values": {}
  }
}
```

The descriptor resolves relative to the world document. `values` is optional and defaults to an empty object. An optional `expected_descriptor_sha256` binds loading to previously inspected descriptor bytes. Game launch passes that guard automatically. Invalid supplied values are rejected before mapping the native module.

The existing runtime receipt pool, exact retry matching, tick/revision guards and gameplay inspection/edit operations apply. `runtime.gameplay.inspect` with `include_schema: true` reports `backend: "native_aot"`, the library hash, state schema, values and native diagnostics. Simulation uses the same callback queue timing, native animation authority and whole-batch rollback as CoreCLR gameplay.

Only one canonical native library path and image are selected per process. The loader never unloads it, including after export or generated-schema validation fails. Replacing a loaded native game is rejected. Stopping a world releases its game instance; starting a new world can create another instance from the same pinned artifact. Selecting a different artifact requires restarting the process. This is separate from development-time compatible CoreCLR reload.

## Package a game

Project version 1 remains the format for projects without gameplay. Version 2 requires a `gameplay` object:

```json
"gameplay": {
  "descriptor": "gameplay/native-gameplay.json",
  "values": { "UseDistance": 3.5 }
}
```

Other project fields retain their existing contracts. Inspection validates initial values against the artifact schema. Export requires a matching target runtime with `features.native_gameplay: true`; older runtime descriptors lacking this feature are treated as false.

Native gameplay export and bundle inspection also require `runtime.json` to declare `gameplay_services_version: 4`. Matching engine version alone is insufficient: an older services ABI cannot execute the new module. Legacy runtime descriptors without this field remain usable for bundles without gameplay.

Export copies the descriptor and exact payload closure into the bundle's `gameplay/` directory and emits game manifest version 2. Game launch verifies the bundle, starts its read-only runtime, loads the compiled module at tick zero, applies supplied values, then enters the player. Its result includes full gameplay inspection. Source projects, development IL and hostfxr are not part of this artifact route.

A native-only engine configuration uses `POIMA_ENABLE_SIMULATION=ON`, `POIMA_ENABLE_NATIVE_GAMEPLAY=ON` and `POIMA_ENABLE_MANAGED_GAMEPLAY=OFF`; hostfxr headers are unnecessary. The native option defaults to the simulation option when first configuring a build. A graphical game bundle still requires the renderer and its platform dependencies. Path-based inspection and subsequent OS loading do not constitute a filesystem-race-free execution boundary.
