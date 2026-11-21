// SPDX-License-Identifier: Apache-2.0
using Poima.Weather;

namespace Poima.Tests;

public struct ManagedWetnessState
{
    public long NextQuantum;
}

[GameModule("poima.test.managed-surface-wetness")]
public sealed class ManagedWetnessGame : Game<ManagedWetnessState>, ISurfaceWetnessGame
{
    private static readonly WeatherSchedule Forecast = new(6, 60, 0, new WeatherWeights(0, 0, 0, 1));

    public override void Initialize(ref ManagedWetnessState state) => state.NextQuantum = 58;

    public override void Tick(ref ManagedWetnessState state, GameContext context)
    {
        Span<uint> rain = stackalloc uint[1];
        rain[0] = Forecast.RainDepthNanometers(state.NextQuantum);
        context.AdvanceSurfaceWetness((ulong)state.NextQuantum, rain);
        ++state.NextQuantum;
    }
}
