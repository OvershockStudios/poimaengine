# Managed reload tracking capacity

This focused executable calls the actual compiled bridge through an independently
declared 80-byte call. Run it in a fresh supervised process using either the
retained legacy bridge or the candidate bridge. Both processes use the existing
[MemoryProbe fixture](../managed_memory_snapshot/Fixture/MemoryProbe.cs).

Build the selected bridge and fixture first, then build this test against that
exact bridge directory:

```sh
dotnet build tests/managed_reload_capacity/ManagedReloadCapacity.csproj -c Release -m:2 \
  -p:BridgeDirectory=ABSOLUTE_SELECTED_BRIDGE_DIRECTORY
dotnet tests/managed_reload_capacity/bin/Release/net10.0/ManagedReloadCapacity.dll \
  ABSOLUTE_MEMORY_PROBE_DLL candidate
```

Use `legacy` instead of `candidate` for the retained bridge that rejects a load
when 1,024 retired entries are already dead. Rebuild into separate owned output
directories when selecting different bridges; do not mix their process-global
registries. Serialize compilation and execution with other qualification work.
The test has a 90-second internal call deadline; an external owner must bound
the whole process and retain its stdout, stderr and natural or forced closure.

The test performs 1,024 real accepted loads and releases. A no-inline test frame
holds each distinct collectible context strongly, verifies the live-retirement
limit, then returns. Bounded explicit **test-host** GC creates a dead-entry
precondition without invoking bridge operation 5. Repeated passive snapshots
must keep all 1,024 entries. The legacy load rejects; the candidate load reclaims
the dead bookkeeping, preserves the fixture schema and executes Initialize with
Witness=17. The candidate then verifies 32 accepted active modules and rejection
of the 33rd. This later scenario cannot alter the earlier retirement precondition.

No synthetic weak entries or reflected list mutations seed capacity. Reflection
only reads module/context identities and counters to establish actual ownership.
Compiled IL checks cover Load, generated Load lambdas and passive snapshot methods
for direct forced-GC/strong-target calls; they do not inspect arbitrary guest code
or every transitive framework implementation. Active teardown is separately
labeled. Every original call/result is retained on stderr before assertions, and
the final JSON records loaded assembly identities and stage observations.

These checks do not establish game performance, native hosting, ordinary-GC
unloading timing or a global retirement bound across arbitrary multi-module
release patterns. The existing passive-memory test remains a separate regression.
