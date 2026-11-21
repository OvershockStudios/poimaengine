// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using Poima;
using Poima.Fps;

namespace Poima.Tests;

public struct MeleeContactState
{
    public long FailAt;
    public int ActiveTicks, Queries, Hits, RepeatedContacts, BlockedContacts;
    public int InitialOverlap;
    public EntityId Candidate, FirstContact, HitTarget;
    public double Fraction, EnvelopeError, Penetration;
    public double GripX, GripY, GripZ, TipX, TipY, TipZ;
    public double QueryX, QueryY, QueryZ, ObstacleX, ObstacleY, ObstacleZ;
    public double NormalX, NormalY, NormalZ;
}

[GameModule("poima.test.melee-contact")]
public sealed class MeleeContactGame : Game<MeleeContactState>, ICapsuleCastGame
{
    private const int ActiveWindowTicks = 5;
    private static readonly EntityId Target = new(0, 100);
    private static readonly EntityId Witness = new(0, 200);
    private static readonly MeleeSweepProfile Profile = new(.075, .004, 24);

    public override void Initialize(ref MeleeContactState state)
    {
        state.FailAt = -1;
        state.Fraction = 1;
        StorePose(ref state, Pose(0));
    }

    public override void Tick(ref MeleeContactState state, GameContext context)
    {
        if (state.ActiveTicks >= ActiveWindowTicks)
            return;

        var previous = new MeleeBladeSample(
            new(state.GripX, state.GripY, state.GripZ),
            new(state.TipX, state.TipY, state.TipZ));
        var current = Pose(checked((int)context.Tick + 1));
        var sweep = Melee.Sweep(Profile, previous, current, context, [Witness]);
        ++state.ActiveTicks;
        StorePose(ref state, current);
        state.Candidate = sweep.Hit?.Entity ?? default;
        state.Queries = sweep.Queries;
        state.Fraction = sweep.Fraction;
        state.EnvelopeError = sweep.MaxIntervalError;
        state.InitialOverlap = sweep.Hit is { InitialOverlap: true } ? 1 : 0;
        state.Penetration = sweep.Hit?.PenetrationDepth ?? 0;
        var query = sweep.Hit?.PointOnQuery ?? default;
        (state.QueryX, state.QueryY, state.QueryZ) = (query.X, query.Y, query.Z);
        var obstacle = sweep.Hit?.PointOnObstacle ?? default;
        (state.ObstacleX, state.ObstacleY, state.ObstacleZ) = (obstacle.X, obstacle.Y, obstacle.Z);
        var normal = sweep.Hit?.Normal ?? default;
        (state.NormalX, state.NormalY, state.NormalZ) = (normal.X, normal.Y, normal.Z);

        if (sweep.Hit is { } hit)
        {
            if (state.FirstContact == default)
            {
                state.FirstContact = hit.Entity;
                context.MoveKinematic(Witness, hit.PointOnObstacle, Quaternion.Identity, 1);
            }

            if (state.FirstContact != Target)
                ++state.BlockedContacts;
            else if (state.HitTarget == hit.Entity)
                ++state.RepeatedContacts;
            else if (hit.Entity == Target)
            {
                state.HitTarget = hit.Entity;
                ++state.Hits;
            }
        }

        if ((long)context.Tick == state.FailAt)
        {
            var location = context.Get(Witness).Transform.Position;
            if (location.X < 2 || location.X > 2.04)
                throw new InvalidOperationException("Melee fixture expected the first contact's native marker before failing.");
            throw new InvalidOperationException(
                $"Melee fixture later Tick fault after {state.Hits} accepted hit, {state.RepeatedContacts} repeat and a native marker move.");
        }
    }

    // The tip follows a wider arc than the grip. Tick poses form the ends
    // of the package's continuous, piecewise-linear trajectory.
    private static MeleeBladeSample Pose(int tick) => new(
        new(.35 + .42 * tick + .018 * tick * tick, 1.05 + .018 * tick, -.35 + .03 * tick),
        new(.65 + .48 * tick + .035 * tick * tick, 1.38 + .027 * tick * tick, .4 - .025 * tick));

    private static void StorePose(ref MeleeContactState state, MeleeBladeSample pose)
    {
        (state.GripX, state.GripY, state.GripZ) = (pose.Grip.X, pose.Grip.Y, pose.Grip.Z);
        (state.TipX, state.TipY, state.TipZ) = (pose.Tip.X, pose.Tip.Y, pose.Tip.Z);
    }
}
