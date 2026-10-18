# Installed C# authoring tools

An authoring pack supplies the compiled gameplay SDK, component analyzer,
build-time native binding generator and NativeAOT publisher. An optional CoreCLR
bridge supports installed development and compatible reload. Games reference
these installed resources without building Poima's internal managed projects.
Keep the pack separate from the native runtime distribution and exported games.

Use Python 3.10+ and a matching .NET 10 SDK. CoreCLR development also needs
the native host and an explicitly selected hostfxr from its installed .NET
runtime. NativeAOT publishing additionally needs an x64 native linker toolchain
and a host matching its target OS; `win-x64` builds run
on Windows. The compiler and .NET framework/runtime packages remain explicit
development prerequisites. The pack builder does not install them or download
packages. The optional bridge does not include a .NET runtime or SDK.

Pack compilation and native publication set
`DOTNET_CLI_WORKLOAD_UPDATE_NOTIFY_DISABLE=true` in their child environments.
This suppresses the SDK's opportunistic workload-update advertising during
these commands. The caller's environment and machine-wide settings are
unchanged; project restore and build requirements still apply.
[SDK setting](https://learn.microsoft.com/en-us/dotnet/core/tools/dotnet-environment-variables#dotnet_cli_workload_update_notify_disable).

## Build and inspect a pack

From a source checkout, select the installed .NET executable and new output and
work directories:

```sh
python scripts/package_authoring.py --dotnet /absolute/path/to/dotnet \
  --output dist/authoring --work build/authoring-work
```

The builder compiles the generator and its SDK dependency, then the component
analyzer. Each build has its own intermediate directory and cleared NuGet
sources. Missing installed SDK/framework inputs fail explicitly. Only the fixed
resources enter the pack; additional compiler outputs need an explicit
packaging policy. Build work remains available for inspection.

To include CoreCLR development resources, add `--coreclr`:

```sh
python scripts/package_authoring.py --dotnet /absolute/path/to/dotnet \
  --coreclr --output dist/authoring --work build/authoring-work
```

The default remains format v1. `--coreclr` selects format v2, builds the bridge
and SDK first, and builds the generator against that exact binary SDK. It also
builds the same component analyzer. No game project needs a reference to the
engine's internal managed projects.

Copy the complete pack to its installed location. Validate it there:

```sh
python /installed/authoring/tools/authoring_pack.py /installed/authoring
```

The result identifies the pack version, manifest SHA-256 and payload count.
`authoring.json` inventories thirteen payloads with paths, roles, sizes and
hashes. Format v2 inventories the same thirteen plus
`bridge/Poima.ManagedBridge.dll`, its `.deps.json` and `.runtimeconfig.json`,
and `bridge/Poima.Gameplay.dll`: seventeen payloads in total. Admission checks
the complete directory contents, every SDK copy and all payload identities.
The manifest is limited to 64 KiB, individual
payloads to 16 MiB and total payload data to 64 MiB. Links, reparse points,
hardlinked files, path aliases and extra files are rejected. Keep build outputs
and caches outside the pack.

## Create a game project

Use separate sibling directories for the installed pack, game source, products
and compiler work. Import the installed props file from an ordinary project:

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <Import Project="../authoring/build/Poima.Gameplay.props" />
  <PropertyGroup><AssemblyName>Example.Game</AssemblyName></PropertyGroup>
</Project>
```

The props select .NET 10/C# 14, the binary SDK and prebuilt component analyzer,
with nullable checks, implicit usings, unsafe code, deterministic compilation
and warnings as errors. There is no repository-relative `ProjectReference`.
Use the supported [C# gameplay API](MANAGED_GAMEPLAY.md) and
[component declarations](CUSTOM_COMPONENTS.md) in your game sources.

A minimal native game type has a public sequential unmanaged state and a public
stateless game class:

```csharp
using System.Runtime.InteropServices;
using Poima;
namespace Example;

[StructLayout(LayoutKind.Sequential)]
public struct State { public int Ticks; }

[GameModule("example.game")]
public sealed class Game : Poima.Game<State>
{
    public override void Initialize(ref State state) => state=default;
    public override void Tick(ref State state, GameContext context) => ++state.Ticks;
}
```

Publish from an independent working directory using the installed publisher:

```sh
python /installed/authoring/tools/publish_native_gameplay.py \
  --authoring-pack /installed/authoring --project /projects/Example/Game.csproj \
  --type Example.Game --dotnet /absolute/path/to/dotnet --rid win-x64 \
  --output /products/example-1 --work /compiler-work/example-1
```

Output and work must be new directories outside both the pack and game-source
directory. Without `--work`, the publisher uses temporary owned build work and
cleans it afterward. The game's original project and dependencies still execute
as trusted MSBuild code. Generated intermediates use the selected work tree.

The built game must copy the exact installed `Poima.Gameplay.dll`. Publication
checks that identity before invoking the prebuilt generator, then compiles the
generated bindings and inventoried `Entry.cs` into a native shared library. It
retains required engine and resolved .NET runtime notices. Unexpected runtime
dependencies fail rather than being silently dropped.

The resulting `native-gameplay.json` and its declared payloads are consumed by
[guarded project adoption](PROJECT_GAMEPLAY_ADOPTION.md). A game with custom
components also contains `game.poima-components.json`; import its schemas before
authoring those components. Inspect generated schema/defaults and actual runtime
behavior before exporting.

## Compile and reload with CoreCLR

Use a format-v2 pack and a native host built with managed gameplay enabled.
Compile the game project with the installed .NET SDK, using the same props
import above. Keep compiler intermediates and outputs outside the installed
pack. If intermediates must also stay outside the authored game, set
`BaseIntermediateOutputPath` and `MSBuildProjectExtensionsPath` before the SDK
props import; the [installed workflow](../tests/coreclr_authoring_workflow.py)
shows that project layout.

The existing `development.compile` operation runs the selected SDK and exposes
terminal status and structured source diagnostics. An accepted job is not a
successful compilation. Inspect `development.inspect` and
`development.diagnostics`, and retain failed attempts before repairing source.
See [compiler jobs](DEVELOPMENT_JOBS.md) for request fields, trust and bounds.

Start the runtime and call `runtime.gameplay.load` with the inspected session,
Tick, gameplay revision and structure revision, the selected `hostfxr`,
`bridge/Poima.ManagedBridge.dll`, the compiled game assembly and its game type.
The compiled game's copied `Poima.Gameplay.dll` must match the pack's SDK. Keep
the complete inventoried bridge directory together. The pack validator can
check its integrity before each consumption; the explicit CoreCLR loader paths
are not implicitly pinned by a NativeAOT build profile.

After a compatible successful build, reload explicitly using fresh native
guards. Compatible state migrates through the supported typed schema; building
a DLL alone does not replace the running game. A failed compilation should
leave the last usable assembly available. Check live state and the next changed
gameplay Tick separately. See [managed gameplay](MANAGED_GAMEPLAY.md) and
[iteration](AGENT_ITERATION.md) for ownership, unsupported changes and recovery.

The bridge is for development. NativeAOT publication continues to use the
pack's inventoried native generator, publisher and Entry source; a format-v2
pack supports that route too. Keep development runtime prerequisites separate
from the exported game's declared native dependencies.

## Use a native build profile

Add the optional host-selected `authoring_pack` directory to a
[development profile](DEVELOPMENT_PROFILES.md). Its `publisher` must select that
pack's `tools/publish_native_gameplay.py`. Existing profile fields and explicit
toolchain/environment policy still apply.

The native host validates the pack at configuration and includes its manifest
and resource inventory in the profile fingerprint. It passes the selected
manifest SHA-256 to publication automatically. A changed queued pack therefore
fails before compilation, and zero-exit publication revalidates the complete
pack before reporting success. Exact historical retries return their retained
job rather than rebuilding or probing removed tools.

Standalone callers can also pass `--authoring-sha256` with a retained manifest
hash. Without it, the publisher admits the current pack at startup and rechecks
that identity at consumption and publication boundaries. These are integrity
checks, not a sandbox or a lease against concurrent writers.

## Diagnose failures

- **Manifest/resource mismatch:** replace the complete pack with the intended
  verified copy. Editing its files invalidates its recorded identity.
- **SDK mismatch:** inspect the game's references and copied SDK. Remove a
  conflicting SDK reference rather than repointing the manifest at other bytes.
- **Missing framework or runtime package:** supply the selected installed SDK
  and package cache through the host's explicit toolchain environment.
- **Occupied output/work:** inspect the earlier attempt and choose fresh
  directories. Publication never overwrites an existing artifact.
- **Failed publication:** retain compiler diagnostics and work. An incomplete
  destination with `.poima-publishing` is not an inspectable completed artifact.

The pack's source checkpoint and a gameplay descriptor's diagnostic engine
version do not prove native compatibility. Adoption checks the actual host ABI
and required features. Export separately requires an exactly matching native
runtime distribution; the authoring pack does not replace that runtime.

## Qualification

The 0.0.162 Windows check builds the actual SDK/analyzer/generator pack, copies
it independently and publishes an original C# component game through the native
build profile. The engine checkout pathname is unavailable during compilation,
adoption, native callbacks, saves and relocated export consumption. The same
checkout directory is restored after the supervising Job closes.

The corrected fresh attempt passes 894 native calls and eighteen complete
checkpoints: lossless component/global values, successful and rolled-back logical
Control callbacks, same-owner restoration and fresh-process continuation in both
the source project and relocated game. Its external supervisor observes 32
processes and finishes with an empty Job without forcing cleanup. The
first attempt's missing fixture callback remains a separate failed record.
[Results, artifact identities and limits](evidence/m2-installed-authoring-tools.json).

The [NativeAOT workflow reproducer](../tests/authoring_tools_workflow.py) needs
Python 3.11+ and takes matching
`--runtime`, `--pack`, `--dotnet`, `--source` and a new `--output`; run it inside
an owned Windows Job using the existing
[process supervisor](../tests/switchback_ownership.py). `--assert-absent` checks
the supplied original path at workflow boundaries. Its caller owns preparing
the offline SDK/linker environment and safely excluding/restoring any checkout;
the workflow alone does not make that path unavailable.

The 0.0.164 [CoreCLR reproducer](../tests/coreclr_authoring_workflow.py) needs
Python 3.11+ and a matching Windows runtime with simulation, managed gameplay
and game UI enabled. Its compiled fixture exercises generated component access,
logical controls and rollback as well as code reload. Run it inside the same
owned Windows Job pattern, with explicit `--runtime`, `--pack`, `--dotnet`,
`--hostfxr`, `--source` and a new `--output`. Its `--assert-absent` has the same
caller-owned exclusion requirement.

The recorded CoreCLR run passes 697 native calls and seventeen complete
version-7 checkpoints while the checkout pathname is unavailable. It compiles
valid source, retains structured CS1525 diagnostics for an intentional failure,
repairs and reloads compatible code, preserves complete saved state except the
two declared reload identity fields, then verifies changed behavior and full
warm/fresh-process save continuation. The external supervisor observes seventeen
processes and finishes with an empty Job without forcing cleanup;
the same checkout directory is restored.

A separate 0.0.164 run uses the same v2 pack for NativeAOT publication,
adoption, export and relocated readonly gameplay. It passes 865 calls and
eighteen complete checkpoints through five owners. The external supervisors
observe thirty-two processes and finish with empty Jobs without forcing cleanup.
The test-created source project is unavailable during bundled
consumption and is restored exactly. The engine checkout stays available in
this separate run; its source-absence evidence belongs to the CoreCLR run and
the earlier v1 NativeAOT check.

Those closure observations describe the external supervisors. Ordinary native
development jobs retain their separate policy of terminating remaining owned
children after the leader finishes. The historical records do not establish
natural exit for every nested child. New
[opt-in process observations](DEVELOPMENT_JOBS.md#process-and-cleanup-observations)
separate actual inner cleanup requests from outer-owner closure without changing
the original workflow verdicts.

These are existing-machine installed authoring checks. Clean-machine
installation, graphics, latency budgets and the broader Alpha gates remain
separate.
[Results and artifact identities](evidence/m2-installed-coreclr-authoring.json).
