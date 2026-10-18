# Passive managed memory snapshot checks

This executable calls the real `Poima.ManagedBridge.Entry.Invoke` with an
independently declared 80-byte unmanaged call. Build the current bridge first,
then build the fixture and test against that exact bridge directory:

```sh
dotnet build managed/Poima.ManagedBridge/Poima.ManagedBridge.csproj -c Release -m:2
dotnet build tests/managed_memory_snapshot/Fixture/MemoryProbe.csproj -c Release -m:2
dotnet run --project tests/managed_memory_snapshot/ManagedMemorySnapshot.csproj -c Release -- \
  tests/managed_memory_snapshot/Fixture/bin/Release/net10.0/Poima.MemoryProbe.dll
```

Both test projects accept `-p:BridgeDirectory=ABSOLUTE_CURRENT_BRIDGE_DIRECTORY`.
The test executable accepts the actual fixture DLL as its sole argument, so
separate owned output folders can be used. These commands compile managed code;
serialize them with other native/compiler qualification on the development PC.

The fixture type is `Poima.Qualification.MemoryProbe`, module identity
`poima.qualification.memory-snapshot`, with 12-byte sequential native-backed
`int32` fields `Ticks`, `Mode` and `Witness`. Initialize sets zero state and
Witness=17; Tick allocates a bounded 8192-byte witness, increments Ticks and sets
Witness=23. Mode=1 throws after those mutations for independent native rollback
checks. `Poima.Qualification.ThrowingInitializeMemoryProbe` has the same schema
and identity but throws during Initialize. Neither type needs entities or UI.

Bridge operation 8 requires a live handle whose load requested the exact JSON
integer `memory_snapshot_version: 1`; the manifest echoes it only when requested.
The returned values are process-global. Queries sample weak `IsAlive` without
pruning, resetting, unloading or explicitly collecting. Formatting, serialization
and runtime observation can allocate and cause normal GC. The stopwatch bracket
ends before formatting/serialization; observations are not atomic. Recorded GC
fields describe historical completed GC data. Index zero means no completed GC
history: generation, flags and the recorded heap, fragmentation, committed and
memory-load bytes are zero. It does not identify a generation-zero collection.
The threshold and available bytes under `last_gc` are computed independently
of completed GC history and can already be nonzero when index is zero.
Completed background and foreground GC index observations need not be monotonic.
Negative/invalid raw observations reject rather than inventing unavailable values.

Checks cover strict malformed/duplicate negotiation, legacy manifest parity,
handle gating, closed JSON types/bounds, buffer bounds, actual load/Initialize/
release cycles and failed-load retirement. A test-only strong reference holds
one real collectible context alive; operation 5 is then used explicitly to
distinguish that controlled retention from a context that can be collected once
the test reference ends. Those active-collection assertions are separate from
passive snapshots. Compiled helper IL is inspected for direct active-collection,
unload or weak Target calls; unchanged collection counters alone do not prove
that a sample requested no collection. No native hosting, window, per-game heap
measurement or memory-growth diagnosis follows from this executable.
Every bounded raw snapshot is written to stderr before semantic assertions, so
failed checks retain the actual observation.
