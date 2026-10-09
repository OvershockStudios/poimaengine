# Compiled hierarchical actors

This fixture builds two reusable animated actors from a frozen runtime template.
Compiled C# reserves each hierarchy, resolves its local nodes, initializes typed
links, starts independent animation clocks and supplies normal capsule movement.
The actors are created by gameplay; they are not pre-spawned scene objects.

This workflow was qualified with Poima **0.0.78**. Use a matching build with
simulation, compiled gameplay, game UI and FBX intake. Configure those optional
features using the [build guide](../../docs/BUILD.md#optional-fixed-step-runtime)
and [managed host setup](../../docs/MANAGED_GAMEPLAY.md#build); CoreCLR is disabled
in an untouched runtime preset. The module requires services epoch 7, the 232-byte hierarchy prefix and
the `character_input_v1`, `hierarchical_instances_v1` and
`component_collections_v1` features. CoreCLR development additionally needs the
matching managed bridge and .NET 10 runtime. See [managed gameplay](../../docs/MANAGED_GAMEPLAY.md),
[custom components](../../docs/CUSTOM_COMPONENTS.md) and
[native publication](../../docs/NATIVE_GAMEPLAY.md).

## Prepare original content

Supply the unchanged Kenney **Animated Characters: Protagonists 1.1** content
from its [official asset page](https://kenney.nl/assets/animated-characters-protagonists).
The supplied directory must contain:

```text
Model/characterMedium.fbx
Animations/run.fbx
Animations/idle.fbx
License.txt
```

These assets use CC0-1.0. The verifier checks the exact original-file fingerprints
listed in the [Locomotion Yard intake](../../examples/locomotion-yard/run.py).
It does not download, rewrite or delete caller files. Each run copies the inputs
into its own new output directory, then removes only those owned source copies
before testing fresh-owner save continuation.

The body and separate Run/Idle files use explicit
[reference-rotation retargeting](../../docs/ANIMATION_RETARGETING.md), with original
Targeting Pose references and selected HipsCtrl/Hips positional deltas. Original
units, bind matrices and geometry are retained. This is code-driven animation
and capsule movement; it does not supply stride calibration, foot locking, IK,
root-motion extraction or repaired loop derivatives.

## Build the development module and manifest

Use SDK **10.0.401**. On Linux, the repository bootstrap can install the pinned
SDK using `python3 scripts/bootstrap_tools.py --only dotnet`.

```sh
.cache/toolchains/dotnet-10.0.401/dotnet build \
  managed/Poima.ManagedBridge/Poima.ManagedBridge.csproj \
  -c Release -m:1 -o build/instances-example/bridge
.cache/toolchains/dotnet-10.0.401/dotnet build \
  tests/managed_instance_gameplay/Poima.ManagedInstanceGame.csproj \
  -c Release -m:1 -o build/instances-example/game
.cache/toolchains/dotnet-10.0.401/dotnet build \
  managed/Poima.NativeGame.Generator/Poima.NativeGame.Generator.csproj \
  -c Release -m:1 -o build/instances-example/generator
.cache/toolchains/dotnet-10.0.401/dotnet \
  build/instances-example/generator/Poima.NativeGame.Generator.dll \
  --components build/instances-example/game/Poima.ManagedInstanceGame.dll \
  build/instances-example/game.poima-components.json
```

Manifest extraction reads generated assembly metadata without executing the game.
The `InstanceLink` component has a scalar value, an external target, a resolved
rig handle and a bounded two-entity member buffer. Its schema is version 2.
Build the bridge and game from the same SDK sources. Explicit output directories
avoid assumptions about platform-dependent MSBuild `bin/` paths.

Run the actual compiled CoreCLR contract:

```sh
python3 tests/managed_instances_contract.py \
  --binary build/runtime-headless/poima \
  --assembly build/instances-example/game/Poima.ManagedInstanceGame.dll \
  --bridge build/instances-example/bridge/Poima.ManagedBridge.dll \
  --hostfxr .cache/toolchains/dotnet-10.0.401/host/fxr/10.0.12/libhostfxr.so \
  --manifest build/instances-example/game.poima-components.json \
  --source-directory /path/to/kenney-protagonists-1.1 \
  --output build/instances-example/coreclr-check
```

For native Windows builds, use the installed SDK 10.0.401, its matching
`host/fxr/10.0.12/hostfxr.dll`, and explicit bridge/game/generator output paths.
Use native Windows Python with Windows paths, or add `--windows-interop` when
the verifier runs in WSL against a Windows executable.

The contract checks real birth callbacks, reserved-read rejection, canceled
births, failed-batch rollback, repaired whole-instance deletion, independent
controller movement and animation clocks, and complete instance maps. It checks
exact version-6 saves and same/fresh-owner continuation. CoreCLR reload must
preserve compatible state; the NativeAOT route must reject in-process library
replacement without altering state.

The pose oracle samples normalized **original** FBX references and motion through
native intake, then uses separate quaternion-chain, forward-kinematics and
weighted-original-geometry math. It is not an independent FBX decoder or an
oracle derived from converted animation output. Compiled birth writes the
resolved scalar/array links explicitly; automatic recipe-default remapping is
covered separately by [native instance tests](../runtime_instances_native.cpp).

## Publish and check NativeAOT

Publishing runs trusted build tooling and needs the matching host's native linker.
Windows publication requires an x64 Visual Studio C++ developer environment and
a compatible Windows SDK. It cannot be cross-published from Linux.

```sh
python3 scripts/publish_native_gameplay.py \
  --project tests/managed_instance_gameplay/Poima.ManagedInstanceGame.csproj \
  --type Poima.Tests.ManagedInstanceGame \
  --dotnet .cache/toolchains/dotnet-10.0.401/dotnet \
  --rid linux-x64 --engine-version 0.0.78 \
  --work build/instances-example/native-linux-work \
  --output build/instances-example/native-linux
python3 tests/managed_instances_contract.py \
  --binary build/runtime-headless/poima \
  --descriptor build/instances-example/native-linux/native-gameplay.json \
  --manifest build/instances-example/native-linux/game.poima-components.json \
  --source-directory /path/to/kenney-protagonists-1.1 \
  --output build/instances-example/native-linux-check
```

For Windows, run the publisher under native Windows Python, select `--rid win-x64`
and the installed `dotnet.exe`, and use distinct work/output directories. The
artifact includes its component manifest, native library and dependency notices.
Its module identity is `poima.test.managed-instances`; its initial `Mode=0` waits
for the fixture's birth action. A shipping project must explicitly set
`gameplay.values` to `{"Mode":1}` to create both actors on its first Tick.

## Export, relocate and render on Windows

The [bundle verifier](../runtime_instances_bundle.py) takes the authored world
left by a successful compiled contract, its installed Windows runtime and the
matching Windows NativeAOT artifact. It adds a separate observer controller
`1000` with camera `100`, which the current
[project entry format](../../docs/PROJECTS.md) requires. It keeps the NPC template
unchanged, sets the typed initial `Mode=1`, exports, relocates the bundle outside
the checkout and deletes its owned authoring project before running the player.

From native Windows Python:

```text
python tests/runtime_instances_bundle.py
  --binary build/windows-runtime/poima.exe
  --runtime build/runtime-windows
  --artifact build/instances-example/native-windows/native-gameplay.json
  --world build/instances-example/native-windows-check/world.json
  --output build/instances-example/bundle-check
  --gpu 0 --gpu 1 --ticks 90
```

Join the lines into one command in your shell. Use one `--gpu` for a single
available Vulkan device. The exporter, installed runtime and artifact must have
the same exact engine version. Export requires an installed runtime tree;
`cmake --install build/windows-runtime --prefix build/runtime-windows` supplies
it. See [project/export setup](../../docs/PROJECTS.md#install-a-runtime-and-export).

The source-free players run before a separate owned copy of bundled content is
opened for headless checks. The verifier expects the compiled first Tick to
birth two actors, physical movement, two skin dispatches totaling 2,058 original
vertices, complete disjoint node maps and independent 1×/2× loop clocks. It
compares player pixels with a separate same-artifact native runtime capture at
the same tick and observed UI revision. This establishes player/runtime
correspondence; the independent original-source pose checks remain the separate
compiled contract above. It is not a clean-machine, physical-input, editor,
crowd-performance or general character-shipping qualification.

Every verifier output directory must be new. Input worlds, caller assets,
installed runtimes and published artifacts remain unchanged. Reports, RPC
records, captures, relocated bundles and failure logs are retained. These tests
use exact frozen schemas and artifacts; they do not demonstrate arbitrary save
migration or asset retargeting compatibility.
