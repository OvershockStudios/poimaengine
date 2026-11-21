// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using Poima;

namespace Poima.Tests;

public struct ManagedSurfaceMarkState
{
    public EntityId TemporarySource;
    public int Mode;
    public int Uses;
    public int Ticks;
    public long FailAt;
}

[GameModule("poima.test.managed-surface-mark")]
public sealed class ManagedSurfaceMarkGame : Game<ManagedSurfaceMarkState>, ISurfaceMarkGame, IParticleBurstGame
{
    private static readonly EntityId Player = new(0, 2);
    private static readonly EntityId Mover = new(0, 100);
    private static readonly EntityId Neighbor = new(0, 101);
    private static readonly EntityId Source = new(0, 120);
    private static readonly EntityId SeparateReceiver = new(0, 132);
    private static readonly TemplateId TemporarySource = new(0, 110);
    private static readonly Quaternion SourceRotation = new(0, MathF.Sqrt(.5f), 0, MathF.Sqrt(.5f));
    private static readonly Vector3d Forward = new(0, 0, -1);
    private static readonly SurfaceMarkStyle Green = new(new(.3125f), .125f, .8f, 0,
        new(.125f, .875f, .125f), .5f, .25f);
    private static readonly SurfaceMarkStyle Blue = new(new(.09375f), .125f, .8f, .5f,
        new(.125f, .125f, .875f), 1, .5f);
    private static readonly SurfaceMarkStyle Yellow = new(new(.09375f), .125f, .8f, .5f,
        new(.875f, .875f, .125f), 1, .375f);

    public override void Initialize(ref ManagedSurfaceMarkState state)
    {
        state = default;
        state.FailAt = -1;
    }

    public override void Tick(ref ManagedSurfaceMarkState state, GameContext context)
    {
        ++state.Ticks;
        if ((long)context.Tick == state.FailAt)
        {
            context.BurstParticles(Source, 1);
            context.PlaceSurfaceMark(Neighbor, new(-.25, 1, 0), Forward, 8, Blue);
            throw new InvalidOperationException("Surface mark fixture later-tick failure.");
        }
        if (!context.Pressed(Player, GameAction.Use))
            return;

        ++state.Uses;
        switch (state.Mode)
        {
            case 0:
                Deposit(ref state, context);
                break;
            case 1:
                context.BurstParticles(Source, 1);
                context.PlaceSurfaceMark(SeparateReceiver, new(10, 1, 0), Forward, 8, Blue);
                break;
            case 2:
                context.MoveKinematic(Source, new(-1.25, 1, -1), SourceRotation, 1);
                context.BurstParticles(Source, 1);
                context.PlaceSurfaceMark(Mover, new(.5, 0, .3125), new(1, 0, 0), 8, Green, Source);
                context.PlaceSurfaceMark(Mover, new(0, 1, 0), Forward, 8, Blue);
                break;
            default:
                throw new InvalidOperationException("Unknown surface mark fixture mode.");
        }
    }

    private static void Deposit(ref ManagedSurfaceMarkState state, GameContext context)
    {
        if (state.Uses == 1)
        {
            context.MoveKinematic(Source, new(-1.25, 1, -1), SourceRotation, 1);
            context.BurstParticles(Source, 2);
            context.PlaceSurfaceMark(Mover, new(.5, 0, .3125), new(1, 0, 0), 8, Green, Source);

            state.TemporarySource = context.Spawn(TemporarySource,
                new SpawnTransform(new(1, .75, -1), Quaternion.Identity, new(1, 1, 1)));
            context.MoveKinematic(state.TemporarySource, new(.25, .75, -1), Quaternion.Identity, 1);
            context.PlaceSurfaceMark(Neighbor, new(0, 0, -.5), Forward, 8, Blue, state.TemporarySource);
        }
        else
        {
            context.BurstParticles(Source, 1);
            context.PlaceSurfaceMark(Mover, new(-2.6875, 1.25, 0), Forward, 8, Blue);
            context.PlaceSurfaceMark(Mover, new(-2.125, .6875, 0), Forward, 8, Yellow);
        }
    }
}
