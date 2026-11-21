// SPDX-License-Identifier: Apache-2.0
namespace Poima.Fps;

/// <summary>Distances are metres. Height is the full height of the upright query capsule.</summary>
public readonly record struct TraversalProfile(double Radius, double Height, double Reach, double MinimumRise,
    double MaximumRise, double VaultDepth, double MaximumDrop, double MaximumSlopeDegrees, uint DurationTicks);

public enum TraversalKind { Mantle, Vault }
public enum TraversalPhase { Lift, Cross, Land, Complete }
public enum TraversalFailure { None, MissingFace, MissingTop, MissingLanding, UnsupportedLanding, TooFast, Blocked, PositionChanged }

/// <summary>Feet-space trajectory. Each segment is swept during planning and sampled in fixed ticks.</summary>
public readonly struct TraversalPlan
{
    internal TraversalPlan(TraversalKind kind, in TraversalProfile profile, Vector3d start, Vector3d raised,
        Vector3d over, Vector3d landing, double obstacleTop, EntityId obstacle, EntityId support, uint liftTicks, uint crossTicks)
    {
        Kind = kind;
        Profile = profile;
        Start = start;
        Raised = raised;
        Over = over;
        Landing = landing;
        ObstacleTop = obstacleTop;
        Obstacle = obstacle;
        Support = support;
        LiftTicks = liftTicks;
        CrossTicks = crossTicks;
    }

    public TraversalKind Kind { get; }
    public TraversalProfile Profile { get; }
    public Vector3d Start { get; }
    public Vector3d Raised { get; }
    public Vector3d Over { get; }
    public Vector3d Landing { get; }
    public double ObstacleTop { get; }
    public EntityId Obstacle { get; }
    public EntityId Support { get; }
    public uint LiftTicks { get; }
    public uint CrossTicks { get; }
    public uint DurationTicks => Profile.DurationTicks;
}

/// <summary>A refused attempt reports the relevant collider when one was observed.</summary>
public readonly record struct TraversalAttempt(TraversalPlan? Plan, TraversalFailure Failure, EntityId Collider);
public readonly record struct TraversalSample(Vector3d Feet, TraversalPhase Phase);
public readonly record struct TraversalAdvance(TraversalSample Sample, TraversalFailure Failure, EntityId Collider);

/// <summary>Committed world queries. Static means the collider is eligible as a fixed traversal support.</summary>
public interface ITraversalQueries
{
    RayHit? Ray(Vector3d origin, Vector3d direction, double distance);
    CapsuleCastHit? Sweep(Vector3d center, double radius, double halfHeight, Vector3d direction, double distance);
    bool IsStatic(EntityId entity);
}

/// <summary>Plans and rechecks collision-aware mantle and vault motion over fixed geometry.</summary>
public static class Traversal
{
    private const double Skin = .025;
    private const double PositionTolerance = .03;
    private const double PositionBound = 999_980;
    private static readonly Vector3d Down = new(0, -1, 0);

    /// <summary>Use from a Tick callback in a game declaring ICapsuleCastGame. Ignore the moving actor.</summary>
    public static TraversalAttempt Plan(in TraversalProfile profile, TraversalKind kind, Vector3d feet,
        Vector3d forward, GameContext context, ReadOnlySpan<EntityId> ignore = default)
    {
        var queries = new NativeQueries(context, ignore);
        return Plan(in profile, kind, feet, forward, ref queries);
    }

    /// <summary>Finds a fixed ledge, confirms the landing footprint and sweeps the upright capsule along the full path.</summary>
    public static TraversalAttempt Plan<TQuery>(in TraversalProfile profile, TraversalKind kind, Vector3d feet,
        Vector3d forward, ref TQuery queries) where TQuery : ITraversalQueries, allows ref struct
    {
        Validate(in profile, kind, feet, forward);
        double horizontalLength = Math.Sqrt(forward.X * forward.X + forward.Z * forward.Z);
        var heading = new Vector3d(forward.X / horizontalLength, 0, forward.Z / horizontalLength);
        var side = new Vector3d(heading.Z, 0, -heading.X);
        double minimumUp = Math.Cos(profile.MaximumSlopeDegrees * Math.PI / 180);
        var face = queries.Ray(Raise(feet, profile.MinimumRise * .5), heading, profile.Reach);
        if (face is not { Normal: { } faceNormal } front || !ValidRay(front, profile.Reach) ||
            front.Distance < Skin || Math.Abs(faceNormal.Y) > .5 || Dot(faceNormal, heading) > -.5 || !queries.IsStatic(front.Entity))
        {
            return Refuse(TraversalFailure.MissingFace, face?.Entity ?? default);
        }

        var topOrigin = Raise(Along(feet, heading, front.Distance + Skin * 2), profile.MaximumRise + Skin * 2);
        double ledgeRange = profile.MaximumRise - profile.MinimumRise + Skin * 2;
        var top = queries.Ray(topOrigin, Down, ledgeRange);
        if (!Walkable(top, ledgeRange, minimumUp) || top!.Value.Entity != front.Entity)
        {
            return Refuse(TraversalFailure.MissingTop, top?.Entity ?? default);
        }

        double topY = top!.Value.Position.Y;
        if (topY - feet.Y < profile.MinimumRise - 1e-5 || topY - feet.Y > profile.MaximumRise + 1e-5)
        {
            return Refuse(TraversalFailure.MissingTop, front.Entity);
        }

        double beyondFace = kind == TraversalKind.Mantle ? profile.Radius + Skin * 2 : profile.VaultDepth;
        var landingHorizontal = Along(feet, heading, front.Distance + beyondFace);
        var landingProbe = AtHeight(landingHorizontal, topY + Skin * 3);
        double landingRayDistance = kind == TraversalKind.Mantle
            ? Skin * 6
            : landingProbe.Y - feet.Y + profile.MaximumDrop;
        var landingHit = queries.Ray(landingProbe, Down, landingRayDistance);
        if (!Walkable(landingHit, landingRayDistance, minimumUp))
        {
            return Refuse(TraversalFailure.MissingLanding, landingHit?.Entity ?? default);
        }

        var surface = landingHit!.Value;
        var landing = AtHeight(landingHorizontal, surface.Position.Y);
        bool onTop = surface.Entity == front.Entity && Math.Abs(landing.Y - topY) <= Skin * 2;
        bool beyondTop = landing.Y < topY - Skin * 4 && landing.Y >= feet.Y - profile.MaximumDrop - 1e-5;
        if ((kind == TraversalKind.Mantle && !onTop) || (kind == TraversalKind.Vault && !beyondTop) ||
            !queries.IsStatic(surface.Entity) ||
            !Supported(in profile, landing, heading, side, surface.Entity, minimumUp, ref queries))
        {
            return Refuse(TraversalFailure.UnsupportedLanding, surface.Entity);
        }

        var plan = MakePlan(in profile, kind, feet, landing, topY, front.Entity, surface.Entity);
        if (!FitsNativeMove(in plan)) return Refuse(TraversalFailure.TooFast, default);
        var obstruction = Sweep(in profile, feet, plan.Raised, ref queries) ?? Sweep(in profile, plan.Raised, plan.Over, ref queries) ??
            Sweep(in profile, plan.Over, landing, ref queries);
        if (obstruction is { } blocked)
        {
            return Refuse(TraversalFailure.Blocked, blocked.Entity);
        }

        return new(plan, TraversalFailure.None, default);
    }

    /// <summary>Reconstructs the sampled path from saved operands. Advance still checks the live geometry.</summary>
    public static TraversalPlan Restore(in TraversalProfile profile, TraversalKind kind, Vector3d start, Vector3d landing,
        double obstacleTop, EntityId obstacle, EntityId support)
    {
        var crossing = new Vector3d(landing.X - start.X, 0, landing.Z - start.Z);
        Validate(in profile, kind, start, crossing);
        double depth = kind == TraversalKind.Mantle ? profile.Radius + Skin * 2 : profile.VaultDepth;
        bool validLanding = kind == TraversalKind.Mantle
            ? obstacle == support && Math.Abs(landing.Y - obstacleTop) <= Skin * 2
            : landing.Y < obstacleTop - Skin * 4 && landing.Y >= start.Y - profile.MaximumDrop - 1e-5;
        if (!Bounded(landing, 1e6 - 5) || obstacle == default || support == default || !validLanding ||
            !InRange(obstacleTop - start.Y, profile.MinimumRise - 1e-5, profile.MaximumRise + 1e-5) ||
            !InRange(Length(crossing), depth + Skin - 1e-7, depth + profile.Reach + 1e-7))
            throw new ArgumentException("Saved traversal operands do not fit the traversal profile.");
        var plan = MakePlan(in profile, kind, start, landing, obstacleTop, obstacle, support);
        if (!FitsNativeMove(in plan)) throw new ArgumentException("Saved traversal moves too quickly for native kinematic targets.");
        return plan;
    }

    /// <summary>Samples the planned feet pose. Position and velocity join continuously between segments.</summary>
    public static TraversalSample Sample(in TraversalPlan plan, uint elapsedTicks)
    {
        if (plan.DurationTicks == 0 || elapsedTicks > plan.DurationTicks)
        {
            throw new ArgumentOutOfRangeException(nameof(elapsedTicks));
        }

        uint crossEnd = plan.LiftTicks + plan.CrossTicks;
        if (elapsedTicks == plan.DurationTicks) return new(plan.Landing, TraversalPhase.Complete);
        if (elapsedTicks < plan.LiftTicks)
            return new(Ease(plan.Start, plan.Raised, elapsedTicks, plan.LiftTicks), TraversalPhase.Lift);
        if (elapsedTicks < crossEnd)
            return new(Ease(plan.Raised, plan.Over, elapsedTicks - plan.LiftTicks, plan.CrossTicks), TraversalPhase.Cross);
        return new(Ease(plan.Over, plan.Landing, elapsedTicks - crossEnd, plan.DurationTicks - crossEnd), TraversalPhase.Land);
    }

    /// <summary>Check just before applying the next fixed tick. Never submits a movement command.</summary>
    public static TraversalAdvance Advance(in TraversalPlan plan, uint elapsedTicks, Vector3d observedFeet,
        GameContext context, ReadOnlySpan<EntityId> ignore = default)
    {
        var queries = new NativeQueries(context, ignore);
        return Advance(in plan, elapsedTicks, observedFeet, ref queries);
    }

    /// <summary>Rejects unexpected actor displacement, new blockers and a missing final support.</summary>
    public static TraversalAdvance Advance<TQuery>(in TraversalPlan plan, uint elapsedTicks, Vector3d observedFeet,
        ref TQuery queries) where TQuery : ITraversalQueries, allows ref struct
    {
        if (!Bounded(observedFeet, 1e6 - 5)) throw new ArgumentOutOfRangeException(nameof(observedFeet));
        var current = Sample(in plan, elapsedTicks);
        if (Length(Subtract(observedFeet, current.Feet)) > PositionTolerance)
            return new(current, TraversalFailure.PositionChanged, default);
        if (elapsedTicks == plan.DurationTicks) return new(current, TraversalFailure.None, default);

        var next = Sample(in plan, elapsedTicks + 1);
        var profile = plan.Profile;
        if (Sweep(in profile, observedFeet, next.Feet, ref queries) is { } blocked)
            return new(current, TraversalFailure.Blocked, blocked.Entity);
        if (next.Phase == TraversalPhase.Complete)
        {
            var crossing = Subtract(plan.Over, plan.Raised);
            var heading = Scale(crossing, 1 / Length(crossing));
            var side = new Vector3d(heading.Z, 0, -heading.X);
            double minimumUp = Math.Cos(profile.MaximumSlopeDegrees * Math.PI / 180);
            if (!queries.IsStatic(plan.Support) ||
                !Supported(in profile, plan.Landing, heading, side, plan.Support, minimumUp, ref queries))
                return new(current, TraversalFailure.UnsupportedLanding, plan.Support);
        }
        return new(next, TraversalFailure.None, default);
    }

    private static bool Supported<TQuery>(in TraversalProfile profile, Vector3d feet, Vector3d heading, Vector3d side,
        EntityId support, double minimumUp, ref TQuery queries) where TQuery : ITraversalQueries, allows ref struct
    {
        double offset = profile.Radius * .55;
        double tolerance = Skin * 2 + offset * Math.Tan(profile.MaximumSlopeDegrees * Math.PI / 180);
        Span<Vector3d> samples = stackalloc Vector3d[5]
        {
            feet, Along(feet, heading, offset), Along(feet, heading, -offset),
            Along(feet, side, offset), Along(feet, side, -offset)
        };
        foreach (var sample in samples)
        {
            var hit = queries.Ray(Raise(sample, tolerance), Down, 2 * tolerance);
            if (!Walkable(hit, 2 * tolerance, minimumUp) || hit!.Value.Entity != support ||
                Math.Abs(hit.Value.Position.Y - feet.Y) > tolerance + 1e-5) return false;
        }
        return true;
    }

    private static CapsuleCastHit? Sweep<TQuery>(in TraversalProfile profile, Vector3d from, Vector3d to,
        ref TQuery queries) where TQuery : ITraversalQueries, allows ref struct
    {
        var delta = Subtract(to, from);
        double length = Length(delta);
        var direction = length > 0 ? Scale(delta, 1 / length) : new Vector3d(0, 1, 0);
        var center = Raise(from, profile.Height * .5 + Skin);
        return queries.Sweep(center, profile.Radius, profile.Height * .5 - profile.Radius, direction, length);
    }

    private static TraversalPlan MakePlan(in TraversalProfile profile, TraversalKind kind, Vector3d start,
        Vector3d landing, double topY, EntityId obstacle, EntityId support)
    {
        double raisedY = Math.Max(topY, landing.Y) + 2 * Skin;
        var raised = AtHeight(start, raisedY);
        var over = AtHeight(landing, raisedY);
        double rise = raised.Y - start.Y;
        double drop = over.Y - landing.Y;
        double travel = rise + Length(Subtract(over, raised)) + drop;
        uint unassigned = profile.DurationTicks - 3;
        uint liftTicks = 1 + (uint)Math.Floor(unassigned * rise / travel);
        uint landTicks = 1 + (uint)Math.Floor(unassigned * drop / travel);
        return new(kind, in profile, start, raised, over, landing, topY, obstacle, support,
            liftTicks, profile.DurationTicks - liftTicks - landTicks);
    }

    private static bool FitsNativeMove(in TraversalPlan plan)
    {
        // Smoothstep's maximum derivative is 1.5. Keep headroom below the native 100 m/s limit.
        const double distancePerTick = 95.0 / (60 * 1.5);
        uint landTicks = plan.DurationTicks - plan.LiftTicks - plan.CrossTicks;
        return plan.Raised.Y - plan.Start.Y <= plan.LiftTicks * distancePerTick &&
            Length(Subtract(plan.Over, plan.Raised)) <= plan.CrossTicks * distancePerTick &&
            plan.Over.Y - plan.Landing.Y <= landTicks * distancePerTick;
    }

    private static bool ValidRay(in RayHit hit, double maximum) => hit.Entity != default &&
        InRange(hit.Distance, 0, maximum) && Bounded(hit.Position, 1e6) && hit.Normal is { } normal &&
        Bounded(normal, 1.00001) && Math.Abs(Dot(normal, normal) - 1) <= 1e-4;

    private static bool Walkable(RayHit? hit, double maximum, double minimumUp) =>
        hit is { Normal: { } normal } contact && ValidRay(in contact, maximum) && normal.Y >= minimumUp;

    private static TraversalAttempt Refuse(TraversalFailure failure, EntityId collider) => new(null, failure, collider);

    private static void Validate(in TraversalProfile profile, TraversalKind kind, Vector3d feet, Vector3d forward)
    {
        if (kind is not (TraversalKind.Mantle or TraversalKind.Vault)) throw new ArgumentOutOfRangeException(nameof(kind));
        if (!Bounded(feet, PositionBound)) throw new ArgumentOutOfRangeException(nameof(feet));
        if (!Bounded(forward, 1e6) || forward.X * forward.X + forward.Z * forward.Z < 1e-12)
            throw new ArgumentOutOfRangeException(nameof(forward));
        if (!InRange(profile.Radius, .05, 1) || !InRange(profile.Height, profile.Radius * 2, 4) ||
            !InRange(profile.Reach, .1, 4) || !InRange(profile.MinimumRise, .15, 2) ||
            !InRange(profile.MaximumRise, profile.MinimumRise, 3) ||
            !InRange(profile.VaultDepth, profile.Radius + 2 * Skin, 5) || !InRange(profile.MaximumDrop, 0, 3) ||
            !InRange(profile.MaximumSlopeDegrees, 0, 60) || profile.DurationTicks is < 6 or > 180)
            throw new ArgumentOutOfRangeException(nameof(profile));
    }

    private static Vector3d Ease(Vector3d a, Vector3d b, uint time, uint duration)
    {
        double t = (double)time / duration;
        return Along(a, Subtract(b, a), t * t * (3 - 2 * t));
    }

    private static bool InRange(double value, double minimum, double maximum) => value >= minimum && value <= maximum;
    private static bool Bounded(Vector3d v, double maximum) => InRange(v.X, -maximum, maximum) &&
        InRange(v.Y, -maximum, maximum) && InRange(v.Z, -maximum, maximum);
    private static double Dot(Vector3d a, Vector3d b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    private static double Length(Vector3d v) => Math.Sqrt(Dot(v, v));
    private static Vector3d Scale(Vector3d v, double factor) => new(v.X * factor, v.Y * factor, v.Z * factor);
    private static Vector3d Subtract(Vector3d a, Vector3d b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    private static Vector3d Raise(Vector3d v, double y) => AtHeight(v, v.Y + y);
    private static Vector3d AtHeight(Vector3d v, double y) => new(v.X, y, v.Z);
    private static Vector3d Along(Vector3d v, Vector3d direction, double distance) =>
        new(v.X + direction.X * distance, v.Y + direction.Y * distance, v.Z + direction.Z * distance);

    private readonly ref struct NativeQueries : ITraversalQueries
    {
        private readonly GameContext context;
        private readonly ReadOnlySpan<EntityId> ignore;

        public NativeQueries(GameContext context, ReadOnlySpan<EntityId> ignore)
        {
            this.context = context;
            this.ignore = ignore;
        }

        public RayHit? Ray(Vector3d origin, Vector3d direction, double distance) => context.Raycast(origin, direction, distance, ignore);
        public CapsuleCastHit? Sweep(Vector3d center, double radius, double halfHeight, Vector3d direction, double distance) =>
            context.CapsuleCast(center, new Quaterniond(0, 0, 0, 1), radius, halfHeight, direction, distance, ignore);
        public bool IsStatic(EntityId entity) => context.Get(entity).Motion == BodyMotion.Static;
    }
}
