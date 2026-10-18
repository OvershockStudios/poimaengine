# Managed retirement observer bounds

This .NET 10 test calls the real compiled managed bridge through an independent
80-byte `Entry.Invoke` declaration. It reuses the `MemoryProbe` assembly from
`tests/managed_memory_snapshot/Fixture`, whose complete state is twelve bytes.
It does not build or replace the gameplay SDK.

Build the helper against the selected bridge directory, which must also contain
its matching `Poima.Gameplay.dll` dependency:

```sh
dotnet build tests/managed_retirement_bounds/ManagedRetirementBounds.csproj \
  -c Release --disable-build-servers \
  -p:UseSharedCompilation=false -p:BridgeDirectory=/absolute/selected/bridge
```

Run each bridge selection in a **fresh process**, with a separate fresh output
directory whose parent exists. `legacy` selects the expected observer refusal
of the retained bridge; `candidate` expects the expanded observer range. These
modes change expectations, not the sequence of real loads and releases.

```sh
dotnet tests/managed_retirement_bounds/bin/Release/net10.0/ManagedRetirementBounds.dll \
  /absolute/MemoryProbe.dll legacy /absolute/new-legacy-output
```

Use `candidate` and another output directory for the corrected bridge build.
Select each bridge through the helper build/dependency directory; a mode argument
does not dynamically replace the bridge already loaded in that process.

## Actual lifetime sequence

The fixture loads thirty-one modules, including one opted-in observer. It then
performs 1,023 accepted temporary loads and releases: each temporary module fills
the thirty-second slot before release. One final admitted peer leaves thirty-two
active modules and 1,023 retired entries. Admission of a thirty-third module must
still refuse without creating a context.

Releasing thirty-one peers reaches 1,054 retired entries while the observer
remains live. Complete raw probes at 1,024, 1,025 and 1,054 are each repeated
without changing the registry or the initialized twelve-byte state. The legacy
bridge must return its original bounds error above 1,024; the candidate must
return the exact active, tracked and alive counts. Real Initialize and Tick
callbacks retain and verify the complete twelve-byte gameplay state. Tick uses a
stack-owned epoch-7, 176-byte service prefix with twenty typed Cdecl callback
slots. These callbacks report failure if unexpectedly invoked; the state-only
fixture must invoke none. This is a valid call boundary, not simulated world
services.

The final observer release produces 1,055 retired entries with **no live module**.
Its expired handle must refuse a snapshot. Thus a native parser test accepting a
synthetic 1,055-entry DTO is separate from the actual live query at 1,054; this
fixture does not claim a successful live query at 1,055.

Every entry comes from a real accepted collectible context. A no-inline test
frame holds those contexts strongly through the decisive probes, using read-only
registry reflection to identify them. No registry seeding, synthetic weak
references, forced GC or bridge operation 5 establishes these conditions.
Ordinary GC can occur during allocations; the controlled roots are a fixture,
not evidence that a normal game leaks contexts.

After that frame returns or unwinds, explicitly labeled operation 5 performs
active teardown. It is outside passive observation and cannot turn an earlier
failed probe into a successful one. Its raw retained-alive count may be nonzero;
forcing that count to zero is not an acceptance condition for the observer-bound
proof. The original 1,024-context load-capacity fixture remains a separate test.

## Retained output and bounds

A single stdout JSON receipt reports `poima.managed-retirement-bounds.v1` with
mode, identities, outcomes, counts, stages and errors. Stderr retains `IDENTITIES`
and every literal `RAW_CALL`, including request text, original returned text,
return code, termination status and complete state bytes before/after callbacks.
Successful observations are written as their original UTF-8 JSON text, including
`retired-1054.json` and its repeat for native parser replay. A refused observation
does not acquire a fabricated JSON artifact.

The helper has a ninety-second deadline, an 8 MiB raw-call stream bound and a
64 KiB terminal-receipt bound. An outer owner must enforce process closure,
time and stream limits independently; helper output alone does not prove them.
This test qualifies controlled managed lifecycle behavior, not native world
hosting, production memory use, frame time or a general unloadability guarantee.
