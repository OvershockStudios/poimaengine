# Native module iteration experiment

This M0 experiment verifies a native extension boundary and provides a comparison for the preferred C# gameplay path. It is not the production SDK, world service or complete hot-reload implementation. C# CoreCLR and shipping AOT need separate integration; **do not load a .NET Native AOT library here**, because this host unloads libraries.

## Build and run

The normal CMake presets build `poima-module-lab` (disable with `POIMA_BUILD_MODULE_LAB=OFF`). It has no graphics dependencies. The standalone fixture only includes the experimental public C header and does not rebuild Poima's renderer or engine libraries:

```sh
cmake -S experiments/native_module -B build/game-fixture -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/game-fixture --parallel 2
./build/headless/poima-module-lab
```

The running host accepts one command per line on stdin and flushes one JSON response per command. Paths are the entire text after the first space; do not add shell quoting inside this protocol. Windows paths use UTF-8 input and Windows filesystem semantics. The default is 1,000 entities; `--entities N` accepts 1–100,000.

| Command | Meaning |
| --- | --- |
| `load PATH` | Copy a native library to a unique private filename, validate ABI, initialize/migrate staged data, then replace the active module. |
| `step N` | Execute 1–10,000 integer simulation ticks. A failed update discards the whole batch. |
| `inspect` | Inspect tick, revision, schema, canonical state hash, first/last entity, native-call count, process resident memory and retained shadow-file count. |
| `save PATH` | Write a canonical checkpoint through a sibling temporary file and rename/replace it. |
| `restore PATH` | Verify format, length, checksum and identities; migrate into staging before committing. |
| `quit` | Respond, unload the module, remove its private temporary directory and exit. EOF also exits. |

Error responses preserve active state for rejected loads, migrations, updates and checkpoint restoration. Diagnostic native-call counts can increase during a rejected update; they are not game state. A native crash remains a process crash. The host does not sandbox trusted project code.

## State and lifetime rules

The C ABI uses fixed-width integers and explicit function tables. The host owns aligned storage; modules borrow it for each call and never retain pointers, callbacks or work. Layout 1 occupies 32 bytes per entity; layout 2 adds an energy field and occupies 40. Migration uses canonical records, not compiler-specific C++ objects. Schema 1→2 supplies energy 100; schema 2→1 is deliberately rejected. Replacing a behavior with the same schema preserves state exactly.

The host is single-threaded and reloads only between commands/ticks. It makes a uniquely named copy of each candidate DLL/shared library so the build artifact remains writable on Windows. It retires the previous module only after successful staging, and removes rejected copies. This does not yet demonstrate retirement of jobs, audio/GPU resources or live callbacks.

Checkpoints encode fixed-width little-endian integers and include a corruption checksum. Identical logical state produces identical bytes across the fixture's Windows/Linux hosts. This is a lab format, not the eventual engine save schema; rename replacement is not a guarantee of recovery from power loss or native crashes.

## Reproducible checks and measurements

```sh
python3 scripts/measure_native_module.py --host build/headless/poima-module-lab --output build/module-evidence-linux
python3 scripts/measure_native_module.py --host build/windows-render/poima-module-lab.exe --windows-interop --output build/module-evidence-windows
```

The Windows command cross-compiles with the pinned LLVM-MinGW toolchain and executes the host on Windows through WSL interoperability. Its state/checkpoints and build sources are on WSL storage; shadow DLLs are on Windows temporary storage. Results include that storage/interop cost and are not a Windows-hosted compiler benchmark.

Each run copies fixture sources into a fresh directory, records configuration and first build separately, makes 40 real source edits, and keeps stepping the old module while the compiler runs. It checks the new behavior after publication. It also verifies compile failure, ABI rejection, migration rejection after partial staging, failed updates, corrupted checkpoint rejection, byte-identical save/restore/save, schema upgrade, rejected downgrade and 100 reloads with live entities. Ordinary Linux CTest uses two edits plus the same correctness/reload checks; the explicit 40-edit run supplies p95 evidence.

`build_ms` includes compilation/linking and the polling interval while servicing old-game ticks. `reload_ms` includes shadow copying, OS loading, query/validation, snapshots, migration, publication and retirement; it is not migration CPU time alone. `observed_total_ms` additionally includes source editing, artifact copying/path conversion, protocol overhead and the first checked update. It ends at correct logical state, not a rendered frame. Resident working-set samples and shadow counts aid leak inspection but do not prove absence of all retained resources.

The fixture has a small source file and 1,000 simple integer entities. Its timings cannot qualify the playable-room targets, whole-engine compile speed, C# performance or console support. See [recorded comparison baseline](evidence/m0-native-module.json) and [implementation status](IMPLEMENTATION_STATUS.md).
