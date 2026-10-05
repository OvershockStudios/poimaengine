# C# gameplay in the native runtime

Poima 0.0.14 can load handwritten C# into its native EnTT/Jolt runtime, inspect and edit typed gameplay fields, and replace game code while retaining compatible state. The example uses the same native collider queries and kinematic motion as the CLI: press **E** while looking at the sliding door to open it. Agents can submit the same `use` action through headless steps or Vulkan replay.

This is the first integrated gameplay SDK subset: one `Game<TState>` module per runtime, with input, entity observation, raycasts and kinematic movement. C++20 remains the engine language. CoreCLR supplies JIT compilation and garbage collection during development. Production native AOT, general per-entity gameplay components and the complete SDK remain unfinished.

## Build

From the repository root, obtain the optional workspace-local Linux .NET SDK if it is absent:

```sh
python3 scripts/bootstrap_tools.py --only dotnet
.cache/toolchains/dotnet-10.0.401/dotnet build managed/Poima.ManagedBridge/Poima.ManagedBridge.csproj -c Release
.cache/toolchains/dotnet-10.0.401/dotnet build examples/managed/DoorGame/Poima.DoorGame.csproj -c Release
cmake --preset runtime-headless -DPOIMA_ENABLE_MANAGED_GAMEPLAY=ON \
  -DPOIMA_DOTNET_HOST_HEADERS="$PWD/.cache/toolchains/dotnet-10.0.401/packs/Microsoft.NETCore.App.Host.linux-x64/10.0.12/runtimes/linux-x64/native"
cmake --build --preset runtime-headless
ctest --preset runtime-headless
```

For the Windows player, configure and build `windows-runtime` with the same two `-D` options after completing the [Windows toolchain setup](BUILD.md). The host headers contain conditional Windows/Linux definitions and were used for both tested builds. These paths identify the measured SDK; another SDK requires its matching header path and separate qualification. The CMake presets leave managed gameplay off by default, and the authoring-only `headless` build needs no .NET installation.

Keep the bridge's output directory together, including `Poima.Gameplay.dll`, its dependency metadata and `Poima.ManagedBridge.runtimeconfig.json`. The game assembly is portable IL; the native executable must load a CoreCLR installation for its own operating system. Tested installations:

| Native engine | Explicit hostfxr path |
| --- | --- |
| Linux x64 | `.cache/toolchains/dotnet-10.0.401/host/fxr/10.0.12/libhostfxr.so` |
| Windows x64 | `C:/Program Files/dotnet/host/fxr/10.0.11/hostfxr.dll` |

No runtime is installed or redistributed by the engine build. Hostfxr and the stable bridge remain loaded for the process lifetime. Changing their selected paths requires restarting the engine process; only game assemblies are replaced during gameplay reload.

## Write gameplay

The [complete sliding-door sample](../examples/managed/DoorGame/DoorGame.cs) references only `Poima.Gameplay` and ordinary .NET types. It does not require C++, unsafe code, renderer internals or JSON inside its update. Its structure is:

```csharp
using Poima;

public struct CounterState
{
    public EntityId Player;
    public int Uses;
}

[GameModule("my-game.counter")]
public sealed class CounterGame : Game<CounterState>
{
    public override void Initialize(ref CounterState state)
    {
        state.Player = new EntityId(0, 100);
    }

    public override void Tick(ref CounterState state, GameContext context)
    {
        if (context.Pressed(state.Player, GameAction.Use)) ++state.Uses;
    }
}
```

`TState` has sequential unmanaged layout and 1–128 public mutable fields, totaling at most 65,536 bytes. Supported fields are `int`, `long`, `float`, `double` and `EntityId`. Floating values must remain finite. The protocol represents `long` as a decimal string to preserve all 64 bits and `EntityId` as 32 hexadecimal digits. Booleans, strings, arbitrary nested structs and collections are not yet supported state fields; the door uses an integer for its open/closed state. Game classes cannot have instance fields. Put authoritative mutable data in `TState` so the runtime can migrate, inspect and roll it back.

`GameContext` is borrowed for the current call. It exposes:

| API | Behavior |
| --- | --- |
| `Tick`, `DeltaTime` | Current tick before its physics update, and a fixed 1/60 second interval. |
| `Inputs`, `Pressed(entity, action)` | Current controller inputs; `Jump` and `Use` are press edges. |
| `Get(entity)` | Live world transform, velocity, body motion kind and remaining motion ticks. |
| `Raycast(origin, direction, distance, ignore)` | Closest collider hit, or null; see the [native ray contract](PHYSICS_INTERACTIONS.md). |
| `PlaySound(emitter, gain)` / `StopSound(voice)` | Create/stop a native voice in the gameplay transaction; [event/recording contract](AUDIO_EVENTS.md). |
| `MoveKinematic(entity, position, rotation, ticks)` | Queue a validated native kinematic target for this tick. |

Controller look is applied before C# runs, so a use action in the same tick sees the new camera direction. Gameplay then runs before physics advances. Queued motions are validated together before application, with at most 128 per tick. Duplicate targets, invalid bodies and exceeding native speed/duration limits fail the batch. Explicit caller motion and gameplay may not target the same body on the first tick of a batch. Movement is held for a step/replay segment; look, jump and use apply only on its first tick. Read-only entity queries during gameplay see the current state, not the eventual result of queued motion.

This is trusted local project code. Do not retain borrowed pointers, start background gameplay work or store authoritative state in statics/external objects. Native-state rollback cannot undo file writes, subscriptions, static variables or other external side effects. Managed exceptions become structured engine errors; this is not a sandbox for arbitrary assemblies.

## Load, inspect and edit

Discover operations through `world.describe` schema revision **14**. `managed_gameplay` and `gameplay_reload` indicate this optional integration; broad `hot_reload` remains false because content/shader/general component reload is incomplete.

First author [interaction-room.jsonl](../examples/interaction-room.jsonl) into a fresh world using the [world service](WORLD_SERVICE.md). Continue sending these requests to that same engine process. This Windows example uses absolute paths; relative paths resolve against the world document's parent directory, not the shell working directory:

```jsonl
{"jsonrpc":"2.0","id":2,"method":"runtime.start","params":{"session_id":"00000000000000000000000000000384","revision":1}}
{"jsonrpc":"2.0","id":3,"method":"runtime.gameplay.load","params":{"session_id":"00000000000000000000000000000384","request_id":"000000000000000000000000000007d0","expected_tick":0,"expected_revision":0,"hostfxr":"C:/Program Files/dotnet/host/fxr/10.0.11/hostfxr.dll","bridge":"D:/poimaengine/managed/Poima.ManagedBridge/bin/Release/net10.0/Poima.ManagedBridge.dll","assembly":"D:/poimaengine/examples/managed/DoorGame/bin/Release/net10.0/Poima.DoorGame.dll","type":"Poima.Examples.DoorGame","values":{"UseDistance":10}}}
{"jsonrpc":"2.0","id":4,"method":"runtime.step","params":{"session_id":"00000000000000000000000000000384","request_id":"000000000000000000000000000007d1","expected_tick":0,"ticks":120}}
{"jsonrpc":"2.0","id":5,"method":"runtime.step","params":{"session_id":"00000000000000000000000000000384","request_id":"000000000000000000000000000007d2","expected_tick":120,"ticks":120,"inputs":[{"entity":"00000000000000000000000000000064","use":true}]}}
{"jsonrpc":"2.0","id":6,"method":"runtime.gameplay.inspect","params":{"session_id":"00000000000000000000000000000384","tick":240,"fields":["Open","Activations"]}}
```

The final inspection reports `Open: 1`, `Activations: 1` and gameplay revision 1. The fixture's longer interaction distance lets the stationary player reach the door. For normal play, the sample default is three meters; approach the door first. `runtime.play` uses the loaded game automatically, supports `use: true` in replay segments and binds interactive use to E. Physical keyboard/mouse interaction still needs a foreground human qualification session.

| Operation | Contract |
| --- | --- |
| `runtime.gameplay.inspect` | Requires `session_id`; optional `tick` guard. Returns session, tick, gameplay revision and module, or null before load. Compact default includes identity, loaded assembly SHA-256 and values. `include_schema: true` includes field types/layout, assembly/type and migration report. Optional unique `fields` selects at most 128 values. |
| `runtime.gameplay.load` | Requires session, request ID, expected tick, expected gameplay revision, hostfxr, bridge, assembly and fully qualified type. Optional `values` applies a field patch after initialization/migration. Returns complete metadata and increments gameplay revision. |
| `runtime.gameplay.edit` | Same session/request/tick/revision guards, with a required `values` object. Atomically updates validated fields and increments gameplay revision. |
| `runtime.gameplay.collect` | Requires the current session and an initialized bridge. Explicit diagnostic GC reports `active_modules` and `retired_alive`; use outside frame-time measurements. |

Gameplay revision is separate from authored-world revision and tick. It changes on successful load/edit, not each game update. Load/edit share the runtime's 32 retry receipts with step/play: an identical request ID and payload returns its previous result without rerunning the operation; a different payload conflicts. Guards reject stale ticks/revisions. Unknown fields, unsupported types and invalid values are errors, with no partial field edits. Important error codes are `-32003` for an unavailable build feature, `-32009` for a guard conflict, `-32010` for reused request identity, `-32060` for gameplay load/edit/collection failure and `-32040` for a failed headless simulation batch.

## Replace game code

Build the game project again while retaining the engine process, then call `runtime.gameplay.load` with a new request ID and current tick/gameplay revision. The engine loads assembly bytes into a new collectible context and computes their actual SHA-256. The main assembly has no persistent file lock. Rebuild/load happens explicitly; file watching is not implemented.

Initialization runs into fresh native-owned staging state. Matching field names and exact field kinds are copied from the old state; added fields keep initialization defaults and removed fields are listed in the migration report. A retained field changing type, a changed module identity, missing/broken assembly or failed initializer rejects replacement. Successful publication retires the old context. A failed compile never replaces the running assembly; a failed load/migration/edit leaves its state and physics intact. This is compatible-field migration, not arbitrary user-defined migration or hot replacement of the engine itself.

Each `runtime.step` checkpoints game state together with physics, controller angles and active kinematic targets. If any tick fails, that entire batch rolls back. Continuous play/replay advances using individual ticks, so a later failure retains earlier completed ticks under its existing partial-progress contract. Authoring documents are never modified by gameplay updates. Runtime state currently disappears on stop/restart; persistent game saves and authored gameplay components remain future work.

Collectible assembly unload is cooperative. External event handlers, threads or retained objects can keep a retired context alive. Normal test reloads collect completely; a deliberately retained event handler is detected as `retired_alive: 1`. The engine reports this condition rather than claiming it can force arbitrary code to unload. There are guards of 32 simultaneously active modules in the bridge, 1,024 recorded retired contexts before collection, and a 64 MiB main-assembly file limit. Only one module is attached to a runtime.

## Verification and limits

Run the actual compiler/reload/rollback fixture on Linux:

```sh
python3 scripts/verify_gameplay.py \
  --binary build/runtime-headless/poima \
  --dotnet .cache/toolchains/dotnet-10.0.401/dotnet \
  --hostfxr .cache/toolchains/dotnet-10.0.401/host/fxr/10.0.12/libhostfxr.so \
  --bridge managed/Poima.ManagedBridge/bin/Release/net10.0/Poima.ManagedBridge.dll \
  --output build/gameplay-contract-linux --reloads 100
```

For Windows, select `build/windows-runtime/poima.exe`, the installed Windows `dotnet.exe` and `hostfxr.dll`, a separate output directory and `--windows-interop`. `tests/gameplay_capture.py` additionally accepts `--game examples/managed/DoorGame/bin/Release/net10.0/Poima.DoorGame.dll`, `--gpu 0` or `1`, and `--output build/gameplay-capture` to check a continuous 480-tick Vulkan replay against independent headless simulation and captures.

[Recorded evidence](evidence/m2-managed-gameplay.json) includes 100 successful reloads on each OS, real compile/load/initialization failures, compatible-field migration, finite numeric/lossless integer edits, retry/guard behavior, same-tick look/use ordering, complete batch rollback and an intentional retained-context probe. Both laptop GPUs pass scripted door interaction, native-state/C#-field equality and exact final-image parity against an independent headless run. The build also passes 12 authoring-only and 15 runtime CTest suites and the applicable Windows CLI/native tests.

| C#-triggered closed door | C#-triggered open door |
| --- | --- |
| ![Closed door](evidence/m2-csharp-door-closed.png) | ![Open door](evidence/m2-csharp-door-open.png) |

These are small integration fixtures. They do not qualify full-game frame times, large SDK builds, allocation-free gameplay, arbitrary cross-platform deterministic C#, production AOT/console deployment or the whole engine. General component registration, generated bindings, general events/jobs, spawn/despawn, animation, complete audio/environmental controls, VFX, UI, game saves, automatic source watching and concurrent authoring while a player window owns the connection remain unfinished. The [independent AOT lab](MANAGED_SHIPPING_LAB.md) does not make this CoreCLR integration shipping-ready.

Sound callbacks extend the gameplay service table to ABI version 2 (48 bytes); the outer call remains version 1 (80 bytes). Rebuild Poima.Gameplay, Poima.ManagedBridge and game assemblies together when upgrading from 0.0.15. Native voices survive compatible game reloads; failed tick batches roll back their handles and state.
