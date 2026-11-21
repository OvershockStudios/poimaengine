// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using System.Runtime.InteropServices;
using Poima;
using Poima.Fps;

namespace Poima.TraversalFixture;

[StructLayout(LayoutKind.Sequential)]
public struct State
{
    public int Kind;
    public int Status;
    public int Failure;
    public int Elapsed;
    public double StartX, StartY, StartZ, LandingX, LandingY, LandingZ, ObstacleTop;
    public EntityId Obstacle;
    public EntityId Support;
    public EntityId Colliding;
}

[GameModule("poima.fps.traversal-fixture")]
public sealed class Game : Game<State>, ICapsuleCastGame
{
    private static readonly TraversalProfile Profile = new(.3, 1.6, 1.1, .2, 1.2, .9, .4, 35, 32);
    private static readonly EntityId Actor = new(0, 2);
    private const double BodyOrigin = .825;

    public override void Initialize(ref State state) { }

    public override void Tick(ref State state, GameContext context)
    {
        if (state.Status != 0) return;

        var here = context.Get(Actor).Transform.Position;
        var feet = new Vector3d(here.X, here.Y - BodyOrigin, here.Z);
        Span<EntityId> ignore = stackalloc EntityId[1] { Actor };
        var kind = (TraversalKind)state.Kind;
        TraversalPlan plan;
        if (state.Elapsed == 0)
        {
            var choice = Traversal.Plan(in Profile, kind, feet, new Vector3d(0, 0, -1), context, ignore);
            if (choice.Plan is not { } found)
            {
                Refuse(ref state, choice.Failure, choice.Collider);
                return;
            }
            plan = found;
            state.StartX = plan.Start.X;
            state.StartY = plan.Start.Y;
            state.StartZ = plan.Start.Z;
            state.LandingX = plan.Landing.X;
            state.LandingY = plan.Landing.Y;
            state.LandingZ = plan.Landing.Z;
            state.ObstacleTop = plan.ObstacleTop;
            state.Obstacle = plan.Obstacle;
            state.Support = plan.Support;
        }
        else
        {
            plan = Traversal.Restore(in Profile, kind, new(state.StartX, state.StartY, state.StartZ),
                new(state.LandingX, state.LandingY, state.LandingZ), state.ObstacleTop, state.Obstacle, state.Support);
        }

        var motion = Traversal.Advance(in plan, (uint)state.Elapsed, feet, context, ignore);
        if (motion.Failure != TraversalFailure.None)
        {
            Refuse(ref state, motion.Failure, motion.Collider);
            return;
        }

        var target = motion.Sample.Feet;
        context.MoveKinematic(Actor, new(target.X, target.Y + BodyOrigin, target.Z), Quaternion.Identity, 1);
        ++state.Elapsed;
        if (motion.Sample.Phase == TraversalPhase.Complete) state.Status = 1;
    }

    private static void Refuse(ref State state, TraversalFailure failure, EntityId collider)
    {
        state.Status = -1;
        state.Failure = (int)failure;
        state.Colliding = collider;
    }
}
