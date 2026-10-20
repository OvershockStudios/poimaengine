// SPDX-License-Identifier: Apache-2.0
// Optional, stateless first-person feedback helpers: critically damped springs, look sway,
// distance-driven bob and seeded smooth noise shake. No engine or camera plumbing.

using System;

namespace Poima.Fps;

/// <summary>
/// Transient scalar spring value. Position is in caller units (metres or radians). Velocity is in
/// caller units per second. This is a convenience only and is not a supported saved layout. Callers
/// persist Position and Velocity as their own primitive double fields.
/// </summary>
public readonly record struct ScalarSpring(double Position, double Velocity);

/// <summary>Transient per-axis vector spring value. Axes are fully independent. Callers persist primitives.</summary>
public readonly record struct VectorSpring(Vector3d Position, Vector3d Velocity);

/// <summary>Bob target. Offsets are in metres and angles in radians. Each |value| is at most |amplitude| * envelope.</summary>
public readonly record struct BobTarget(double LateralMetres, double VerticalMetres, double RollRadians, double PitchRadians);

/// <summary>Exact integer noise cell plus its double-precision fraction in [0, 1).</summary>
public readonly record struct NoiseCoordinate(ulong Cell, double Fraction);

/// <summary>
/// Deterministic, allocation-free, stateless FPS feedback math.
/// Units: metres, radians, seconds. Velocities are in units per second.
/// Every function validates all inputs, including for no-op calls.
/// Invalid inputs throw ArgumentOutOfRangeException.
/// A non-finite intermediate, or an output outside the supported domain, throws ArithmeticException.
/// Results are returned whole, so a throw never leaves partially updated caller data.
/// No input is clamped or normalized. Output clamps enforce the documented artistic sway/noise bounds.
/// There is no static or instance mutable state, no Random, no clock, and no reflection.
/// Runtime Math.Exp/Sin are used; cross-platform bit-identical transcendental results are not promised.
/// </summary>
public static class Feedback
{
    /// <summary>Largest supported spring frequency in Hz. Frequency must lie in (0, MaxFrequencyHz].</summary>
    public const double MaxFrequencyHz = 1000.0;
    /// <summary>Largest supported step in seconds. dt must lie in [0, MaxStepSeconds].</summary>
    public const double MaxStepSeconds = 1.0;
    /// <summary>Supported bound on |position| and |target| for springs, in metres or radians.</summary>
    public const double MaxSpringPosition = 1.0e6;
    /// <summary>Supported bound on |velocity| and |impulse| for springs, in units per second.</summary>
    public const double MaxSpringVelocity = 1.0e9;
    /// <summary>Supported bound on |look delta| per sample, in radians.</summary>
    public const double MaxLookDeltaRadians = 1.0e3;
    /// <summary>Supported bound on |sway gain|. Gain is dimensionless (radians per radian).</summary>
    public const double MaxSwayGain = 1.0e3;
    /// <summary>Largest allowed artistic sway limit, in radians.</summary>
    public const double MaxSwayRadians = Math.PI;
    /// <summary>Largest committed planar travel per bob advance, in metres.</summary>
    public const double MaxBobStepMetres = 1.0e3;
    /// <summary>Largest bob rate, in cycles per metre.</summary>
    public const double MaxCyclesPerMetre = 100.0;
    /// <summary>Largest |bob offset amplitude|, in metres.</summary>
    public const double MaxBobMetres = 1.0;
    /// <summary>Largest |bob angle amplitude|, in radians.</summary>
    public const double MaxBobRadians = Math.PI / 4.0;
    /// <summary>Largest per-channel shake amplitude, in caller units (metres or radians).</summary>
    public const double MaxShakeAmplitude = 10.0;

    /// <summary>
    /// Advances a critically damped spring toward a constant target using the exact analytic solution.
    /// omega = 2*pi*frequencyHz. If dtSeconds is 0, the input state is returned unchanged (bit-exact,
    /// including signed zeros) after validation. Outputs outside the supported domain throw.
    /// </summary>
    public static ScalarSpring Step(ScalarSpring state, double target, double frequencyHz, double dtSeconds)
    {
        double omega = ValidateStep(frequencyHz, dtSeconds);
        ValidateAxis(state.Position, state.Velocity, target);
        if (dtSeconds == 0.0)
        {
            return state;
        }

        (double x, double v) = Solve(state.Position, state.Velocity, target, omega, dtSeconds);
        return new ScalarSpring(x, v);
    }

    /// <summary>Per-axis analytic spring step. Axes are independent. dt 0 returns the input unchanged.</summary>
    public static VectorSpring Step(VectorSpring state, Vector3d target, double frequencyHz, double dtSeconds)
    {
        double omega = ValidateStep(frequencyHz, dtSeconds);
        ValidateAxis(state.Position.X, state.Velocity.X, target.X);
        ValidateAxis(state.Position.Y, state.Velocity.Y, target.Y);
        ValidateAxis(state.Position.Z, state.Velocity.Z, target.Z);
        if (dtSeconds == 0.0)
        {
            return state;
        }

        (double px, double vx) = Solve(state.Position.X, state.Velocity.X, target.X, omega, dtSeconds);
        (double py, double vy) = Solve(state.Position.Y, state.Velocity.Y, target.Y, omega, dtSeconds);
        (double pz, double vz) = Solve(state.Position.Z, state.Velocity.Z, target.Z, omega, dtSeconds);
        return new VectorSpring(new Vector3d(px, py, pz), new Vector3d(vx, vy, vz));
    }

    /// <summary>
    /// Adds an explicit velocity change, in units per second. Position is untouched. A zero delta returns
    /// the velocity bit-exact (this avoids -0 + 0 = +0). A result where |velocity| exceeds
    /// MaxSpringVelocity throws.
    /// </summary>
    public static ScalarSpring ApplyImpulse(ScalarSpring state, double deltaVelocity)
    {
        ValidateAxis(state.Position, state.Velocity, 0.0);
        return new ScalarSpring(state.Position, AddVelocity(state.Velocity, deltaVelocity));
    }

    /// <summary>Per-axis explicit velocity change, in units per second. See the scalar overload.</summary>
    public static VectorSpring ApplyImpulse(VectorSpring state, Vector3d deltaVelocity)
    {
        ValidateAxis(state.Position.X, state.Velocity.X, 0.0);
        ValidateAxis(state.Position.Y, state.Velocity.Y, 0.0);
        ValidateAxis(state.Position.Z, state.Velocity.Z, 0.0);
        var velocity = new Vector3d(
            AddVelocity(state.Velocity.X, deltaVelocity.X),
            AddVelocity(state.Velocity.Y, deltaVelocity.Y),
            AddVelocity(state.Velocity.Z, deltaVelocity.Z));
        return new VectorSpring(state.Position, velocity);
    }

    /// <summary>
    /// Look-delta sway target in radians: gain * lookDeltaRadians, then an intentional artistic clamp to
    /// [-maximumRadians, maximumRadians]. The input must be an actual look delta in radians, not raw mouse
    /// counts. Use a negative gain for sway that lags opposite to the look direction. The clamp is applied
    /// only after full validation, so it never hides invalid, non-finite or overflowing input.
    /// maximumRadians must lie in (0, MaxSwayRadians].
    /// </summary>
    public static double SwayTarget(double lookDeltaRadians, double gain, double maximumRadians)
    {
        RequireRange(lookDeltaRadians, -MaxLookDeltaRadians, MaxLookDeltaRadians, nameof(lookDeltaRadians));
        RequireRange(gain, -MaxSwayGain, MaxSwayGain, nameof(gain));
        if (!(maximumRadians > 0.0 && maximumRadians <= MaxSwayRadians))
        {
            throw new ArgumentOutOfRangeException(nameof(maximumRadians), maximumRadians, "Must be finite in (0, MaxSwayRadians].");
        }

        double raw = gain * lookDeltaRadians;
        RequireFinite(raw);
        return Math.Clamp(raw, -maximumRadians, maximumRadians);
    }

    /// <summary>Per-axis sway target. Each axis is saturated independently. See the scalar overload.</summary>
    public static Vector3d SwayTarget(Vector3d lookDeltaRadians, double gain, double maximumRadians)
    {
        double x = SwayTarget(lookDeltaRadians.X, gain, maximumRadians);
        double y = SwayTarget(lookDeltaRadians.Y, gain, maximumRadians);
        double z = SwayTarget(lookDeltaRadians.Z, gain, maximumRadians);
        return new Vector3d(x, y, z);
    }

    /// <summary>
    /// Advances bob phase by actual committed planar travel. The canonical phase is in cycles and must
    /// lie in [0, 1). distanceMetres must lie in [0, MaxBobStepMetres]. cyclesPerMetre must lie in
    /// [0, MaxCyclesPerMetre]. If the distance or the rate is zero, the phase is returned bit-exact after
    /// validation. Because there is no time input, a blocked player (zero committed travel) never advances.
    /// The result is wrapped into [0, 1).
    /// </summary>
    public static double AdvanceBobPhase(double phaseCycles, double distanceMetres, double cyclesPerMetre)
    {
        RequirePhase(phaseCycles, nameof(phaseCycles));
        RequireRange(distanceMetres, 0.0, MaxBobStepMetres, nameof(distanceMetres));
        RequireRange(cyclesPerMetre, 0.0, MaxCyclesPerMetre, nameof(cyclesPerMetre));
        if (distanceMetres == 0.0 || cyclesPerMetre == 0.0)
        {
            return phaseCycles;
        }

        double sum = phaseCycles + (distanceMetres * cyclesPerMetre);
        RequireFinite(sum);
        double wrapped = sum - Math.Floor(sum);
        if (!(wrapped >= 0.0 && wrapped < 1.0))
        {
            throw new ArithmeticException("Bob phase left the canonical interval [0, 1).");
        }

        return wrapped;
    }

    /// <summary>
    /// Evaluates the bob target from a phase in [0, 1), with a = 2*pi*phase:
    /// lateral = L*sin(a), vertical = V*sin(2a), roll = R*sin(a), pitch = P*sin(2a), each scaled by
    /// idleEnvelope. The caller supplies idleEnvelope in [0, 1]. A value of 0 yields a zero-magnitude
    /// (idle) target. Smoothing the envelope is the caller's responsibility. Amplitudes may be signed,
    /// with |metres| at most MaxBobMetres and |radians| at most MaxBobRadians.
    /// </summary>
    public static BobTarget EvaluateBob(double phaseCycles, double idleEnvelope, double lateralMetres, double verticalMetres, double rollRadians, double pitchRadians)
    {
        RequirePhase(phaseCycles, nameof(phaseCycles));
        RequireRange(idleEnvelope, 0.0, 1.0, nameof(idleEnvelope));
        RequireRange(lateralMetres, -MaxBobMetres, MaxBobMetres, nameof(lateralMetres));
        RequireRange(verticalMetres, -MaxBobMetres, MaxBobMetres, nameof(verticalMetres));
        RequireRange(rollRadians, -MaxBobRadians, MaxBobRadians, nameof(rollRadians));
        RequireRange(pitchRadians, -MaxBobRadians, MaxBobRadians, nameof(pitchRadians));
        double angle = 2.0 * Math.PI * phaseCycles;
        double once = Math.Sin(angle) * idleEnvelope;
        double twice = Math.Sin(2.0 * angle) * idleEnvelope;
        return new BobTarget(lateralMetres * once, verticalMetres * twice, rollRadians * once, pitchRadians * twice);
    }

    /// <summary>
    /// Maps an integer tick to a noise lattice coordinate using exact integer arithmetic:
    /// Cell = tick / ticksPerCell, and Fraction = (tick % ticksPerCell) / ticksPerCell, which lies in [0, 1).
    /// Integer cell/remainder do not lose large-tick bits; only the fractional division is rounded to double.
    /// </summary>
    public static NoiseCoordinate NoiseAt(ulong tick, uint ticksPerCell)
    {
        if (ticksPerCell == 0u)
        {
            throw new ArgumentOutOfRangeException(nameof(ticksPerCell), ticksPerCell, "Must be at least 1.");
        }

        return new NoiseCoordinate(tick / ticksPerCell, (double)(tick % ticksPerCell) / ticksPerCell);
    }

    /// <summary>
    /// Seeded smooth value-noise shake. X, Y and Z use independent lattice channels. Values are
    /// interpolated with the quintic 6t^5 - 15t^4 + 10t^3, which has zero first and second derivatives at
    /// lattice points. Each output component lies in [-amplitude, amplitude] for that axis. The caller
    /// chooses the units (metres or radians) per component. Each amplitude must lie in
    /// [0, MaxShakeAmplitude]. The output depends only on the coordinate, seed and amplitude.
    /// Cell + 1 wraps at ulong.MaxValue, deterministically.
    /// </summary>
    public static Vector3d Shake(NoiseCoordinate coordinate, uint seed, Vector3d amplitude)
    {
        RequirePhase(coordinate.Fraction, nameof(coordinate));
        RequireRange(amplitude.X, 0.0, MaxShakeAmplitude, nameof(amplitude));
        RequireRange(amplitude.Y, 0.0, MaxShakeAmplitude, nameof(amplitude));
        RequireRange(amplitude.Z, 0.0, MaxShakeAmplitude, nameof(amplitude));
        double t = coordinate.Fraction;
        double s = t * t * t * ((t * ((t * 6.0) - 15.0)) + 10.0);
        return new Vector3d(
            amplitude.X * Channel(seed, coordinate.Cell, 0u, s),
            amplitude.Y * Channel(seed, coordinate.Cell, 1u, s),
            amplitude.Z * Channel(seed, coordinate.Cell, 2u, s));
    }

    private static double Channel(uint seed, ulong cell, uint channel, double s)
    {
        double a = Lattice(seed, cell, channel);
        double b = Lattice(seed, unchecked(cell + 1UL), channel);
        // Guard against last-bit rounding only. Mathematically, a + (b - a) * s already lies in [-1, 1].
        return Math.Clamp(a + ((b - a) * s), -1.0, 1.0);
    }

    private static double Lattice(uint seed, ulong cell, uint channel)
    {
        ulong key = Mix64(((ulong)seed << 2) | channel);
        ulong h = Mix64(unchecked(cell + key));
        return ((double)(uint)(h >> 32) / uint.MaxValue * 2.0) - 1.0;
    }

    private static ulong Mix64(ulong z)
    {
        unchecked
        {
            z += 0x9E3779B97F4A7C15UL;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9UL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBUL;
            return z ^ (z >> 31);
        }
    }

    private static (double Position, double Velocity) Solve(double x, double v, double target, double omega, double dt)
    {
        // Exact critically damped solution about a constant target, evaluated as specified.
        double y = x - target;
        double j = v + (omega * y);
        double e = Math.Exp(-omega * dt);
        double newX = target + ((y + (j * dt)) * e);
        double newV = (v - (omega * j * dt)) * e;
        RequireFinite(y);
        RequireFinite(j);
        RequireFinite(e);
        RequireDomain(newX, MaxSpringPosition);
        RequireDomain(newV, MaxSpringVelocity);
        return (newX, newV);
    }

    private static double AddVelocity(double velocity, double deltaVelocity)
    {
        RequireRange(deltaVelocity, -MaxSpringVelocity, MaxSpringVelocity, nameof(deltaVelocity));
        if (deltaVelocity == 0.0)
        {
            return velocity;
        }

        double result = velocity + deltaVelocity;
        RequireDomain(result, MaxSpringVelocity);
        return result;
    }

    private static double ValidateStep(double frequencyHz, double dtSeconds)
    {
        if (!(frequencyHz > 0.0 && frequencyHz <= MaxFrequencyHz))
        {
            throw new ArgumentOutOfRangeException(nameof(frequencyHz), frequencyHz, "Must be finite in (0, MaxFrequencyHz].");
        }

        RequireRange(dtSeconds, 0.0, MaxStepSeconds, nameof(dtSeconds));
        return 2.0 * Math.PI * frequencyHz;
    }

    private static void ValidateAxis(double position, double velocity, double target)
    {
        RequireRange(position, -MaxSpringPosition, MaxSpringPosition, nameof(position));
        RequireRange(velocity, -MaxSpringVelocity, MaxSpringVelocity, nameof(velocity));
        RequireRange(target, -MaxSpringPosition, MaxSpringPosition, nameof(target));
    }

    private static void RequireRange(double value, double min, double max, string name)
    {
        if (!(value >= min && value <= max))
        {
            throw new ArgumentOutOfRangeException(name, value, "Must be finite and within the documented supported range.");
        }
    }

    private static void RequirePhase(double value, string name)
    {
        if (!(value >= 0.0 && value < 1.0))
        {
            throw new ArgumentOutOfRangeException(name, value, "Must be finite in [0, 1).");
        }
    }

    private static void RequireFinite(double value)
    {
        if (!double.IsFinite(value))
        {
            throw new ArithmeticException("Non-finite intermediate result.");
        }
    }

    private static void RequireDomain(double value, double bound)
    {
        if (!(Math.Abs(value) <= bound))
        {
            throw new ArithmeticException("Result is non-finite or outside the supported domain.");
        }
    }
}
