# Retained instantiated-game save evolution

This target consumes an actual saved imported-character game produced by the
earlier [hierarchical instance fixture](../../managed_instance_gameplay/README.md).
It keeps `poima.test.managed-instances` and `Poima.Tests.ManagedInstanceGame` as
the game identity/type. The original source game, compiled artifacts, cooked
model closure and version-6 slot are inputs; this directory does not replace or
rebuild that source game.

The target assigns explicit persistent IDs to all 28 legacy global fields and
renames `Ticks` to `SimulationTicks`. Its `InstanceLink` component retains the
stable IDs for Value, Rig and Members, renames those fields and changes their C#
declaration order. Value's declared default changes from 7 to 999; saved 11/22
must survive. Target is explicitly retired. Bonus defaults to 37 and a new
two-element int buffer defaults empty. An explicit version-2 upgrade plan grows
Members from two to four elements without truncating its saved references.

On its first upgraded Tick, the compiled target checks both real instance maps,
preserved values/references and new defaults. It fills the newly available array
slots, writes generated components and continues native character movement and
independent speed-1/speed-2 rig playback. Later fixture mode inputs retire the
first complete instance and spawn it again, exercising restored ID allocation.
The verifier uses guarded `runtime.gameplay.edit` only to choose a mode; the
compiled Tick performs the actual structural/physics/animation changes.

Build the target with the repository's documented .NET toolchain:

```sh
dotnet build tests/fixtures/instance_save_evolution/target/Poima.ManagedInstanceGame.csproj \
  -c Release -o build/instance-evolution-target
```

Build emits the DLL; it does not write a component manifest. Extract the manifest
from that exact DLL without running game code, using the
[component metadata generator](../../../docs/CUSTOM_COMPONENTS.md#import-and-author):

```sh
dotnet run --project managed/Poima.NativeGame.Generator -c Release -- \
  --components build/instance-evolution-target/Poima.ManagedInstanceGame.dll \
  build/instance-evolution-target/game.poima-components.json
```

Use the resulting `build/instance-evolution-target/game.poima-components.json`
as `--target-manifest`.

For the negative capacity-one target, use a separate output and intermediate
directory so it cannot overwrite the capacity-four artifact:

```sh
dotnet build tests/fixtures/instance_save_evolution/target/Poima.ManagedInstanceGame.csproj \
  -c Release -p:InstanceEvolutionShrink=true \
  --artifacts-path build/instance-evolution-shrink-artifacts \
  -o build/instance-evolution-shrink
```

Extract its separate manifest from the shrink DLL, not the capacity-four output:

```sh
dotnet run --project managed/Poima.NativeGame.Generator -c Release -- \
  --components build/instance-evolution-shrink/Poima.ManagedInstanceGame.dll \
  build/instance-evolution-shrink/game.poima-components.json
```

Use this manifest only with its matching shrink configuration, through
`--overflow-target-manifest` and `--overflow-target-config`.

`InstanceEvolutionShrink=true` adds `INSTANCE_EVOLUTION_SHRINK` to the project's
compile constants. The [Native AOT publisher](../../../scripts/publish_native_gameplay.py)
must receive that same evaluated project property when publishing the shrink
profile; supplying a capacity-one manifest with a capacity-four binary is not a
valid overflow check. Native AOT source and target games run in separate engine
processes because native gameplay images are process-pinned.

To publish the normal Native AOT target on the matching native OS, with the
documented .NET/native linker toolchain available:

```sh
python3 scripts/publish_native_gameplay.py \
  --project tests/fixtures/instance_save_evolution/target/Poima.ManagedInstanceGame.csproj \
  --type Poima.Tests.ManagedInstanceGame \
  --output build/instance-evolution-native \
  --work build/instance-evolution-native-work
```

Both destinations must be new. The artifact includes its matching
`game.poima-components.json`; use that file with its `native-gameplay.json`
descriptor. For a separate shrink publication, set the MSBuild environment
property for the publisher and its child builds. In a POSIX shell:

```sh
InstanceEvolutionShrink=true python3 scripts/publish_native_gameplay.py \
  --project tests/fixtures/instance_save_evolution/target/Poima.ManagedInstanceGame.csproj \
  --type Poima.Tests.ManagedInstanceGame \
  --output build/instance-evolution-shrink-native \
  --work build/instance-evolution-shrink-native-work
```

On Windows, run the publisher with native Windows Python and set
`$env:InstanceEvolutionShrink = 'true'` in PowerShell before the shrink command;
remove that environment variable afterward. The normal publication must not
inherit it. This is the project's evaluated MSBuild property, not a publisher
command-line flag. Confirm the resulting shrink artifact's component metadata
has Members capacity 1; the normal artifact must have capacity 4.

Create target configuration JSON using one backend. Paths resolve relative to
the configuration file:

```json
{
  "assembly": "Poima.ManagedInstanceGame.dll",
  "bridge": "path/to/Poima.ManagedBridge.dll",
  "hostfxr": "path/to/hostfxr",
  "type": "Poima.Tests.ManagedInstanceGame"
}
```

For Native AOT, the configuration is instead:

```json
{"descriptor": "path/to/native-gameplay.json"}
```

Run the verifier with native Python and paths for the engine's operating system:

```sh
python3 tests/instance_save_evolution.py \
  --binary path/to/poima \
  --source-world path/to/retained/world.json \
  --source-saves path/to/retained/saves \
  --source-config path/to/source-config.json \
  --target-config path/to/target-config.json \
  --target-manifest path/to/game.poima-components.json \
  --output build/instance-evolution-check
```

The source world's `.assets` directory must be present beside its world file.
The source slot defaults to `hierarchy`; select another with `--slot`.
`--output` must be new and outside the caller's save/model closures. Nothing is
downloaded, compiled or imported by the verifier. It copies the retained slot and
cooked models, derives the target from the slot's frozen authored document and
uses actual host metadata to authorize the upgrade edge. Caller files are hashed
before/after; successful and failed attempts, RPCs, snapshots and process cleanup
are retained in `evidence.json`.

Supply both `--overflow-target-config` and `--overflow-target-manifest` to test
the genuine compiled capacity-one profile. The retained source recipe itself has
two local references, so this check rejects authored-array overflow before the
snapshot mapper runs. It does not claim a runtime-only overflow test. Native
mapper/snapshot tests cover the other bounded array capacities and element types.

On Windows, optional `--capture --gpu 0` (or 1) compares actual source restore
and target upgrade images at the identical saved tick and camera. It requires a
rendering-enabled runtime and hardware GPU. Each capture records its mesh and
skinned-instance draw counts. The retained fixture's camera 100 is at the origin;
the qualification readbacks draw zero meshes and zero skinned instances. These
images check Vulkan capture and background/UI output preservation. They do not
show the imported characters or establish their visual preservation. Character
state preservation is checked separately through exact saved native transforms,
instance maps, animation state and unchanged cooked geometry. There is no
physical-input, independent FBX decoding or rendering-quality claim.

The verifier positively arms
`POIMA_INSTANCE_UPGRADE_FORBID_INITIALIZE=1`: ordinary target initialization must
fail before the upgraded load succeeds without calling Initialize. It also
checks exact-load rejection, wrong-image/capacity rejection, removed-plan receipt
recovery, invalid references in a field being retired, independent immediate
snapshot mapping, compiled collection edits and same/fresh-owner continuation.
Only `ObservedEpochHigh` and `ObservedEpochLow` are excluded from continued
snapshot comparisons because each restore has a new save epoch. Original slots
and engine-authored geometry remain unchanged.
