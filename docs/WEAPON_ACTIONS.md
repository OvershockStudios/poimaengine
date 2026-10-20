# Weapon actions

The optional [FPS package](../packages/fps/README.md) models magazine, chamber,
reserve ammunition and fixed-tick weapon actions. It has no native callbacks;
the game supplies inputs, stores the returned state and handles the events.
Use it with native input actions, hit queries, animation and camera controls.

## State and ownership

`Weapons.Step` accepts a `WeaponDefinition`, `WeaponState` and `WeaponInput`,
and returns a `WeaponStep` containing the next state and `WeaponEvents` flags.
These are immutable numerical records. Keep their persistent operands as
ordinary primitive fields in `Game<TState>` or typed components; the records
are not nested serializable game-state declarations.

A definition specifies magazine capacity, fire interval, eject/insert/chamber
and equip durations, fire mode and burst count. Durations count fixed ticks,
not rendered frames or animation events. At a 60 Hz simulation, an interval
of six ticks permits ten shots per second. Changing the simulation frequency
requires choosing timings for that frequency.

Magazine rounds exclude the chamber. A tactical reload can therefore leave
one chambered round plus a full magazine. An empty reload fills the magazine
then moves one round into the chamber. The explicit initial state determines
whether a newly spawned weapon starts loaded, empty, ready or holstered.

State includes the phase, remaining phase ticks, cooldown, remaining burst,
previous trigger-held value and accepted-shot ordinal. Save all of them.
Omitting the previous trigger value can turn a held semi-auto trigger into an
extra shot after restoration. Omitting a reload phase can transfer ammunition
twice. Recipes and the game's selected weapon identity must also remain
compatible with saved state.

## Fire modes and events

| Mode | Trigger policy |
| --- | --- |
| `SemiAutomatic` | One rising edge; holding does not fire again when cooldown expires. |
| `Automatic` | Fire while held whenever ready and cooldown reaches zero. |
| `Burst` | A ready rising edge starts the configured burst; accepted shots continue at the fire interval after release. |

A burst starts only when its rising edge arrives with zero cooldown. A press
during cooldown is consumed rather than queued. An existing burst continues
through its normal cooldown between shots.

A shot requires a chambered round. It consumes that round and immediately feeds
one magazine round when available, representing a closed-bolt weapon. `Shot`
increments `AcceptedShots` exactly once. An empty attempt emits `DryFire`,
starts the normal cooldown and cancels an unfinished burst without advancing
the shot ordinal. Inputs received during a busy phase are not queued as new
semi-auto or burst presses.

Drive hit queries, recoil and fire sounds from **`Shot`**, not the trigger edge:

```csharp
var step = Weapons.Step(definition, previous, input);
var next = step.State;
if ((step.Events & WeaponEvents.Shot) != 0)
{
    ulong ordinal = checked((ulong)(next.AcceptedShots - 1));
    var aimKick = Recoil.AngularKick(aimPattern, ordinal, seed,
        new(.003, .002, 0), RecoilMode.Hybrid, stream: 0);
    var visualKick = Recoil.AngularKick(weaponPattern, ordinal, seed,
        new(.010, .008, .004), RecoilMode.Hybrid, stream: 1);
    // Apply the independent aim/visual impulses and stage the hit and sound.
}
```

The game retains native staged-write and rollback rules. A returned event is
not an external delivery guarantee: keep authoritative effects inside the
simulation boundary. Persist identities separately if exporting events to
systems that need deduplication.

## Reload and interruption

A reload has separate eject, insert and chamber phases. Ejection returns the
partial magazine to reserve; insertion transfers at most capacity from reserve.
These transfers occur once, at their phase boundary. This model retains partial
magazines rather than dropping them as physical items.

Cancellation and holstering retain whatever transfers already completed.
Cancelling before ejection leaves the original magazine. Cancelling between
ejection and insertion leaves its rounds in reserve. Cancelling before the
chamber phase finishes leaves an unchambered weapon; request reload again to
chamber available ammunition. The helper does not restore an old magazine or
create ammunition to make an animation look continuous.

The order within a step is explicit:

1. Validate the complete recipe and state; decrement an existing cooldown.
2. Update the saved trigger-held value and derive its rising edge.
3. Handle holstering first. While holstered, only equip can start an action.
4. Cancel an existing reload, or start an eligible reload from Ready.
5. Advance one existing phase tick and commit a completed phase's transfer.
6. If ready, evaluate firing with the updated cooldown and trigger policy.

Starting an action retains its full configured duration on that tick. A phase
started by another phase's completion also retains its full duration. Returning
to Ready at a phase boundary permits firing on that same tick. Cancelling a
reload suppresses firing for that tick. Holstering cancels reload/burst state;
equipping has its own duration. The normal cooldown continues while busy.

The phase and remaining ticks can drive procedural weapon motion or select
imported clips. Animation is presentation; changing clip speed must not silently
move the ammunition transfer to a different authoritative tick. Full animation
notifies, attachments, multiple-weapon inventory, per-shell loading, open-bolt
operation and dropped-magazine items require additional game/package policies.

## Bounds and recovery

Magazine capacity is 1–4,096, burst length 1–16 and every duration 1–1,000,000
ticks. Total magazine, chamber and reserve rounds cannot exceed 1,000,000.
The chamber contains zero or one round. Phase timers and burst/cooldown state
must match the supplied definition. ReloadInsert requires an empty magazine;
Chambering requires an empty chamber and at least one magazine round. The accepted-shot counter is a nonnegative
signed 64-bit value; overflow rejects instead of wrapping.

Invalid inputs throw before returning a successor. The helper owns no mutable
RNG, wall clock, hidden state, I/O or normal-path heap allocation. Repeating the
same definition, state and input returns the same successor. A game changing
capacity, timings or fire mode must first make its current state valid for the
new definition; changing a definition is not a save migration.

For a fixed definition and no explicit inventory edits, the sum of magazine,
chamber, reserve and accepted shots remains constant. This is an ammunition
accounting invariant, not a promise of complete cross-platform engine replay.

See the [Weapon Actions sample](../examples/weapon-actions/README.md) for flat
saved fields, logical controls and native checkpoint checks.

## Verify the rules

Run the independent pure action checks with .NET 10:

```sh
dotnet run --project tests/WeaponActions/Poima.WeaponActions.Tests.csproj -c Release
```

The checks use explicit cadence and transfer cases, deterministic input traces,
ammunition conservation and complete caller-state replay. They do not execute
the native engine. The [sample verifier](../tests/weapon_actions_gameplay_contract.py)
separately exercises compiled callbacks, real native hit queries, pause/control
intents and complete native save/rollback/fresh-process continuation.

[Checkpoint evidence](evidence/m2-weapon-actions.json) identifies the actual
runtime, SDK and compiled module used for those checks. Native AOT shipping,
interactive devices, animation quality and representative performance require
their own qualification.
