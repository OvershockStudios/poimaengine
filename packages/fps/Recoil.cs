// SPDX-License-Identifier: Apache-2.0
using System;

namespace Poima.Fps;

/// <summary>How a single accepted shot obtains its angular kick.</summary>
public enum RecoilMode
{
    Authored,
    Seeded,
    Hybrid
}

/// <summary>What an authored or hybrid profile does after its last authored shot.</summary>
public enum RecoilPatternEnd
{
    Repeat,
    Hold,
    Stop
}

/// <summary>
/// Stateless per-shot recoil. Vector axes mean pitch X, yaw Y and roll Z, all in radians.
/// Pattern entries are angular increments for individual accepted shots, not absolute orientations.
/// Callers retain their own primitive shot index and seed; this helper never advances either.
/// No mutable state, clock, global RNG, reflection, engine writes or normal-path allocations are used.
/// </summary>
/// <remarks>
/// Aim, visual weapon recoil and camera effects are independent consumers. Evaluate separate profiles
/// (and optionally separate stream IDs) for each. Do not add camera shake or weapon motion to a hit ray.
/// Applying a returned kick to gameplay aim is an explicit caller decision, never a side effect here.
/// Nested helper values are not a promise of supported Game&lt;TState&gt; saved layouts.
/// </remarks>
public static class Recoil
{
    /// <summary>Maximum number of samples in a bounded authored profile.</summary>
    public const int MaxPatternSamples = 256;

    /// <summary>Maximum absolute per-axis input and returned kick, in radians.</summary>
    public const double MaxKickRadians = Math.PI / 2.0;

    /// <summary>
    /// Returns one angular kick without modifying caller state. shotIndex is zero-based and identifies
    /// an accepted shot. Repeating the same inputs returns the same result, including at large indices.
    /// Authored uses only the pattern; Seeded uses only seeded variation; Hybrid adds the two.
    /// variationRadians is a nonnegative per-axis half-width, sampled uniformly in [-width, width).
    /// All supplied pattern samples and variation components are validated even when a mode ignores them.
    /// Empty patterns are accepted only by Seeded. Each input component and the combined result must
    /// lie within MaxKickRadians. Invalid input throws ArgumentOutOfRangeException; an out-of-domain
    /// combined result throws ArithmeticException. No saturation hides such errors.
    /// </summary>
    /// <remarks>
    /// Repeat wraps the pattern; Hold repeats its final sample; Stop returns a zero kick for the entire
    /// authored/hybrid profile after its final sample. Seeded ignores patternEnd after validating it.
    /// stream is an explicit independent variation selector, not an implicit mutable RNG stream.
    /// Integer mixing intentionally wraps modulo 2^64 and hashes no assets or external data.
    /// </remarks>
    public static Vector3d AngularKick(
        ReadOnlySpan<Vector3d> authoredPattern,
        ulong shotIndex,
        uint seed,
        Vector3d variationRadians,
        RecoilMode mode,
        RecoilPatternEnd patternEnd = RecoilPatternEnd.Repeat,
        uint stream = 0)
    {
        if (mode is not (RecoilMode.Authored or RecoilMode.Seeded or RecoilMode.Hybrid))
            throw new ArgumentOutOfRangeException(nameof(mode));
        if (patternEnd is not (RecoilPatternEnd.Repeat or RecoilPatternEnd.Hold or RecoilPatternEnd.Stop))
            throw new ArgumentOutOfRangeException(nameof(patternEnd));
        if (authoredPattern.Length > MaxPatternSamples || (authoredPattern.IsEmpty && mode != RecoilMode.Seeded))
            throw new ArgumentOutOfRangeException(nameof(authoredPattern), "Profile requires 1..256 samples; only Seeded accepts an empty pattern.");

        ValidateVariation(variationRadians.X);
        ValidateVariation(variationRadians.Y);
        ValidateVariation(variationRadians.Z);
        foreach (Vector3d sample in authoredPattern)
        {
            ValidateSample(sample.X);
            ValidateSample(sample.Y);
            ValidateSample(sample.Z);
        }

        Vector3d authored = default;
        if (mode != RecoilMode.Seeded)
        {
            int index;
            if (shotIndex < (ulong)authoredPattern.Length)
                index = (int)shotIndex;
            else if (patternEnd == RecoilPatternEnd.Stop)
                return default;
            else if (patternEnd == RecoilPatternEnd.Hold)
                index = authoredPattern.Length - 1;
            else
                index = (int)(shotIndex % (ulong)authoredPattern.Length);
            authored = authoredPattern[index];
        }
        if (mode == RecoilMode.Authored)
            return authored;

        ulong key = Mix(shotIndex ^ (((ulong)seed << 32) | stream));
        var random = new Vector3d(
            Variation(key, 0, variationRadians.X),
            Variation(key, 1, variationRadians.Y),
            Variation(key, 2, variationRadians.Z));
        if (mode == RecoilMode.Seeded)
            return random;

        double pitch = authored.X + random.X;
        double yaw = authored.Y + random.Y;
        double roll = authored.Z + random.Z;
        ValidateResult(pitch);
        ValidateResult(yaw);
        ValidateResult(roll);
        return new Vector3d(pitch, yaw, roll);
    }

    private static double Variation(ulong key, uint axis, double amplitude)
    {
        if (amplitude == 0.0)
            return 0.0;
        ulong bits = Mix(unchecked(key + (0x9E3779B97F4A7C15UL * ((ulong)axis + 1UL))));
        // The top 53 bits are exactly representable as double; the denominator is exactly 2^53.
        double unit = (double)(bits >> 11) * (1.0 / 9007199254740992.0);
        return ((unit * 2.0) - 1.0) * amplitude;
    }

    private static ulong Mix(ulong value)
    {
        unchecked
        {
            value += 0x9E3779B97F4A7C15UL;
            value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9UL;
            value = (value ^ (value >> 27)) * 0x94D049BB133111EBUL;
            return value ^ (value >> 31);
        }
    }

    private static void ValidateSample(double value)
    {
        if (!(value >= -MaxKickRadians && value <= MaxKickRadians))
            throw new ArgumentOutOfRangeException("authoredPattern", "Every angular sample must be finite and within the documented bound.");
    }

    private static void ValidateVariation(double value)
    {
        if (!(value >= 0.0 && value <= MaxKickRadians))
            throw new ArgumentOutOfRangeException("variationRadians", "Variation must be finite, nonnegative and within the documented bound.");
    }

    private static void ValidateResult(double value)
    {
        if (!(value >= -MaxKickRadians && value <= MaxKickRadians))
            throw new ArithmeticException("Combined recoil exceeds the finite angular domain; no output was published.");
    }
}
