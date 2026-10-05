// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Examples;

public struct AnimationGameState
{
    public EntityId Player, Rig;
    public int NextClip, Changes, ObservedClip;
    public double ObservedTime, BlendWeight;
}

// Instantiate an animated model as entity 100, with at least two clips, and
// assign the player controller to entity 10. These IDs can also be edited
// through runtime.gameplay.edit before stepping the loaded module.
[GameModule("poima.example.animation-switching")]
public sealed class AnimationGame : Game<AnimationGameState>
{
    public override void Initialize(ref AnimationGameState state)
    {
        state.Player = new(0, 10);
        state.Rig = new(0, 100);
        state.NextClip = 0;
    }

    public override void Tick(ref AnimationGameState state, GameContext context)
    {
        var animation = context.GetAnimation(state.Rig);
        if (animation is null) return;
        state.ObservedClip = animation.Value.Clip ?? -1;
        state.ObservedTime = animation.Value.Time;
        state.BlendWeight = animation.Value.Transition?.Weight ?? 1;
        if (!context.Pressed(state.Player, GameAction.Use)) return;

        // Thirty fixed ticks give a half-second fade. Repeated Use interrupts
        // an existing fade smoothly from its currently evaluated local pose.
        context.SetAnimation(state.Rig, state.NextClip, blendTicks: 30);
        state.NextClip = 1 - state.NextClip;
        ++state.Changes;
        // Queries in this Tick still describe the state before this queued
        // command. The command becomes visible after Tick returns successfully.
    }
}
