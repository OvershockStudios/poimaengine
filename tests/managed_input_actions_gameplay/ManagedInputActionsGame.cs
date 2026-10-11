// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct ManagedInputActionsState
{
    public EntityId Player, Camera, Target;
    public long FailAt;
    public int Ticks, Shots, Hits, Ammo, Reloads, AdsTicks, FirePressed, FireReleased;
    public int AimPressed, AimReleased, AbsentTicks, TargetHeldTicks, Mode, LegacyUses;
}

[GameModule("poima.test.managed-input-actions")]
public sealed class ManagedInputActionsGame : Game<ManagedInputActionsState>, IInputActionsGame
{
    public override void Initialize(ref ManagedInputActionsState state)
    {
        state = default;
        state.Player = new(0, 100); state.Camera = new(0, 101); state.Target = new(0, 300);
        state.Ammo = 3; state.FailAt = -1;
    }

    public override void Tick(ref ManagedInputActionsState state, GameContext context)
    {
        ++state.Ticks;
        var fire = context.GetInputAction(state.Player, "fire");
        var aim = context.GetInputAction(state.Player, "aim");
        var reload = context.GetInputAction(state.Player, "reload");
        var target = context.GetInputAction(state.Target, "fire");
        if (!fire.Present) ++state.AbsentTicks;
        if (target.Held) ++state.TargetHeldTicks;
        if (fire.Pressed) ++state.FirePressed;
        if (fire.Released) ++state.FireReleased;
        if (aim.Pressed) ++state.AimPressed;
        if (aim.Released) ++state.AimReleased;
        if (aim.Held) ++state.AdsTicks;
        if (reload.Pressed) { ++state.Reloads; state.Ammo = 3; }
        if ((fire.Held || fire.Pressed) && state.Ammo > 0)
        {
            --state.Ammo; ++state.Shots;
            var camera = context.Get(state.Camera).Transform;
            var hit = context.Raycast(camera.Position, camera.Forward, 20, [state.Player]);
            if (hit is { } value && value.Entity == state.Target) ++state.Hits;
        }
        foreach (var input in context.Inputs) if (input.Pressed(GameAction.Use)) ++state.LegacyUses;
        if (state.Mode == 1) context.GetInputAction(state.Player, "Fire");
        if (state.Mode == 2) context.GetInputAction(new(0, 999999), "fire");
        if (state.Mode == 3)
            for (var i = 0; i < 1025; ++i) context.GetInputAction(state.Player, "fire");
        if ((long)context.Tick == state.FailAt)
            throw new InvalidOperationException("Input action later-tick rollback fixture");
    }
}
