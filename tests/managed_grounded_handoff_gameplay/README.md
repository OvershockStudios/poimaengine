<!-- SPDX-License-Identifier: Apache-2.0 -->

# Compiled grounded handoff fixture

This actual C# `Game<TState>` exercises requested grounding controls, generated
point/oriented IK and linked physical takeover through the existing gameplay
SDK. Its 116 entity/scalar fields have an explicitly checked 920-byte layout.
Callback checks join both foot endpoints and normalized FK rotation matrices
to point/oriented IK under the prepared actor frame. The SDK exposes requested
`Enabled`/`Weight` controls, not contact or planting caches. The Python driver inspects those complete native diagnostics separately
and rejoins them to full saved witnesses and literal replies.

The original deterministic fixture has seven nodes, two planted feet, seven
spherical proxies and six joints, a moving/rotating spawned support with
nonuniform render scale, an independently grounded rig and distinct base/layer
clocks. Its tiny procedural triangle requires no downloaded art. This is a
contract fixture, not a production character or performance sample.

The complete authored inventory is 21 entities: two rig wrappers, fourteen rig
nodes, two triangle primitive children, the controller actor, its separate
camera and the independent floor. The retirable support is a template until
the runtime spawns it.

Both compiled support-retirement paths clear the game's live `Support`
`EntityId` after queuing its despawn. The native historical contact and planting
records retain the retired support identity; the driver checks both lifetimes.

The authored grounding profiles explicitly include the required sole offset,
unit up/forward axes and reference height, plus the native fixture's contact
thresholds, probe distances, correction/slope limits and planting policy.

## Build matching assemblies

Use the [managed prerequisites](../../docs/MANAGED_GAMEPLAY.md#build). Build the
bridge, then compile this game against the exact same SDK copy:

```sh
.cache/toolchains/dotnet-10.0.401/dotnet build managed/Poima.ManagedBridge/Poima.ManagedBridge.csproj -c Release --artifacts-path "$PWD/build/grounded-managed" --disable-build-servers
.cache/toolchains/dotnet-10.0.401/dotnet build tests/managed_grounded_handoff_gameplay/Poima.ManagedGroundedHandoffGame.csproj -c Release --artifacts-path "$PWD/build/grounded-managed" --disable-build-servers -p:PoimaGameplayAssembly="$PWD/build/grounded-managed/bin/Poima.Gameplay/release/Poima.Gameplay.dll" -p:BuildProjectReferences=false -p:UseAppHost=false
```

A qualification run may use a retained SDK/bridge pair with matching hashes;
that identifies the actual consumed images separately from these source-build
commands. Keep each output directory intact, including `Poima.Gameplay.dll`,
the bridge runtime configuration and dependency files. The game is portable IL;
hostfxr must match the native engine's operating system. No Native AOT compiler
is involved.

## Run the contract

The driver builds nothing and opens no window. Give it prebuilt images and a
fresh output directory beneath an external process-tree owner:

```sh
python3 -B tests/grounded_handoff_gameplay_contract.py \
  --binary build/linux-game/poima \
  --hostfxr .cache/toolchains/dotnet-10.0.401/host/fxr/10.0.12/libhostfxr.so \
  --bridge build/grounded-managed/bin/Poima.ManagedBridge/release/Poima.ManagedBridge.dll \
  --assembly build/grounded-managed/bin/Poima.ManagedGroundedHandoffGame/release/Poima.ManagedGroundedHandoffGame.dll \
  --output build/grounded-managed/consumer
```

For Windows use native Windows Python, the Windows CLI and Windows hostfxr,
for example `C:/Program Files/dotnet/host/fxr/10.0.14/hostfxr.dll`. The existing
[Windows process owner](../switchback_ownership.py) supplies a Job boundary.
`--windows-interop` instead maps local WSL filesystem arguments for a Windows
CLI; it does not rewrite native replies, receipts, saved paths or identities.

The workflow has three sequential fresh CLI processes. It retains complete
archives, generated field layouts/types, all native entity/part/joint/contact
and planting observations, manifest identities, original failure replies and
whole-byte comparisons. It covers staged setters, prepared actor/FK/IK reads,
malformed/duplicate/ownership/passive writes, immediate and late callback
rollback, active-cold immediate takeover, both support-retirement timings,
actual native body-index reuse, historical diagnostics and complete saved
continuation. Literal streams and archives are rejoined after owner closure.

The new combined policy's actual passing platforms and compiler images belong
to its qualification record. This source alone does not prove execution,
imported-humanoid motion, rendered quality, physical input or performance.
