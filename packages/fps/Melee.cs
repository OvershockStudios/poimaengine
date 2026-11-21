// SPDX-License-Identifier: Apache-2.0
using System;

namespace Poima.Fps;

/// <summary>World-space endpoints of the physical blade segment, in metres.</summary>
public readonly record struct MeleeBladeSample(Vector3d Grip, Vector3d Tip);

/// <summary>Blade radius, maximum interval inflation in metres and per-sweep query budget.</summary>
public readonly record struct MeleeSweepProfile(double Radius, double MaxIntervalError, int MaxIntervals);

/// <summary>The original query contact and its mapped fraction over the whole sample pair.
/// A miss has fraction one. Queries counts submitted casts. MaxIntervalError is the planned geometric
/// inflation before rounding the radius to the native representation.</summary>
public readonly record struct MeleeSweepResult(CapsuleCastHit? Hit, double Fraction, int Queries, double MaxIntervalError);

/// <summary>A fixed-orientation capsule swept from Origin. Its cylinder lies along local Y.</summary>
public readonly record struct MeleeSweepQuery(
    Vector3d Origin, Quaterniond Rotation, double Radius, double HalfHeight, Vector3d Direction, double Distance);

/// <summary>Returns the first unignored contact of a single capsule query, including a blocker or overlap.
/// Fractions are in [0,1] and distances in [0,query.Distance]. An initial overlap starts at fraction
/// and distance zero. A provider that cannot complete the query must report an error.</summary>
public interface IMeleeQueries
{
    CapsuleCastHit? Sweep(in MeleeSweepQuery query);
}

/// <summary>Finds the first contact over a pair of consecutive logical blade samples.</summary>
/// <remarks>
/// The caller supplies the active window's samples and owns phase, contact consumption and damage policy.
/// Each endpoint follows its own linear interpolant. An interval sweeps the capsule aligned to the
/// interval midpoint's axis along the blade-centre chord. Its radius grows by one quarter of the
/// interval's blade-axis change, covering the change of direction or length in exact arithmetic.
/// The enclosure can contact early, including overlap at an interval's start. Identical blade axes
/// need only a translated capsule (or a stationary overlap), subject to the native travel limit.
/// Native orientation, travel and contact precision still apply. Supply more closely spaced logical
/// samples for curved motion between ticks. Obstacle poses remain fixed at the committed query state.
/// The earliest reporting interval wins, including a contact at its endpoint. Within an interval the
/// provider chooses the contact, including the native overlap-depth and entity/triangle tie rules.
/// </remarks>
public static class Melee
{
    private const double PositionLimit = 1e6;
    private const double TravelLimit = 10000;
    private const double RadiusLimit = 10;
    private const double HalfHeightLimit = 10;
    private const int TickQueryLimit = 32;

    /// <summary>Queries committed geometry during Tick with ICapsuleCastGame enabled.
    /// The native service validates the ignore list of at most 128 unique, live entities. Exclude the
    /// owner when needed and let other physics colliders stop the sweep. The caller budgets these and
    /// all other capsule attempts together.</summary>
    public static MeleeSweepResult Sweep(in MeleeSweepProfile profile, in MeleeBladeSample previous,
        in MeleeBladeSample current, GameContext context, ReadOnlySpan<EntityId> ignore = default)
    {
        var native = new NativeQueries(context, ignore);
        return Sweep(in profile, in previous, in current, ref native);
    }

    /// <summary>Queries through a caller-owned provider without delegate allocation or boxing.
    /// Invalid profiles or samples, as well as geometry that cannot fit the configured bounds, fail
    /// before any query. A native or custom provider error propagates to the caller.</summary>
    public static MeleeSweepResult Sweep<TQuery>(in MeleeSweepProfile profile, in MeleeBladeSample previous,
        in MeleeBladeSample current, ref TQuery query) where TQuery : IMeleeQueries, allows ref struct
    {
        if (!double.IsFinite(profile.Radius) || profile.Radius < .001 || profile.Radius > RadiusLimit ||
            !double.IsFinite(profile.MaxIntervalError) || profile.MaxIntervalError < 1e-6 || profile.MaxIntervalError > 1 ||
            profile.MaxIntervals < 1 || profile.MaxIntervals > TickQueryLimit)
            throw new ArgumentOutOfRangeException(nameof(profile), "Melee requires a radius of .001–10 m, interval error of 1e-6–1 m and 1–32 intervals.");

        Vector3d previousAxis = ValidateSample(in previous, nameof(previous));
        Vector3d currentAxis = ValidateSample(in current, nameof(current));
        Vector3d previousCentre = Midpoint(previous.Grip, previous.Tip);
        Vector3d currentCentre = Midpoint(current.Grip, current.Tip);
        double axisChange = Length(Subtract(currentAxis, previousAxis));
        double centreTravel = Length(Subtract(currentCentre, previousCentre));

        Span<MeleeSweepQuery> plan = stackalloc MeleeSweepQuery[profile.MaxIntervals];
        for (int count = 1; count <= plan.Length; count++)
        {
            double intervalError = axisChange / (4 * count);
            double radius = RoundedRadius(profile.Radius, intervalError);
            if (intervalError > profile.MaxIntervalError || radius > RadiusLimit || centreTravel / count > TravelLimit)
                continue;
            var intervals = plan[..count];
            if (!PlanIntervals(intervals, previousCentre, currentCentre, previousAxis, currentAxis, radius))
                continue;

            for (int i = 0; i < intervals.Length; i++)
            {
                if (query.Sweep(in intervals[i]) is not { } hit)
                    continue;
                if (hit.Entity == default || !double.IsFinite(hit.Fraction) || hit.Fraction < 0 || hit.Fraction > 1 ||
                    !double.IsFinite(hit.Distance) || hit.Distance < 0 || hit.Distance > intervals[i].Distance ||
                    (hit.InitialOverlap && (hit.Fraction != 0 || hit.Distance != 0)) ||
                    (intervals[i].Distance == 0 && !hit.InitialOverlap))
                    throw new InvalidOperationException("Melee provider returned invalid contact identity or travel data.");
                return new(hit, (i + hit.Fraction) / count, i + 1, intervalError);
            }
            return new(null, 1, count, intervalError);
        }
        throw new ArithmeticException("Melee motion cannot fit the configured error and interval budget within capsule query bounds.");
    }

    private static bool PlanIntervals(Span<MeleeSweepQuery> plan, Vector3d startCentre, Vector3d endCentre,
        Vector3d startAxis, Vector3d endAxis, double radius)
    {
        Vector3d origin = startCentre;
        for (int i = 0; i < plan.Length; i++)
        {
            Vector3d end = Interpolate(startCentre, endCentre, (i + 1.0) / plan.Length);
            Vector3d delta = Subtract(end, origin);
            double distance = Length(delta);
            Vector3d direction = distance == 0 ? new(0, 1, 0) : Divide(delta, distance);
            Vector3d axis = Interpolate(startAxis, endAxis, (i + .5) / plan.Length);
            double axisLength = Length(axis);
            double halfHeight = axisLength * .5;
            if (distance > TravelLimit || halfHeight > HalfHeightLimit || !WithinPosition(origin) ||
                !WithinPosition(TranslatedEnd(origin, direction, distance)))
                return false;
            plan[i] = new(origin, CapsuleRotation(axis, axisLength), radius, halfHeight, direction, distance);
            origin = end;
        }
        return true;
    }

    private static Vector3d ValidateSample(in MeleeBladeSample sample, string parameterName)
    {
        if (!WithinPosition(sample.Grip) || !WithinPosition(sample.Tip))
            throw new ArgumentOutOfRangeException(parameterName, "Blade endpoints must be finite and within ±1000 km on each axis.");
        Vector3d axis = Subtract(sample.Tip, sample.Grip);
        if (Length(axis) > 2 * HalfHeightLimit)
            throw new ArgumentOutOfRangeException(parameterName, "The blade segment must be at most 20 m long.");
        return axis;
    }

    private static double RoundedRadius(double bladeRadius, double intervalError)
    {
        double radius = bladeRadius + intervalError;
        // First keep the sum outward in double, then keep it outward in Jolt's float radius.
        if (radius - Math.Max(bladeRadius, intervalError) < Math.Min(bladeRadius, intervalError))
            radius = Math.BitIncrement(radius);
        float nativeRadius = (float)radius;
        return nativeRadius < radius ? MathF.BitIncrement(nativeRadius) : nativeRadius;
    }

    private static Quaterniond CapsuleRotation(Vector3d axis, double length)
    {
        if (length == 0)
            return Quaterniond.Identity;

        // A capsule's axis is unsigned. Use the upper hemisphere so the rotation
        // from +Y never approaches the singular antiparallel case.
        Vector3d unitAxis = Divide(axis, axis.Y < 0 ? -length : length);
        double w = 1 + unitAxis.Y;
        double norm = Math.Sqrt(unitAxis.Z * unitAxis.Z + unitAxis.X * unitAxis.X + w * w);
        return new(unitAxis.Z / norm, 0, -unitAxis.X / norm, w / norm);
    }

    private static bool WithinPosition(Vector3d value) =>
        double.IsFinite(value.X) && Math.Abs(value.X) <= PositionLimit &&
        double.IsFinite(value.Y) && Math.Abs(value.Y) <= PositionLimit &&
        double.IsFinite(value.Z) && Math.Abs(value.Z) <= PositionLimit;

    private static Vector3d TranslatedEnd(Vector3d origin, Vector3d direction, double distance)
    {
        // Match the managed capsule service's normalization when checking its endpoint domain.
        double length = Math.Sqrt(direction.X * direction.X + direction.Y * direction.Y + direction.Z * direction.Z);
        return new(origin.X + direction.X / length * distance, origin.Y + direction.Y / length * distance,
            origin.Z + direction.Z / length * distance);
    }

    private static Vector3d Interpolate(Vector3d first, Vector3d second, double t) => t == 1 ? second : new(
        Math.FusedMultiplyAdd(second.X - first.X, t, first.X),
        Math.FusedMultiplyAdd(second.Y - first.Y, t, first.Y),
        Math.FusedMultiplyAdd(second.Z - first.Z, t, first.Z));

    private static Vector3d Midpoint(Vector3d a, Vector3d b) => new((a.X + b.X) * .5, (a.Y + b.Y) * .5, (a.Z + b.Z) * .5);
    private static Vector3d Subtract(Vector3d a, Vector3d b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    private static Vector3d Divide(Vector3d v, double divisor) => new(v.X / divisor, v.Y / divisor, v.Z / divisor);
    private static double Length(Vector3d v) => double.Hypot(double.Hypot(v.X, v.Y), v.Z);

    private readonly ref struct NativeQueries : IMeleeQueries
    {
        private readonly GameContext context;
        private readonly ReadOnlySpan<EntityId> ignore;

        public NativeQueries(GameContext context, ReadOnlySpan<EntityId> ignore)
        {
            this.context = context;
            this.ignore = ignore;
        }

        public CapsuleCastHit? Sweep(in MeleeSweepQuery query) => context.CapsuleCast(query.Origin, query.Rotation,
            query.Radius, query.HalfHeight, query.Direction, query.Distance, ignore);
    }
}
