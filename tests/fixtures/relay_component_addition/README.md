# Defaulted component on a retained Relay Yard save

This small target recipe adds `RelayUpgradeProgress` to the permanent player
100 and courier 300 in a retained CoreCLR Relay Yard checkpoint. It consumes
the original saved document, cooked closure, compiled source image and slot;
this directory does not include or regenerate that historical source cohort.
The [v3 upgrade contract](../../../docs/SAVE_UPGRADES.md#add-a-defaulted-component-type)
authorizes one new type and these two existing authored, saved-live IDs.

The selected source checkpoint has snapshot version 5, tick 140, 50 gameplay
globals, 83 live entities, three custom types and one collected cell. Its
original DLL and saved bytes remain inputs. The target retains its global
layout, existing component schemas and 3 m interaction policy. Do not replace
that source with a newly built current sample or use its later edited world
file instead of the checkpoint's frozen document.

Prepare a separate target directory from the retained project's four sources:
`Iteration.csproj`, `RelayYardGame.cs`, `RelayAudio.cs` and
`LocomotionYardGame.cs`. Add the adjacent
[RelayUpgradeProgress.cs](RelayUpgradeProgress.cs). Keep the retained project's
matching SDK and component-generator references; the actual target assembly is
`Poima.RelayYardGame.dll`. The original source project is never rebuilt.

In the target's running `Tick` path, insert exactly one call after its two
audio cadence updates and before UI publication:

```csharp
RelayAudio.Steps(context,Player);
RelayAudio.Steps(context,Actor);
RelayUpgradeProgress.Observe(context,in state);
Publish(context,in state);
```

The component declares three stable int32 fields with zero defaults. Typed
Query/Get/Set requires exactly the permanent player and courier; Count advances
once per running callback, while Collected and Won reflect the real objectives.
Do not add this component to pickup templates, spawned instances or rig nodes.

Use a trusted native `development.compile` request with a new request ID,
explicit .NET executable, the target project, separate output, `Release`,
`restore:true` and a finite timeout. Inspect its actual terminal status and
diagnostics; recover an exact retry receipt instead of submitting another job
after an unknown outcome. See [development jobs](../../../docs/DEVELOPMENT_JOBS.md).
Do not publish a Native AOT descriptor for this CoreCLR fixture.

Build output alone does not supply the component manifest. Extract it from the
exact compiled DLL using the matching, already built metadata generator:

```sh
dotnet path/to/Poima.NativeGame.Generator.dll --components \
  path/to/compiled/managed/Poima.RelayYardGame.dll \
  path/to/target/game.poima-components.json
```

The extracted manifest must contain exactly the original three types plus
`c1120000000000000000000000000001`, with Count, Collected and Won field IDs
1, 2 and 3, kind `int32`, default 0. Obtain the native canonical fingerprint
through `component.schemas`; do not guess it from a filename or schema version.
Keep compiler inputs, managed dependencies, request/status/diagnostics and
manifest hashes with the new target output.

The [verifier](../../relay_yard_component_addition.py) takes the retained cohort
directory and separately compiled target inputs. Run with native Windows
Python and paths matching the engine OS, under a bounded owner of the complete
process tree:

```sh
python tests/relay_yard_component_addition.py \
  --binary path/to/poima.exe \
  --baseline path/to/retained-cohort \
  --target-assembly path/to/compiled/managed/Poima.RelayYardGame.dll \
  --target-manifest path/to/target/game.poima-components.json \
  --hostfxr path/to/hostfxr.dll --bridge path/to/Poima.ManagedBridge.dll \
  --output path/to/new-verification-output
```

The baseline directory supplies `owner.json` and its retained `run` source,
image, assets and saves. The verifier checks the exact selected generation,
ordinary source restoration, rejected target loads, an explicitly bound v3
plan, separate addition reports and full immediate native payload preservation.
It then uses real remaining pickups and courier completion, saves a new target
slot, and exact-restores it in a fresh engine process before more callbacks.
Source files, original image and slot are checked unchanged.

Target compilation, native mapping tests and retained-game continuation are
separate qualifications. This recipe makes no Native AOT, graphics, physical
input, latency or general component-migration claim. Refer to current
[implementation status](../../../docs/IMPLEMENTATION_STATUS.md) for completed
evidence.
