# C# native shipping experiment

This M0 experiment compiles the same C# `Game` used by the development lab into a Linux x64 Native AOT executable. The C++ service library still owns state buffers and exposes the counted native arithmetic call. There is no game-assembly loading or replacement in this executable: game code is linked during publishing and runs for the process lifetime. The experiment is separate from the engine CLI and renderer.

The user-facing gameplay SDK, Windows AOT and console backends remain unimplemented/unqualified. This fixture proves a narrow C# shipping path, not a complete engine distribution. AOT retains .NET runtime services, including garbage collection. [Microsoft Native AOT documentation](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/)

## Reproduce

On Linux x64 with CMake, Ninja, C++20, Clang and zlib development files available:

```sh
python3 scripts/bootstrap_tools.py --only dotnet
python3 scripts/verify_managed_shipping.py
```

The bootstrap verifies and extracts SDK **10.0.401** in `.cache/toolchains`. The verifier uses runtime **10.0.12**, restores packages from nuget.org into `.cache/nuget/packages`, and keeps its CLI home in `.cache/dotnet-user`. It builds fresh copies of the source under `build/managed-shipping-evidence` and changes only the copied game source for the second layout. Build logs, commands, source/binary hashes and outcomes go to `results.json`. Initial restoration requires network access. Nothing installs into the main engine package or changes the system SDK.

The AOT publish directories contain an ELF executable, the C++ service library, debug symbols and notices. They contain no managed DLLs and execute directly with `DOTNET_ROOT` pointing to an absent directory. Native system libraries are still required; building on Kali does not establish an older Linux distribution compatibility floor. The executable's `RuntimeFeature` diagnostics report dynamic code support/compilation as false. The matching CoreCLR build reports true.

The app accepts four positional arguments:

```text
Poima.ShippingLab KERNEL INPUT_OR_DASH OUTPUT TICKS
```

`-` initializes a fresh world; otherwise input is a canonical `PMLAB001` checkpoint. `TICKS` is 0–10,000. It validates/migrates, updates, releases native storage, atomically replaces the output checkpoint and emits a JSON result. Errors emit JSON and exit 4. This is a bounded batch fixture, not a production save-game format or interactive engine protocol. File contents are flushed before replacement; filesystem-wide power-loss durability is not qualified.

## Verified evidence

[Recorded Linux shipping evidence](evidence/m0-managed-shipping.json) covers:

- Both 32-byte and 40-byte entity layouts, all 1,000 entities checked against an independently encoded Python oracle.
- Checkpoint identity with the previously recorded C++ Windows/Linux baseline, which the Windows CoreCLR lab also matched.
- CoreCLR-to-AOT and AOT-to-CoreCLR save/resume; schema upgrade and rejected downgrade.
- Corrupt checksum, invalid length/identity/schema, tick overflow, excessive tick count and missing output directory.
- Existing output retained after failed input/migration; zero native state bytes after successful completion.
- Twenty fresh processes per runtime, each running 1,000 ticks and producing the exact expected checkpoint.
- AOT publishing with compiler/trimmer warnings treated as errors; native format/dependency inspection and runtime JIT diagnostics.

The two AOT executables each occupy **1,872,096 bytes**, excluding the service library, symbols and notices. Process launch through update/save/exit measured p95 **85.6 ms** for AOT and **309.1 ms** for CoreCLR across twenty runs each. These include filesystem I/O and, for CoreCLR, cold-process JIT work; they are not steady-state game performance or isolated startup measurements. The observed update bodies allocated zero managed bytes; initialization, snapshots and JSON still allocate.

Fresh schema-1 AOT publishing took **70.7 s** and schema-2 **40.2 s**, including their build/restore work with different cache histories. These are individual observations, not a warm-build distribution. Development remains on the separately tested reload path rather than requiring AOT publication per edit.

These measurements used a Windows-mounted filesystem through WSL, SDK 10.0.401/runtime 10.0.12 and Clang 21.1.8 for the AOT native link. They do not update the earlier Windows development-reload timing qualification, which used another SDK and filesystem arrangement.

Windows toolchain/package execution, an older-distribution Linux compatibility floor, representative gameplay dependencies and allocation/latency workloads, generated SDK coverage, and console runtime integration remain unqualified. Linux collectible reload/native hosting also remains separate from this directly launched CoreCLR fixture. Native AOT libraries must never be unloaded through the development reload loop. [Native library limitations](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/libraries)

The separate lab above retains its original scope and evidence. As of 0.0.35, the actual engine has a distinct [Native AOT gameplay and bundle integration](NATIVE_GAMEPLAY.md), with its own [execution and deployment evidence](evidence/m2-native-gameplay.json).
