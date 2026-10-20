// SPDX-License-Identifier: Apache-2.0
using System;

namespace Poima.Fps;

/// <summary>How the trigger requests shots from a ready weapon.</summary>
public enum WeaponFireMode
{
    SemiAutomatic,
    Automatic,
    Burst
}

/// <summary>Committed action stage. Every timer counts fixed calls to Weapons.Step.</summary>
public enum WeaponPhase
{
    Ready,
    Holstered,
    Equipping,
    ReloadEject,
    ReloadInsert,
    Chambering
}

/// <summary>Events produced by one completed step; only Shot authorizes shot effects.</summary>
[Flags]
public enum WeaponEvents
{
    None = 0,
    Shot = 1 << 0,
    DryFire = 1 << 1,
    ReloadStarted = 1 << 2,
    MagazineEjected = 1 << 3,
    MagazineInserted = 1 << 4,
    Chambered = 1 << 5,
    ReloadCancelled = 1 << 6,
    Holstered = 1 << 7,
    EquipStarted = 1 << 8,
    Equipped = 1 << 9
}

/// <summary>
/// Immutable action recipe. Magazine capacity excludes the independently retained chamber.
/// Every duration is at least one tick, including durations unused by the selected fire mode.
/// </summary>
public readonly record struct WeaponDefinition(
    int MagazineCapacity,
    int FireIntervalTicks,
    int ReloadEjectTicks,
    int ReloadInsertTicks,
    int ChamberTicks,
    int EquipTicks,
    WeaponFireMode FireMode,
    int BurstCount);

/// <summary>
/// Complete caller-owned action state. Persist its primitive members explicitly in gameplay state;
/// this helper record does not add nested game-state serialization support.
/// </summary>
public readonly record struct WeaponState(
    int Magazine,
    int Chamber,
    int Reserve,
    WeaponPhase Phase,
    int TicksRemaining,
    int Cooldown,
    int BurstRemaining,
    bool TriggerHeld,
    long AcceptedShots);

/// <summary>One fixed-tick input sample. Held state is observed even while actions block firing.</summary>
public readonly record struct WeaponInput(
    bool TriggerHeld = false,
    bool ReloadPressed = false,
    bool CancelReload = false,
    bool Holster = false,
    bool Equip = false);

/// <summary>Whole immutable result of one step.</summary>
public readonly record struct WeaponStep(WeaponState State, WeaponEvents Events);

/// <summary>
/// Deterministic, allocation-free fixed-tick weapon actions. No engine writes, hidden mutable state,
/// clock, randomness or reflection. The game constructs the initial state and owns hit queries,
/// audio, recoil and presentation. Only an accepted Shot event should drive those shot effects.
/// </summary>
public static class Weapons
{
    /// <summary>Largest combined magazine, chamber and reserve inventory.</summary>
    public const int MaxAmmo = 1_000_000;
    /// <summary>Largest configured action duration or fire interval, in fixed ticks.</summary>
    public const int MaxTicks = 1_000_000;

    /// <summary>
    /// Validates the complete recipe/state, decrements cooldown and then performs at most one stage
    /// advancement. Newly started stages retain their full timer. A stage completing to Ready may
    /// fire in the same call. Holster takes priority; cancellation suppresses reload/fire for this call.
    /// Reload stages preserve chambered ammunition and never undo a previously committed transfer.
    /// </summary>
    /// <remarks>
    /// Magazine excludes Chamber, so a full magazine plus a retained round is capacity + 1.
    /// AcceptedShots increments with checked long arithmetic only for Shot; overflow throws without
    /// returning a result. Invalid recipe/state throws ArgumentOutOfRangeException. Every failure
    /// leaves caller inputs untouched. Inventory plus AcceptedShots is conserved for an unchanged
    /// recipe unless the caller performs an explicit inventory operation between steps.
    /// </remarks>
    public static WeaponStep Step(
        in WeaponDefinition definition,
        in WeaponState state,
        in WeaponInput input)
    {
        Validate(definition, state);

        bool rising = input.TriggerHeld && !state.TriggerHeld;
        WeaponState next = state with
        {
            Cooldown = state.Cooldown > 0 ? state.Cooldown - 1 : 0,
            TriggerHeld = input.TriggerHeld
        };
        WeaponEvents events = WeaponEvents.None;

        if (input.Holster)
        {
            if (next.Phase != WeaponPhase.Holstered)
                events |= WeaponEvents.Holstered;
            if (Reloading(next.Phase))
                events |= WeaponEvents.ReloadCancelled;
            next = next with { Phase = WeaponPhase.Holstered, TicksRemaining = 0, BurstRemaining = 0 };
            return new WeaponStep(next, events);
        }

        if (next.Phase == WeaponPhase.Holstered)
        {
            if (input.Equip)
            {
                next = next with { Phase = WeaponPhase.Equipping, TicksRemaining = definition.EquipTicks };
                events |= WeaponEvents.EquipStarted;
            }
            return new WeaponStep(next, events);
        }

        if (input.CancelReload && Reloading(next.Phase))
        {
            next = next with { Phase = WeaponPhase.Ready, TicksRemaining = 0, BurstRemaining = 0 };
            return new WeaponStep(next, WeaponEvents.ReloadCancelled);
        }

        if (input.ReloadPressed && next.Phase == WeaponPhase.Ready)
        {
            if (next.Reserve > 0 && next.Magazine < definition.MagazineCapacity)
            {
                next = next with
                {
                    Phase = WeaponPhase.ReloadEject,
                    TicksRemaining = definition.ReloadEjectTicks,
                    BurstRemaining = 0
                };
                return new WeaponStep(next, WeaponEvents.ReloadStarted);
            }
            if (next.Chamber == 0 && next.Magazine > 0)
            {
                next = next with
                {
                    Phase = WeaponPhase.Chambering,
                    TicksRemaining = definition.ChamberTicks,
                    BurstRemaining = 0
                };
                return new WeaponStep(next, WeaponEvents.ReloadStarted);
            }
        }

        if (next.Phase != WeaponPhase.Ready)
        {
            next = next with { TicksRemaining = next.TicksRemaining - 1 };
            if (next.TicksRemaining == 0)
            {
                switch (next.Phase)
                {
                    case WeaponPhase.Equipping:
                        next = next with { Phase = WeaponPhase.Ready };
                        events |= WeaponEvents.Equipped;
                        break;
                    case WeaponPhase.ReloadEject:
                        next = next with
                        {
                            Reserve = next.Reserve + next.Magazine,
                            Magazine = 0,
                            Phase = WeaponPhase.ReloadInsert,
                            TicksRemaining = definition.ReloadInsertTicks
                        };
                        events |= WeaponEvents.MagazineEjected;
                        break;
                    case WeaponPhase.ReloadInsert:
                        int inserted = Math.Min(definition.MagazineCapacity, next.Reserve);
                        next = next with
                        {
                            Magazine = inserted,
                            Reserve = next.Reserve - inserted,
                            Phase = next.Chamber == 0 && inserted > 0 ? WeaponPhase.Chambering : WeaponPhase.Ready,
                            TicksRemaining = next.Chamber == 0 && inserted > 0 ? definition.ChamberTicks : 0
                        };
                        events |= WeaponEvents.MagazineInserted;
                        break;
                    case WeaponPhase.Chambering:
                        // Validation admits this stage only with a transferable round.
                        next = next with { Magazine = next.Magazine - 1, Chamber = 1, Phase = WeaponPhase.Ready };
                        events |= WeaponEvents.Chambered;
                        break;
                }
            }
            if (next.Phase != WeaponPhase.Ready)
                return new WeaponStep(next, events);
        }

        if (next.Cooldown != 0)
            return new WeaponStep(next, events);

        bool fire;
        if (definition.FireMode == WeaponFireMode.Burst)
        {
            if (next.BurstRemaining == 0 && rising)
                next = next with { BurstRemaining = definition.BurstCount };
            fire = next.BurstRemaining > 0;
        }
        else
            fire = definition.FireMode == WeaponFireMode.Automatic ? input.TriggerHeld : rising;

        if (!fire)
            return new WeaponStep(next, events);

        if (next.Chamber == 0)
        {
            next = next with { Cooldown = definition.FireIntervalTicks, BurstRemaining = 0 };
            return new WeaponStep(next, events | WeaponEvents.DryFire);
        }

        long shots = checked(next.AcceptedShots + 1L);
        next = next with
        {
            Magazine = next.Magazine > 0 ? next.Magazine - 1 : 0,
            Chamber = next.Magazine > 0 ? 1 : 0,
            Cooldown = definition.FireIntervalTicks,
            BurstRemaining = next.BurstRemaining > 0 ? next.BurstRemaining - 1 : 0,
            AcceptedShots = shots
        };
        return new WeaponStep(next, events | WeaponEvents.Shot);
    }

    private static bool Reloading(WeaponPhase phase) =>
        phase is WeaponPhase.ReloadEject or WeaponPhase.ReloadInsert or WeaponPhase.Chambering;

    private static void Validate(in WeaponDefinition definition, in WeaponState state)
    {
        if (definition.MagazineCapacity is < 1 or > 4096 ||
            definition.BurstCount is < 1 or > 16 ||
            definition.FireMode is not (WeaponFireMode.SemiAutomatic or WeaponFireMode.Automatic or WeaponFireMode.Burst) ||
            !Duration(definition.FireIntervalTicks) || !Duration(definition.ReloadEjectTicks) ||
            !Duration(definition.ReloadInsertTicks) || !Duration(definition.ChamberTicks) || !Duration(definition.EquipTicks))
            throw new ArgumentOutOfRangeException(nameof(definition), "Invalid capacity, fire mode, burst count or fixed-tick duration.");

        if (state.Magazine < 0 || state.Magazine > definition.MagazineCapacity ||
            state.Chamber is < 0 or > 1 || state.Reserve < 0 ||
            (long)state.Magazine + state.Chamber + state.Reserve > MaxAmmo || state.AcceptedShots < 0 ||
            state.Cooldown < 0 || state.Cooldown > definition.FireIntervalTicks ||
            state.BurstRemaining < 0 || state.BurstRemaining > definition.BurstCount ||
            (state.BurstRemaining != 0 && (state.Phase != WeaponPhase.Ready || definition.FireMode != WeaponFireMode.Burst)))
            throw new ArgumentOutOfRangeException(nameof(state), "Invalid ammunition, shot count, cooldown or pending burst.");

        int duration = state.Phase switch
        {
            WeaponPhase.Ready or WeaponPhase.Holstered => 0,
            WeaponPhase.Equipping => definition.EquipTicks,
            WeaponPhase.ReloadEject => definition.ReloadEjectTicks,
            WeaponPhase.ReloadInsert => definition.ReloadInsertTicks,
            WeaponPhase.Chambering => definition.ChamberTicks,
            _ => throw new ArgumentOutOfRangeException(nameof(state), "Unknown weapon phase.")
        };
        if ((duration == 0 ? state.TicksRemaining != 0 : state.TicksRemaining < 1 || state.TicksRemaining > duration) ||
            (state.Phase == WeaponPhase.ReloadInsert && state.Magazine != 0) ||
            (state.Phase == WeaponPhase.Chambering && (state.Chamber != 0 || state.Magazine <= 0)))
            throw new ArgumentOutOfRangeException(nameof(state), "Invalid action timer or unreachable insertion/chambering ammunition.");
    }

    private static bool Duration(int ticks) => ticks is >= 1 and <= MaxTicks;
}
