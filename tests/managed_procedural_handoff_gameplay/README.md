<!-- SPDX-License-Identifier: Apache-2.0 -->

# Compiled procedural handoff fixture

This C# `Game<TState>` uses the real gameplay SDK to aim a three-joint rig, stage
point and oriented IK edits, transfer its committed pose into physical bodies,
and continue an independent animated rig. It retains 80 scalar/entity fields so
the verifier can compare the complete gameplay state across failure, rewind and
fresh-process restores.

The native content is an analytic three-node GLB with a small procedural triangle,
three spherical proxies and two joints. It does not require downloaded art.
It is a contract fixture, not a production character or performance sample.

## Build matching assemblies

Use the [managed build prerequisites](../../docs/MANAGED_GAMEPLAY.md#build).
Build the bridge first, then compile this game against its exact SDK copy:

```sh
.cache/toolchains/dotnet-10.0.401/dotnet build managed/Poima.ManagedBridge/Poima.ManagedBridge.csproj -c Release --artifacts-path "$PWD/build/procedural-managed" --disable-build-servers
.cache/toolchains/dotnet-10.0.401/dotnet build tests/managed_procedural_handoff_gameplay/Poima.ManagedProceduralHandoffGame.csproj -c Release --artifacts-path "$PWD/build/procedural-managed" --disable-build-servers -p:PoimaGameplayAssembly="$PWD/build/procedural-managed/bin/Poima.Gameplay/release/Poima.Gameplay.dll" -p:BuildProjectReferences=false -p:UseAppHost=false
```

These commands build matching assemblies from source. The recorded qualification
instead compiled this game against a retained, exact SDK/bridge pair; the
[record](../../docs/evidence/m2-procedural-physical-handoff-windows.json) identifies
those consumed images separately.

Keep the bridge output directory intact, including its runtime configuration and
`Poima.Gameplay.dll`. The game assembly is portable IL; hostfxr must match the
native engine's operating system. The driver compiles nothing.

## Run the contract

With a Linux game-profile CLI and a fresh output path:

```sh
python3 -B tests/procedural_handoff_gameplay_contract.py \
  --binary build/linux-game/poima \
  --hostfxr .cache/toolchains/dotnet-10.0.401/host/fxr/10.0.12/libhostfxr.so \
  --bridge build/procedural-managed/bin/Poima.ManagedBridge/release/Poima.ManagedBridge.dll \
  --assembly build/procedural-managed/bin/Poima.ManagedProceduralHandoffGame/release/Poima.ManagedProceduralHandoffGame.dll \
  --output build/procedural-managed/consumer
```

Use native Windows Python, the Windows CLI and the installed Windows hostfxr for
the Windows run. The qualified installation used
`C:/Program Files/dotnet/host/fxr/10.0.14/hostfxr.dll`. Collect qualification beneath
an external process-tree owner; the [Windows ownership helper](../switchback_ownership.py)
provides the tested Job boundary. The driver itself closes its three sequential
CLI owners and rejoins their literal request/reply streams after closure.

A successful run retains 30 complete save archives and observations, 17 complete
byte/typed-JSON comparisons, generated schema/field types, manifests, receipts
and original failure replies. The fixture also checks planned camera/bone/IK
coherence, same-callback setter invisibility, malformed and duplicate writes,
activation conflicts, late-Tick rollback and passive historical diagnostics.
See the [qualification record](../../docs/evidence/m2-procedural-physical-handoff-windows.json)
for exact images, independent audits and platform limits. This is CoreCLR proof;
it does not qualify Native AOT compilation, graphics or imported humanoids.
