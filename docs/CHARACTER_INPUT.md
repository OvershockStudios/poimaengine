# Compiled character controls

`CharacterController` can drive a player or an autonomous actor. Author its
required `camera` field as `null` for an actor without a camera. A supplied
camera ID must still name a direct child with a `Camera` component. Camera-free
actors retain capsule collision, gravity, heading, jump, observation, rollback
and saved state. They are excluded from interactive player-camera selection.

## C# movement

Declare `ICharacterInputGame` alongside `Game<TState>`:

```csharp
using Poima;

public struct WalkerState { public EntityId Actor; }

[GameModule("example.walker")]
public sealed class Walker : Game<WalkerState>, ICharacterInputGame
{
    public override void Initialize(ref WalkerState state)
        => state.Actor = new(0, 300);

    public override void Tick(ref WalkerState state, GameContext context)
        => context.SetCharacterInput(state.Actor, moveRight: 0, moveForward: 1);
}
```

The target must already be a live controller. Movement is relative to its
heading, uses each controller's speed, and clamps diagonal magnitude to one.
The API accepts move axes in `[-1,1]`, yaw/pitch deltas in `[-180,180]` degrees,
and an optional `jump`. Jump requires ground support; pitch clamps to ±85°.
These are the existing direct-velocity capsule controls, including in air.
[Static pathfinding](NAVIGATION.md#query-from-compiled-gameplay) is available through the independent `INavigationGame` marker. Acceleration, dynamic avoidance and a production locomotion model remain separate work.

Each command applies for one fixed 60 Hz Tick. Issue movement on every Tick
while walking; omission neutralizes horizontal movement on the next Tick.
Look and jump apply once per commanded Tick. At most 32 distinct controllers
can be commanded per Tick. Duplicate, dead and non-controller targets reject.

The setter stages an intent. `Get(entity)` inside Tick sees the current
pre-physics state after caller-input or neutral-input preparation; it does not
see this Tick's staged character command. In particular, an omitted actor's
horizontal velocity has already been neutralized before the callback.
`GameContext.Inputs` contains caller input only, excluding these staged intents.

An explicitly supplied `RuntimeInput` owns its target for every Tick in that
step batch, even when its axes and buttons are neutral. A compiled command for
the same target rejects the entire batch. Player and NPC controls can target
different controllers together. Caller movement remains held across a batch;
caller look/jump/use edges apply only on its first Tick.

After the game callback and candidate preparation succeed, compiled intents
apply before physics. A later failure restores the complete batch's physics,
controller angles, gameplay and other runtime state. Queued intents are cleared;
saves retain actual controller and decision state rather than pending commands.

## Service negotiation

The independent `character_input_v1` feature requires a 216-byte services-7
prefix. Original 176-byte baseline, 192-byte inertial-animation and 208-byte
masked-layer profiles keep their layouts and bounded views. Character input
does not require either animation marker. A module combining features declares
each one; a larger allocation alone does not grant an undeclared feature.

`PoimaGameCharacterInputV1` is a version-1, exact 48-byte record with entity,
move/look axes, a jump flag and zero reserved word. The setter occupies byte
208 of `PoimaGameCharacterServicesV1`. Host negotiation rejects an unsupported
declaration before constructing the game. CoreCLR remains the reloadable path;
NativeAOT libraries remain pinned for the process lifetime.

See [runtime controls](RUNTIME.md), [C# gameplay](MANAGED_GAMEPLAY.md) and
[native artifacts](NATIVE_GAMEPLAY.md) for the surrounding contracts.

## Reproduce

Build `tests/managed_character_gameplay/Poima.ManagedCharacterGame.csproj`
against the current SDK, then run:

```sh
python3 tests/character_gameplay_contract.py build/runtime-headless/poima \
  --hostfxr /path/to/libhostfxr.so \
  --bridge /path/to/Poima.ManagedBridge.dll \
  --assembly /path/to/Poima.ManagedCharacterGame.dll \
  --output build/character-clr-check
```

For a published native artifact, replace the three managed paths with
`--native-descriptor /path/to/native-gameplay.json`. Add `--windows-interop`
when invoking a Windows executable from WSL. Use a new output directory for
each run. The fixture checks movement, wall contact, jump, player ownership,
neutral ticks, rejected commands, second-tick rollback and fresh-process saves.
The separate `character_legacy_compatibility.py` compares preserved and current
executables, including the original saved payload and continued input replay.
