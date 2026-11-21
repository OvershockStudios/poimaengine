// SPDX-License-Identifier: Apache-2.0
namespace Poima.Weather;

/// <summary>Converts scheduled rain to fixed, replayable precipitation inputs.</summary>
public static class WeatherRainQuanta
{
    public const int MillisecondsPerQuantum = 1_000;
    public const uint MaximumRainNanometers = 1_000_000;
    public const long MaximumQuantumIndex = WeatherClock.MaximumTimeMilliseconds / MillisecondsPerQuantum - 1;

    private const int NanometersPerMillimeter = 1_000_000;

    /// <summary>Returns one elapsed second of scheduled rain in integer nanometers.</summary>
    /// <remarks>
    /// Quantum zero spans [0, 1000) game milliseconds. Only complete
    /// intervals through the supported scheduler endpoint are available.
    /// Integrate each quantum separately and round its depth once to the
    /// nearest nanometer, with midpoint ties away from zero. Query order,
    /// frame cadence and earlier queries have no effect on the result.
    /// Apply only completed intervals and save the next unapplied index.
    /// </remarks>
    public static uint RainDepthNanometers(this WeatherSchedule schedule, long quantumIndex)
    {
        ArgumentNullException.ThrowIfNull(schedule);
        if (quantumIndex is < 0 or > MaximumQuantumIndex)
            throw new ArgumentOutOfRangeException(nameof(quantumIndex));

        long start = quantumIndex * MillisecondsPerQuantum;
        long end = start + MillisecondsPerQuantum;
        double rounded = Math.Round(schedule.RainDepthMillimeters(start, end) * NanometersPerMillimeter,
            MidpointRounding.AwayFromZero);
        if (!(rounded >= 0 && rounded <= MaximumRainNanometers))
            throw new InvalidOperationException("Scheduled rain exceeds the fixed precipitation input range.");
        return (uint)rounded;
    }
}
