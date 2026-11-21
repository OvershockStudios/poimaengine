// SPDX-License-Identifier: Apache-2.0
using Poima;
using Poima.Interaction;

namespace Poima.Tests;

public struct FocusedUseState
{
    public double Reach, Distance, UseDistance;
    public EntityId Hit, UseHit;
    public int Ticks, Status, UseStatus, UseEdges, Activations;
    public long FailAt;
}

[GameModule("poima.test.focused-use")]
public sealed class FocusedUseGame : Game<FocusedUseState>
{
    public override void Initialize(ref FocusedUseState state)
    {
        state.Reach = 4;
        state.FailAt = -1;
    }

    public override void Tick(ref FocusedUseState state, GameContext context)
    {
        EntityId actor = new(0, 100);
        EntityId camera = new(0, 101);
        var focus = FocusedUse.Probe(context, actor, camera, [new(0, 300), new(0, 301)], state.Reach);
        ++state.Ticks;
        state.Status = focus.Status switch
        {
            FocusStatus.Miss => 0,
            FocusStatus.Blocked => 1,
            FocusStatus.Target => 2,
            _ => throw new InvalidOperationException("Unknown focus status.")
        };
        state.Hit = focus.Hit?.Entity ?? default;
        state.Distance = focus.Hit?.Distance ?? 0;

        if (context.Pressed(actor, GameAction.Use))
        {
            ++state.UseEdges;
            state.UseStatus = state.Status;
            state.UseHit = state.Hit;
            state.UseDistance = state.Distance;
            if (focus.IsTarget)
                ++state.Activations;
        }

        if ((long)context.Tick == state.FailAt)
            throw new InvalidOperationException($"Focused Use rollback witness after {state.Activations} activations.");
    }
}
