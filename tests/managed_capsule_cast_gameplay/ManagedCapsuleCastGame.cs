// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

// Primitive caller-owned fields are deliberately saved in full by the normal
// gameplay state schema. CapsuleCastHit itself is transient, not saved state.
public struct ManagedCapsuleCastState
{
    public int Mode,Ticks,Queries,Hit,InitialOverlap,NormalValid,TriangleValid;
    public long FailAt,Triangle;
    public EntityId Entity;
    public double Fraction,Distance,QueryX,QueryY,QueryZ,ObstacleX,ObstacleY,ObstacleZ;
    public double NormalX,NormalY,NormalZ,PenetrationDepth;
}

[GameModule("poima.test.managed-capsule-cast")]
public sealed class ManagedCapsuleCastGame:Game<ManagedCapsuleCastState>,ICapsuleCastGame
{
    public override void Initialize(ref ManagedCapsuleCastState state)
    {state=default;state.FailAt=-1;}

    public override void Tick(ref ManagedCapsuleCastState state,GameContext context)
    {
        ++state.Ticks;
        int count=state.Mode is 32 or 33 ? state.Mode : 1;
        for(int i=0;i<count;++i)
        {
            // Native fixture: a box centered at (0,0,-3), half extents(1,1,.1).
            // Mode2 is an intentionally invalid query, not a silently fixed input.
            var hit=context.CapsuleCast(new(0,0,0),Quaterniond.Identity,.25,.5,
                state.Mode==2 ? new(0,0,0) : new(0,0,-1),5);
            ++state.Queries;
            state.Hit=hit.HasValue ? 1 : 0;
            if(hit is { } h)
            {
                state.Entity=h.Entity;state.Fraction=h.Fraction;state.Distance=h.Distance;
                state.QueryX=h.PointOnQuery.X;state.QueryY=h.PointOnQuery.Y;state.QueryZ=h.PointOnQuery.Z;
                state.ObstacleX=h.PointOnObstacle.X;state.ObstacleY=h.PointOnObstacle.Y;state.ObstacleZ=h.PointOnObstacle.Z;
                state.PenetrationDepth=h.PenetrationDepth;state.InitialOverlap=h.InitialOverlap ? 1 : 0;
                state.NormalValid=h.Normal.HasValue ? 1 : 0;
                state.NormalX=h.Normal?.X ?? 0;state.NormalY=h.Normal?.Y ?? 0;state.NormalZ=h.Normal?.Z ?? 0;
                state.TriangleValid=h.Triangle.HasValue ? 1 : 0;state.Triangle=h.Triangle ?? 0;
            }
            else
            {
                state.Entity=default;state.Fraction=state.Distance=state.QueryX=state.QueryY=state.QueryZ=0;
                state.ObstacleX=state.ObstacleY=state.ObstacleZ=state.NormalX=state.NormalY=state.NormalZ=state.PenetrationDepth=0;
                state.InitialOverlap=state.NormalValid=state.TriangleValid=0;state.Triangle=0;
            }
        }
        if((long)context.Tick==state.FailAt)
            throw new InvalidOperationException("Capsule later-Tick rollback fixture");
    }
}
