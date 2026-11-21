// SPDX-License-Identifier: Apache-2.0
using Poima;

namespace Poima.Tests;

public struct ManagedTimedSurfaceMarkState
{
    public int Uses;
    public int Ticks;
    public int Sustained;
}

[GameModule("poima.test.managed-timed-surface-mark")]
public sealed class ManagedTimedSurfaceMarkGame : Game<ManagedTimedSurfaceMarkState>, ITimedSurfaceMarkGame
{
    private static readonly EntityId Player = new(0, 2);
    private static readonly EntityId Receiver = new(0, 100);
    private static readonly EntityId Terrain = new(0, 110);
    private static readonly EntityId MaskProfile = new(0, 132);
    private static readonly Vector3d StampTangent = new(1, 0, 0);
    private static readonly SurfaceMarkStyle Green = new(new(.125f, .09375f), .125f, .8f, .25f,
        new(.125f, .875f, .125f), 1, .5f);
    private static readonly SurfaceMarkStyle Purple = Green with { LinearColor = new(.625f, .125f, .875f) };
    private static readonly SurfaceMarkStyle Footprint = new(new(.25f, .125f), .03125f, .75f, .5f,
        new(.125f, .875f, .25f), .75f, .5f);

    public override void Initialize(ref ManagedTimedSurfaceMarkState state) => state = default;

    public override void Tick(ref ManagedTimedSurfaceMarkState state, GameContext context)
    {
        ++state.Ticks;
        if (state.Sustained != 0)
        {
            var slot = (state.Ticks - 1) % 30;
            if (slot < 20)
            {
                var x = 10.25 + .09375 * (slot % 4);
                context.PlaceTimedSurfaceMark(Terrain, new(x, 8, -4.375), new(0, -1, 0), 12,
                    Footprint, StampTangent, 8, SurfaceMarkOverflowPolicy.RecycleOldestTimed);
            }
            return;
        }

        if (!context.Pressed(Player, GameAction.Use))
            return;

        ++state.Uses;
        if (state.Uses == 1)
        {
            context.PlaceTimedSurfaceMark(Receiver, new(-2, .75, 0), new(0, 0, -1), 8,
                Green, StampTangent, 2, SurfaceMarkOverflowPolicy.RecycleOldestTimed, profile: MaskProfile);
        }
        else if (state.Uses == 2)
        {
            context.PlaceTimedSurfaceMark(Receiver, new(-2, .75, 0), new(0, 0, -1), 8,
                Purple, StampTangent, 3, SurfaceMarkOverflowPolicy.RecycleOldestTimed);
        }
    }
}
