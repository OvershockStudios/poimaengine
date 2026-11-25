// SPDX-License-Identifier: Apache-2.0
namespace Poima.Weather;

/// <summary>Authored daily mean air and wet-bulb temperatures in degrees Celsius.</summary>
public readonly record struct SeasonalTemperature(double AirCelsius, double WetBulbCelsius);

public readonly record struct SeasonalClimateSample(
    SolarSample Solar, SeasonalTemperature Temperature, double SnowFraction);

/// <summary>Liquid precipitation and the liquid-water equivalent of snowfall.</summary>
public readonly record struct PrecipitationPhaseDepth(uint RainNanometers, uint SnowWaterEquivalentNanometers)
{
    public ulong TotalNanometers => (ulong)RainNanometers + SnowWaterEquivalentNanometers;
}

/// <summary>Samples a local seasonal climate on the solar clock and apportions scheduled precipitation.</summary>
public sealed class SeasonalClimate
{
    private readonly SeasonalTemperature[] normals;

    public double LatitudeDegrees { get; }
    public double DailyAmplitudeCelsius { get; }
    public double FullSnowWetBulbCelsius { get; }
    public double FullRainWetBulbCelsius { get; }

    /// <param name="latitudeDegrees">Local latitude. The southern year is offset by half a year.</param>
    /// <param name="spring">Daily mean temperatures at mid-spring.</param>
    /// <param name="summer">Daily mean temperatures at mid-summer.</param>
    /// <param name="autumn">Daily mean temperatures at mid-autumn.</param>
    /// <param name="winter">Daily mean temperatures at mid-winter.</param>
    /// <param name="dailyAmplitudeCelsius">Degrees above the mean at 15:00 local solar time.</param>
    /// <param name="fullSnowWetBulbCelsius">At or below this temperature all precipitation is snow.</param>
    /// <param name="fullRainWetBulbCelsius">At or above this temperature all precipitation is rain.</param>
    public SeasonalClimate(double latitudeDegrees, SeasonalTemperature spring, SeasonalTemperature summer,
        SeasonalTemperature autumn, SeasonalTemperature winter, double dailyAmplitudeCelsius = 0,
        double fullSnowWetBulbCelsius = -1, double fullRainWetBulbCelsius = 2)
    {
        if (!double.IsFinite(latitudeDegrees) || latitudeDegrees is < -90 or > 90)
            throw new ArgumentOutOfRangeException(nameof(latitudeDegrees));
        ValidateNormal(spring, nameof(spring));
        ValidateNormal(summer, nameof(summer));
        ValidateNormal(autumn, nameof(autumn));
        ValidateNormal(winter, nameof(winter));
        if (!double.IsFinite(dailyAmplitudeCelsius) || dailyAmplitudeCelsius is < 0 or > 50)
            throw new ArgumentOutOfRangeException(nameof(dailyAmplitudeCelsius));
        if (!ValidTemperature(fullSnowWetBulbCelsius))
            throw new ArgumentOutOfRangeException(nameof(fullSnowWetBulbCelsius));
        if (!ValidTemperature(fullRainWetBulbCelsius) || fullRainWetBulbCelsius <= fullSnowWetBulbCelsius)
            throw new ArgumentOutOfRangeException(nameof(fullRainWetBulbCelsius));

        LatitudeDegrees = latitudeDegrees;
        DailyAmplitudeCelsius = dailyAmplitudeCelsius;
        FullSnowWetBulbCelsius = fullSnowWetBulbCelsius;
        FullRainWetBulbCelsius = fullRainWetBulbCelsius;
        normals = [spring, summer, autumn, winter];
    }

    /// <summary>Samples the local phase, interpolated temperatures and solar data at one game time.</summary>
    public SeasonalClimateSample Sample(long worldMilliseconds)
    {
        SolarSample sun = SolarClock.Sample(worldMilliseconds, LatitudeDegrees);
        SeasonalTemperature temperature = TemperatureAt(sun.Date);
        return new(sun, temperature, SnowFractionAt(temperature.WetBulbCelsius));
    }

    /// <summary>Partitions the already rounded scheduled depth of a completed one-second quantum.</summary>
    /// <remarks>
    /// The phase uses the quantum's midpoint temperature. Round the snow portion once, with
    /// midpoint ties away from zero. The remaining nanometres are rain, conserving the input
    /// depth exactly. Save the next quantum index to continue without duplicating either phase.
    /// </remarks>
    public PrecipitationPhaseDepth Partition(WeatherSchedule schedule, long quantumIndex)
    {
        uint total = schedule.RainDepthNanometers(quantumIndex);
        if (total == 0)
            return default;
        long midpoint = quantumIndex * WeatherRainQuanta.MillisecondsPerQuantum +
            WeatherRainQuanta.MillisecondsPerQuantum / 2;
        SeasonalTemperature temperature = TemperatureAt(WeatherClock.DateAt(midpoint));
        uint snow = (uint)Math.Round(total * SnowFractionAt(temperature.WetBulbCelsius),
            MidpointRounding.AwayFromZero);
        return new(total - snow, snow);
    }

    private SeasonalTemperature TemperatureAt(GameDate date)
    {
        // Anchor each normal at its hemisphere's season midpoint.
        double position = WeatherClock.AnnualPhase(date) * 4 + (LatitudeDegrees < 0 ? 2 : 0) - .5;
        int previous = (int)Math.Floor(position);
        var from = normals[(previous + 4) % 4];
        var to = normals[(previous + 5) % 4];
        double weight = Smooth(position - previous);
        double day = date.MillisecondsOfDay / (double)WeatherClock.MillisecondsPerDay;
        double swing = DailyAmplitudeCelsius * Math.Cos(Math.Tau * (day - 15d / 24));
        return new(from.AirCelsius + (to.AirCelsius - from.AirCelsius) * weight + swing,
            from.WetBulbCelsius + (to.WetBulbCelsius - from.WetBulbCelsius) * weight + swing);
    }

    private double SnowFractionAt(double temperature) =>
        1 - Smooth(Math.Clamp((temperature - FullSnowWetBulbCelsius) /
            (FullRainWetBulbCelsius - FullSnowWetBulbCelsius), 0, 1));

    private static double Smooth(double fraction) => fraction * fraction * (3 - 2 * fraction);

    private static bool ValidTemperature(double value) => double.IsFinite(value) && value is >= -120 and <= 100;

    private static void ValidateNormal(SeasonalTemperature normal, string argument)
    {
        if (!ValidTemperature(normal.AirCelsius) || !ValidTemperature(normal.WetBulbCelsius) ||
            normal.WetBulbCelsius > normal.AirCelsius)
            throw new ArgumentOutOfRangeException(argument,
                "Temperatures must be finite and within [-120, 100] Celsius. Wet bulb must not exceed air temperature.");
    }
}
