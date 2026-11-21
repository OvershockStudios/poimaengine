// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using Poima;

namespace Poima.Tests;

public struct ManagedDirectionalSurfaceMarkState
{
    public EntityId TemporarySource;
    public int Uses;
    public int Ticks;
}

[GameModule("poima.test.managed-directional-surface-mark")]
public sealed class ManagedDirectionalSurfaceMarkGame : Game<ManagedDirectionalSurfaceMarkState>,
    ISurfaceMarkGame, IDirectionalSurfaceMarkGame
{
    private static readonly EntityId Player = new(0, 2);
    private static readonly EntityId Receiver = new(0, 100);
    private static readonly EntityId Neighbor = new(0, 101);
    private static readonly TemplateId SourceTemplate = new(0, 110);
    private static readonly Quaternion InitialSourceRotation = Quaternion.CreateFromAxisAngle(Vector3.UnitY, 75 * MathF.PI / 180);
    private static readonly Quaternion FinalSourceRotation = new(0, MathF.Sqrt(.5f), 0, MathF.Sqrt(.5f));
    private static readonly SurfaceMarkStyle Green = new(new(.25f, .0625f), .125f, .8f, .25f,
        new(.125f, .875f, .125f), .8125f, .5f);
    private static readonly SurfaceMarkStyle Gold = new(new(.1875f, .0625f), .125f, .8f, .25f,
        new(.875f, .625f, .125f), 1, .375f);

    public override void Initialize(ref ManagedDirectionalSurfaceMarkState state) => state = default;

    public override void Tick(ref ManagedDirectionalSurfaceMarkState state, GameContext context)
    {
        ++state.Ticks;
        if (!context.Pressed(Player, GameAction.Use))
            return;

        ++state.Uses;
        if (state.Uses == 1)
        {
            state.TemporarySource = context.Spawn(SourceTemplate, new SpawnTransform(
                new(-.35, 1.75, -1), InitialSourceRotation, new(1, 1, 1)));
            context.MoveKinematic(state.TemporarySource, new(-1.0625, 1.25, -1), FinalSourceRotation, 1);
            context.PlaceDirectionalSurfaceMark(Receiver, new(.5, 0, 0), new(1, 0, 0), 8,
                Green, new(2, 3, 4), state.TemporarySource);
            context.PlaceSurfaceMark(Neighbor, new(.25, .75, 0), new(0, 0, -1), 8, Gold);
        }
        else if (state.Uses == 2)
        {
            context.Despawn(state.TemporarySource);
            state.TemporarySource = default;
        }
    }
}
