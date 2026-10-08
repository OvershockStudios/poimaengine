# Compiled navigation fixture

A small acceptance fixture for an actual compiled C# NPC using `INavigationGame`
and `ICharacterInputGame`. `NavigationGame.cs` stores up to ten XYZ corners and
its cursor in one generated 512-byte component. Scalar gameplay state keeps the
goal, route status and test counters. The NPC turns and moves through normal
`SetCharacterInput`; the Python verifier never patches a route or teleports it.

Build against the matching SDK and extract the real generated component
manifest before running the harness. For example, from the repository root:

```sh
dotnet build tests/managed_navigation_gameplay/Poima.NavigationGameplay.csproj \
  -c Release --artifacts-path build/navigation-game-fixture -m:1
dotnet build managed/Poima.NativeGame.Generator/Poima.NativeGame.Generator.csproj \
  -c Release --artifacts-path build/navigation-game-fixture -m:1
dotnet build/navigation-game-fixture/bin/Poima.NativeGame.Generator/release/Poima.NativeGame.Generator.dll \
  --components build/navigation-game-fixture/bin/Poima.NavigationGameplay/release/Poima.NavigationGameplay.dll \
  build/navigation-game-fixture/components.json
python tests/navigation_runtime_contract.py \
  --binary /path/to/poima --hostfxr /path/to/hostfxr --bridge /path/to/Poima.ManagedBridge.dll \
  --assembly build/navigation-game-fixture/bin/Poima.NavigationGameplay/release/Poima.NavigationGameplay.dll \
  --manifest build/navigation-game-fixture/components.json --output build/navigation-game-replay
```

Select an actual hostfxr library for the engine's OS and the matched bridge. The
output must be new. `--windows-interop` converts WSL-local input paths when the
supplied engine is Windows native. Alternatively, `--descriptor` selects an
already published Native AOT artifact instead of `--assembly/--hostfxr/--bridge`.
Publication is separate; the runner does not build artifacts or claim deployment.

The harness authors a flat static floor, central cover, a camera-free character,
its component and logical buttons; bakes and durably binds navigation; then
checks real compiled queries and native motion. It independently checks the
blocked straight ray, stored multi-corner buffer, observed detour, actual native
eight-attempt quota and reset, failed-query span preservation, late-batch
rollback, compatible CoreCLR reload or Native AOT replacement rejection, and
same/fresh-owner save continuation and immediate replan. A single-tick replay is
compared with a grouped continuation. Optional `--legacy-binary` and
`--unavailable-binary` exercise actual host rejection before the game constructor.
The unavailable binary must include simulation and exclude navigation.

`Game<TState>.Initialize(ref TState)` and `ControlContext` expose no navigation
query in this SDK. This fixture therefore makes no claim about deliberately
forged non-Tick callbacks; those require separate native ABI tests. It also does
not qualify crowds, moving obstacles, arbitrary stairs, graphics, editor input,
physical devices, performance, or clean-machine deployment. Local evidence
records the actual backend, input/source hashes, RPCs and process cleanup; a
source fixture by itself is not a passing qualification.
