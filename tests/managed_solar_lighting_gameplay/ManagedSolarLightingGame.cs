// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using Poima.Weather;

namespace Poima.Tests;

public struct ManagedSolarLightingState
{
    public int Ticks;
    public long FailAt;
}

[GameModule("poima.test.managed-solar-lighting")]
public sealed class ManagedSolarLightingGame : Game<ManagedSolarLightingState>, ISolarLightingGame
{
    public override void Initialize(ref ManagedSolarLightingState state) => state.FailAt = -1;

    public override void Tick(ref ManagedSolarLightingState state, GameContext context)
    {
        state.Ticks++;
        switch (context.Tick)
        {
        case 0:
            Stage(context, 25_200_000, .5f, new(1, .375f, .125f), .5f, new(.03125f, .0625f, .125f));
            break;
        case 1:
            Stage(context, 43_200_000, 1, Vector3.One, 1, new(.125f, .125f, .125f));
            break;
        case 3:
            Stage(context, 61_200_000, .5f, new(1, .5f, .25f), .375f, new(.0625f, .09375f, .1875f));
            break;
        case 4:
            Stage(context, 86_400_000, 0, new(.25f, .5f, 1), .0625f, new(.0078125f, .015625f, .03125f));
            break;
        }
        if ((long)context.Tick == state.FailAt)
            throw new InvalidOperationException("Solar lighting later-Tick rollback fixture.");
    }

    private static void Stage(GameContext context, long time, float sunGain, Vector3 sunTint,
                              float skyGain, Vector3 ambient)
    {
        var direction = SolarClock.Sample(time, latitudeDegrees: 30, axialTiltDegrees: 0).Direction;
        context.SetSolarLighting((ulong)time, new Vector3d(direction.X, direction.Y, direction.Z),
                                 sunGain, sunTint, skyGain, ambient);
    }
}
