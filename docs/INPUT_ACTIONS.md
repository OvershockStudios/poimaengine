# Named gameplay actions

Named digital actions connect keyboard, mouse and gamepad bindings to compiled
C# gameplay and headless simulation. Use them for independent Fire, Aim, Reload,
Sprint or game-specific controls. Movement, look, Jump and Use retain their
existing interfaces. Discover the current limits with
`world.describe {"view":"section","name":"input_actions"}` and the available
physical controls with `input.describe`.

## Define bindings

Add an `actions` object to a complete [input profile](INPUT_PROFILES.md):

```json
"actions": {
  "aim": ["mouse.3"],
  "fire": ["mouse.1"],
  "reload": ["key.r"]
}
```

This member selects `poima.input.v3`, including when it is empty. Its omission
preserves v1/v2 profile bytes and hashes. Optional `gamepad` settings enable the
existing pad controls, sticks and trigger hysteresis. There are no mandatory
FPS bindings: choose controls from discovery and clear any conflicting legacy
binding in the same transaction. A control may appear only once across the
whole profile, including legacy and named bindings.

Names are ASCII `[a-z][a-z0-9_.-]{0,63}`: 1–64 characters, starting with a lower
case letter. A profile permits at most 32 names and zero to four controls per
name. Existing files cannot change format in place; create a new destination
for an explicitly converted profile. Contexts, chords and live rebinding menus
remain separate work.

## Send semantic input

`runtime.step` and player replay segments accept optional `actions` records:

```json
{
  "entity": "00000000000000000000000000000064",
  "actions": [
    {"name":"aim", "held":true, "pressed":true, "released":false},
    {"name":"fire", "held":true, "pressed":true, "released":false}
  ]
}
```

Records must be sorted by name and unique, with all four fields present and
Boolean state values. There are at most 32 records per controller and 32
controller inputs per batch. Held state repeats throughout the batch or replay
segment. Pressed and Released apply only on its first tick. A quick tap can
have both edges true and Held false. Omitted records are absent and neutral;
the runtime does not remember a previous batch's held input. Playback consumes
semantic input directly; changing physical bindings does not reinterpret it.

The physical evaluator retains held bindings and pending edges between polls.
It commits edges without allocation only after simulation succeeds. Failed
ticks retain pending edges; focus loss or UI capture clearing discards them.
Disconnecting a pad removes its held/press state, preserves keyboard/mouse
pending input, and emits a release when the last held alternative disappears.
`input.evaluate` observes this evaluator without advancing a world.

## Read in C#

Declare `IInputActionsGame` independently of other service markers:

```csharp
using Poima;

public struct CombatState
{
    public EntityId Player;
    public int Ammo, Aiming;
}

[GameModule("example.combat")]
public sealed class CombatGame : Game<CombatState>, IInputActionsGame
{
    public override void Initialize(ref CombatState state)
    {
        state = default;
        state.Player = new(0, 100); // Your authored controller entity.
        state.Ammo = 3;
    }

    public override void Tick(ref CombatState state, GameContext context)
    {
        var fire = context.GetInputAction(state.Player, "fire");
        var aim = context.GetInputAction(state.Player, "aim");
        var reload = context.GetInputAction(state.Player, "reload");
        if (reload.Pressed) state.Ammo = 3;
        if ((fire.Held || fire.Pressed) && state.Ammo > 0)
            --state.Ammo;
        state.Aiming = aim.Held ? 1 : 0;
    }
}
```

Replace the example controller ID with your authored entity. `InputActionState` has immutable Held,
Pressed, Released and Present values. A live entity without that input record
returns all false; an unknown entity rejects the callback. Reads are Tick-only
and bounded to 1,024 calls per tick. Input is transient: save the resulting
ammo, reload state or aiming state in native-owned gameplay fields. Those
fields join ordinary batch rollback, compatible reload and durable saves.
External side effects in trusted gameplay code remain outside rollback.

## Compatibility

The independent feature is `input_actions_v1`, requiring a 264-byte services
prefix in epoch 7. Its callback is appended at byte 256. The action result is a
version-1, 16-byte structure: version, byte count, flags and zero reserved field.
Flags are Held=1, Pressed=2, Released=4 and Present=8; absence has flags=0.
The SDK validates the returned version, size, flags and reserved value.

Call ABI 1/80, baseline services 176 bytes and the 40-byte legacy GameInput stay
unchanged. Earlier consumers receive their previously declared prefixes.
A larger host table does not grant undeclared movement, animation, navigation
or preference operations. The matched bridge, SDK and Native AOT generator
negotiate the named feature before construction. This gameplay extension does
not expand the released authoring-core v1 stability promise.

## Qualification

The [Windows qualification](evidence/m2-input-actions.json) covers native input
profiles and gamepad event evaluation, compiled ABI guards, actual CoreCLR and
Native AOT gameplay, compatible CoreCLR reload, complete batch rollback and
fresh-process save continuation. Forward and deferred Vulkan player replay
match the complete headless checkpoint on one GPU. These are functional checks;
physical devices, editor input, latency and a complete weapon framework remain
separate qualification.

The compiled consumer and reproducible fixtures are
[ManagedInputActionsGame.cs](../tests/managed_input_actions_gameplay/ManagedInputActionsGame.cs),
[input_actions_gameplay_contract.py](../tests/input_actions_gameplay_contract.py)
and [input_actions_player.py](../tests/input_actions_player.py). They require a
matching native engine, bridge/game assembly or published Native AOT artifact,
and an external bounded process owner. They do not build their own dependencies.

In the results, `durable_snapshot_comparisons` counts each selected save payload
compared with its recorded complete snapshot (42 CoreCLR, 40 Native AOT); these
counts are separate from the before/after rollback and continuation pairs.
