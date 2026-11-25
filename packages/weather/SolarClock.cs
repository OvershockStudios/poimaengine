// SPDX-License-Identifier: Apache-2.0
namespace Poima.Weather;

public enum Season { Spring, Summer, Autumn, Winter }

/// <summary>Zero-based year, one-based day and milliseconds since local solar midnight.</summary>
public readonly record struct GameDate(long YearIndex, int DayOfYear, int MillisecondsOfDay);

public static class WeatherClock
{
    public const int DaysPerYear = 365;
    public const int MillisecondsPerDay = 86_400_000;
    public const long MaximumTimeMilliseconds = 10_000L * DaysPerYear * MillisecondsPerDay - 1;

    /// <summary>Converts elapsed game time into a repeating 365-day calendar.</summary>
    public static GameDate DateAt(long worldMilliseconds)
    {
        CheckTime(worldMilliseconds, nameof(worldMilliseconds));
        long day = worldMilliseconds / MillisecondsPerDay;
        return new(day / DaysPerYear, (int)(day % DaysPerYear) + 1,
            (int)(worldMilliseconds % MillisecondsPerDay));
    }

    internal static void CheckTime(long milliseconds, string argument)
    {
        if (milliseconds is < 0 or > MaximumTimeMilliseconds)
            throw new ArgumentOutOfRangeException(argument);
    }

    // The northern spring equinox is at local noon on day 80.
    internal static double AnnualPhase(GameDate date)
    {
        double fractionOfDay = date.MillisecondsOfDay / (double)MillisecondsPerDay;
        double phase = (date.DayOfYear - 80.5 + fractionOfDay) / DaysPerYear;
        return phase < 0 ? phase + 1 : phase;
    }
}

/// <summary>Unit direction from an observer toward the sun in Poima world axes. North is −Z and east is +X.</summary>
public readonly record struct SunDirection(double X, double Y, double Z);

/// <summary>Angles in radians. Azimuth increases from north toward east.</summary>
public readonly record struct SolarSample(GameDate Date, Season Season, SunDirection Direction,
    double ElevationRadians, double AzimuthRadians, double DeclinationRadians)
{
    public bool IsAboveHorizon => Direction.Y > 0;
}

public static class SolarClock
{
    /// <summary>Measures an approximate sun on a fixed-length game calendar using local solar time.</summary>
    public static SolarSample Sample(long worldMilliseconds, double latitudeDegrees, double axialTiltDegrees = 23.44)
    {
        if (!double.IsFinite(latitudeDegrees) || latitudeDegrees is < -90 or > 90)
            throw new ArgumentOutOfRangeException(nameof(latitudeDegrees));
        if (!double.IsFinite(axialTiltDegrees) || axialTiltDegrees is < 0 or > 90)
            throw new ArgumentOutOfRangeException(nameof(axialTiltDegrees));

        GameDate date = WeatherClock.DateAt(worldMilliseconds);
        double fractionOfDay = date.MillisecondsOfDay / (double)WeatherClock.MillisecondsPerDay;
        double seasonFraction = WeatherClock.AnnualPhase(date);
        Season season = (Season)(((int)(seasonFraction * 4) + (latitudeDegrees < 0 ? 2 : 0)) % 4);

        const double radiansPerDegree = Math.PI / 180;
        double latitude = latitudeDegrees * radiansPerDegree;
        double declination = axialTiltDegrees * radiansPerDegree * Math.Sin(Math.Tau * seasonFraction);
        double hourAngle = Math.Tau * (fractionOfDay - 0.5);
        double horizontal = Math.Cos(declination) * Math.Cos(hourAngle);
        double vertical = Math.Sin(declination);
        double sinLatitude = Math.Sin(latitude);
        double cosLatitude = Math.Cos(latitude);
        double east = -Math.Cos(declination) * Math.Sin(hourAngle);
        double north = cosLatitude * vertical - sinLatitude * horizontal;
        double up = sinLatitude * vertical + cosLatitude * horizontal;

        // At the zenith or nadir the direction remains usable and azimuth is arbitrary.
        double azimuth = Math.Abs(east) + Math.Abs(north) <= 1e-12 ? 0 : Math.Atan2(east, north);
        if (azimuth < 0) azimuth += Math.Tau;
        return new(date, season, new(east, up, -north), Math.Asin(Math.Clamp(up, -1, 1)), azimuth, declination);
    }
}
