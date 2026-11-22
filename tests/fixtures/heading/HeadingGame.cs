// SPDX-License-Identifier: Apache-2.0
using Poima;
using Poima.Ai;

namespace Poima.Tests;

public struct HeadingState
{
    public long FailAt;
    public int Ticks, HasDirection;
    public EntityId Target;
    public double AngularVelocity, CommittedYaw, CommandedYaw, ErrorRadians, AppliedRadians;
}

[GameModule("poima.test.heading")]
public sealed class HeadingGame : Game<HeadingState>
{
    private static readonly EntityId Actor = new(0, 100);
    private static readonly YawTurnProfile Profile = new(3, 36, .002);

    public override void Initialize(ref HeadingState state) => state.FailAt = -1;

    public override void Tick(ref HeadingState state, GameContext context)
    {
        EntityId target = context.Tick switch
        {
            < 12 => new(0, 201),
            < 20 => new(0, 202),
            < 28 => new(0, 203),
            _ => new(0, 204)
        };
        var actor = context.Get(Actor).Transform;
        var destination = context.Get(target).Transform.Position;
        var position = actor.Position;
        var direction = new Vector3d(destination.X - position.X, destination.Y - position.Y, destination.Z - position.Z);
        var step = YawSteering.Step(actor.Forward, direction,
            new YawSteeringState(state.AngularVelocity, YawHalfTurn.Positive), Profile);
        context.MoveKinematic(Actor, position, step.Rotation, 1);

        double previousCommittedYaw = state.CommittedYaw;
        state.Target = target;
        state.CommittedYaw = step.CommittedYaw;
        state.CommandedYaw = step.CommandedYaw;
        state.ErrorRadians = step.ErrorRadians;
        state.AppliedRadians = step.AppliedRadians;
        state.AngularVelocity = step.State.AngularVelocity;
        state.HasDirection = step.HasDirection ? 1 : 0;
        ++state.Ticks;

        if ((long)context.Tick == state.FailAt)
        {
            if (previousCommittedYaw <= 3 || step.CommittedYaw >= -3)
                throw new InvalidOperationException("Heading fixture expected the earlier tick to turn the native root through the seam.");
            throw new InvalidOperationException("Heading fixture later Tick fault after the native seam crossing.");
        }
    }
}
