// SPDX-License-Identifier: Apache-2.0
namespace Poima.Weather;

public enum WeatherKind { Clear, Overcast, Rain, Storm }

/// <summary>Relative integer weights per forecast slot. Set a mode's weight to zero to disable it.</summary>
public readonly record struct WeatherWeights(ushort Clear, ushort Overcast, ushort Rain, ushort Storm)
{
    public static WeatherWeights Temperate => new(45, 30, 20, 5);
    internal int Total => Clear + Overcast + Rain + Storm;
}

/// <summary>Cloud and wetness targets in [0, 1], rain in mm/h and horizontal wind in m/s along world +X and +Z.</summary>
public readonly record struct WeatherConditions(double CloudCover, double RainMillimetersPerHour,
    double WindX, double WindZ, double WetnessTarget);

public readonly record struct WeatherSample(long SlotIndex, WeatherKind PreviousKind, WeatherKind Kind,
    double Transition, WeatherConditions Conditions);

/// <summary>One storm strike with a stable slot identity, world time in milliseconds and strength in [0, 1].</summary>
public readonly record struct LightningStrike(long SlotIndex, long WorldMilliseconds, double Strength);

/// <summary>Local weather determined solely by authored settings and elapsed game time.</summary>
public sealed class WeatherSchedule
{
    public const int MinimumSlotSeconds = 60;
    public const int MaximumSlotSeconds = 86_400;
    public const int MaximumQuerySlots = 64;
    public ulong Seed { get; }
    public int SlotSeconds { get; }
    public int TransitionSeconds { get; }
    public WeatherWeights Weights { get; }

    private readonly long slotMilliseconds;
    private readonly long transitionMilliseconds;

    public WeatherSchedule(ulong seed, int slotSeconds, int transitionSeconds, WeatherWeights weights)
    {
        if (slotSeconds is < MinimumSlotSeconds or > MaximumSlotSeconds)
            throw new ArgumentOutOfRangeException(nameof(slotSeconds));
        if (transitionSeconds < 0 || transitionSeconds > slotSeconds / 2)
            throw new ArgumentOutOfRangeException(nameof(transitionSeconds));
        if (weights.Total == 0) throw new ArgumentException("Enable at least one weather mode.", nameof(weights));

        Seed = seed;
        SlotSeconds = slotSeconds;
        TransitionSeconds = transitionSeconds;
        Weights = weights;
        slotMilliseconds = 1000L * slotSeconds;
        transitionMilliseconds = 1000L * transitionSeconds;
    }

    public WeatherSample Sample(long worldMilliseconds)
    {
        WeatherClock.CheckTime(worldMilliseconds, nameof(worldMilliseconds));
        long slot = worldMilliseconds / slotMilliseconds;
        long elapsed = worldMilliseconds % slotMilliseconds;
        WeatherKind kind = KindAt(slot);
        WeatherKind previousKind = slot == 0 ? kind : KindAt(slot - 1);
        WeatherConditions target = ConditionsAt(slot, kind);
        if (slot == 0 || elapsed >= transitionMilliseconds)
            return new(slot, previousKind, kind, 1, target);

        double fraction = elapsed / (double)transitionMilliseconds;
        double blend = fraction * fraction * (3 - 2 * fraction);
        WeatherConditions previous = ConditionsAt(slot - 1, previousKind);
        return new(slot, previousKind, kind, blend, new(
            Lerp(previous.CloudCover, target.CloudCover, blend),
            Lerp(previous.RainMillimetersPerHour, target.RainMillimetersPerHour, blend),
            Lerp(previous.WindX, target.WindX, blend),
            Lerp(previous.WindZ, target.WindZ, blend),
            Lerp(previous.WetnessTarget, target.WetnessTarget, blend)));
    }

    /// <summary>Collects strikes in (afterExclusive, throughInclusive]. Use −1 before the first sample.</summary>
    /// <remarks>Scans up to 64 touched slots. A rejected request leaves the destination unchanged.</remarks>
    public int CollectLightning(long afterExclusive, long throughInclusive, Span<LightningStrike> destination)
    {
        WeatherClock.CheckTime(throughInclusive, nameof(throughInclusive));
        if (afterExclusive < -1 || afterExclusive > throughInclusive)
            throw new ArgumentOutOfRangeException(nameof(afterExclusive));
        if (afterExclusive == throughInclusive) return 0;

        long first = (afterExclusive + 1) / slotMilliseconds;
        long last = throughInclusive / slotMilliseconds;
        CheckSlotCount(first, last, nameof(throughInclusive));
        Span<LightningStrike> found = stackalloc LightningStrike[MaximumQuerySlots];
        int count = 0;
        for (long slot = first; slot <= last; ++slot)
        {
            if (KindAt(slot) != WeatherKind.Storm) continue;
            long minimum = Math.Max(slotMilliseconds / 4, transitionMilliseconds);
            long span = slotMilliseconds * 3 / 4 - minimum;
            long time = slot * slotMilliseconds + minimum + (long)(Roll(slot, 5) % (ulong)span);
            if (time > afterExclusive && time <= throughInclusive)
                found[count++] = new(slot, time, 0.65 + 0.35 * Fraction(slot, 6));
        }
        if (count > destination.Length)
            throw new ArgumentException("Destination is too short for the strikes in this time range.", nameof(destination));

        found[..count].CopyTo(destination);
        return count;
    }

    /// <summary>Integrates the scheduled precipitation in millimetres over [startInclusive, endExclusive).</summary>
    public double RainDepthMillimeters(long startInclusive, long endExclusive)
    {
        WeatherClock.CheckTime(startInclusive, nameof(startInclusive));
        WeatherClock.CheckTime(endExclusive, nameof(endExclusive));
        if (endExclusive < startInclusive) throw new ArgumentOutOfRangeException(nameof(endExclusive));
        if (endExclusive == startInclusive) return 0;

        long first = startInclusive / slotMilliseconds;
        long last = (endExclusive - 1) / slotMilliseconds;
        CheckSlotCount(first, last, nameof(endExclusive));
        double total = 0;
        for (long slot = first; slot <= last; ++slot)
        {
            long slotStart = slot * slotMilliseconds;
            long lower = Math.Max(startInclusive - slotStart, 0);
            long upper = Math.Min(endExclusive - slotStart, slotMilliseconds);
            double nextRain = RainAt(slot, KindAt(slot));
            if (slot > 0 && lower < transitionMilliseconds)
            {
                double previousRain = RainAt(slot - 1, KindAt(slot - 1));
                long transitionEnd = Math.Min(upper, transitionMilliseconds);
                double a = lower / (double)transitionMilliseconds;
                double b = transitionEnd / (double)transitionMilliseconds;
                double nextWeight = transitionMilliseconds * (IntegralOfBlend(b) - IntegralOfBlend(a));
                total += previousRain * (transitionEnd - lower - nextWeight) + nextRain * nextWeight;
                lower = transitionEnd;
            }
            total += nextRain * (upper - lower);
        }
        return total / 3_600_000;
    }

    private static double Lerp(double a, double b, double fraction) => a + (b - a) * fraction;
    private static double IntegralOfBlend(double fraction) => fraction * fraction * fraction * (1 - 0.5 * fraction);

    private static void CheckSlotCount(long first, long last, string argument)
    {
        if (last - first >= MaximumQuerySlots)
            throw new ArgumentOutOfRangeException(argument, "A query can touch at most 64 forecast slots.");
    }

    private WeatherKind KindAt(long slot)
    {
        int bucket = (int)(Roll(slot, 0) % (ulong)Weights.Total);
        if (bucket < Weights.Clear) return WeatherKind.Clear;
        if (bucket < Weights.Clear + Weights.Overcast) return WeatherKind.Overcast;
        if (bucket < Weights.Clear + Weights.Overcast + Weights.Rain) return WeatherKind.Rain;
        return WeatherKind.Storm;
    }

    private double RainAt(long slot, WeatherKind kind) => kind switch
    {
        WeatherKind.Rain => 4 + 6 * Fraction(slot, 1),
        WeatherKind.Storm => 16 + 12 * Fraction(slot, 1),
        _ => 0
    };

    private WeatherConditions ConditionsAt(long slot, WeatherKind kind)
    {
        double cloudVariation = Fraction(slot, 2);
        double speedVariation = Fraction(slot, 3);
        (double cloud, double speed, double wetness) = kind switch
        {
            WeatherKind.Clear => (0.03 + 0.17 * cloudVariation, 1.5 + 2.5 * speedVariation, 0),
            WeatherKind.Overcast => (0.60 + 0.30 * cloudVariation, 4 + 4 * speedVariation, 0.03),
            WeatherKind.Rain => (0.75 + 0.23 * cloudVariation, 7 + 5 * speedVariation, 0.65),
            _ => (0.92 + 0.08 * cloudVariation, 12 + 14 * speedVariation, 1)
        };
        double direction = Math.Tau * Fraction(slot, 4);
        return new(cloud, RainAt(slot, kind), speed * Math.Cos(direction), speed * Math.Sin(direction), wetness);
    }

    private double Fraction(long slot, uint channel) => (Roll(slot, channel) >> 11) * (1.0 / (1UL << 53));

    private ulong Roll(long slot, uint channel)
    {
        unchecked
        {
            // SplitMix64 finalizer applied to an address formed from the seed, slot and channel.
            ulong value = Seed + (((ulong)slot << 3) + channel + 1) * 0x9e3779b97f4a7c15UL;
            value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9UL;
            value = (value ^ (value >> 27)) * 0x94d049bb133111ebUL;
            return value ^ (value >> 31);
        }
    }
}
