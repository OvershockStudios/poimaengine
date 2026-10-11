// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct ManagedHitRegionsState
{
    public EntityId Owner,Head,Box,Capsule,Geometry,Fallback,Rig,Bone;
    public long FailAt,TicketHigh,TicketLow,TicketSequence;
    public int Ticks,Queries,RegionHits,GeometryHits,Misses,LegacyHits,Mode,SaveSucceeded,BoneHits,BoneMisses;
    public double LastDistance;
}
[GameModule("poima.test.managed-hit-regions")]
public sealed class ManagedHitRegionsGame : Game<ManagedHitRegionsState>,IHitRegionsGame
{
    public override void Initialize(ref ManagedHitRegionsState state)
    {
        state=default;state.Owner=new(0,300);state.Head=new(0,301);state.Box=new(0,302);state.Capsule=new(0,303);
        state.Geometry=new(0,400);state.Fallback=new(0,500);state.Rig=EntityId.Parse(new('a',32));state.FailAt=-1;
    }
    private static HitTestHit Region(GameContext context,Vector3d origin,EntityId owner,EntityId attachment,string name)
    {
        var result=context.HitTest(origin,new(0,0,-1),20) ?? throw new InvalidOperationException("Compiled hit region missing");
        if(result.Entity!=owner || result.RegionEntity!=attachment || result.Region!=name || result.Triangle!=null ||
            !double.IsFinite(result.Distance) || !double.IsFinite(result.Fraction) ||
            Math.Abs(result.Distance-3.5)>1e-12 || Math.Abs(result.Fraction-.175)>1e-14 || result.Normal!=new Vector3d(0,0,1))
            throw new InvalidOperationException("Compiled region identity/distance/normal differs");
        return result;
    }
    public override void Tick(ref ManagedHitRegionsState state,GameContext context)
    {
        ++state.Ticks;
        var head=Region(context,new(0,0,0),state.Owner,state.Head,"head");
        _=Region(context,new(2,0,0),state.Owner,state.Box,"torso");
        _=Region(context,new(-2,0,0),state.Owner,state.Capsule,"limb");
        state.RegionHits+=3;state.LastDistance=head.Distance;
        var body=context.HitTest(new(6,0,0),new(0,0,-1),20) ?? throw new InvalidOperationException("Compiled geometry missing");
        if(body.Entity!=state.Geometry || body.RegionEntity!=null || body.Region!=null || body.Triangle!=null)
            throw new InvalidOperationException("Compiled geometry identity differs");
        ++state.GeometryHits;
        if(context.HitTest(new(10,0,0),new(0,0,-1),20)!=null)throw new InvalidOperationException("Compiled miss differs");
        ++state.Misses;
        foreach(var ignored in new[]{state.Owner,state.Head})
        {
            var fallback=context.HitTest(default,new(0,0,-1),20,[ignored]) ?? throw new InvalidOperationException("Compiled ignored-region fallback missing");
            if(fallback.Entity!=state.Fallback || fallback.Region!=null)throw new InvalidOperationException("Owner/attachment ignore or proxy suppression differs");
            ++state.GeometryHits;
        }
        var inside=context.HitTest(new(0,0,-4),new(0,0,-1),20) ?? throw new InvalidOperationException("Compiled inside region missing");
        if(inside.RegionEntity!=state.Head || inside.Distance!=0 || inside.Fraction!=0 || inside.Normal!=null)
            throw new InvalidOperationException("Compiled inside semantics differ");
        ++state.RegionHits;
        var legacy=context.Raycast(default,new(0,0,-1),20) ?? throw new InvalidOperationException("Legacy raycast proxy missing");
        if(legacy.Entity!=state.Owner || legacy.Distance>=head.Distance)throw new InvalidOperationException("Legacy raycast changed or proxy was not preserved");
        ++state.LegacyHits;state.Queries+=8;
        if(state.Mode==1)_=context.HitTest(default,default,20);
        if(state.Mode==2)_=context.HitTest(default,new(0,0,-1),20,[new(0,999999)]);
        if(state.Mode is 3 or 4)
        {
            int additional=state.Mode==3?1017:1016;
            for(int i=0;i<additional;++i)_=context.HitTest(new(10,0,0),new(0,0,-1),20);
            state.Queries+=additional;
        }
        if(state.Mode==5)
        {
            var ticket=context.RequestSave("hit-intent",0);state.TicketHigh=ticket.EpochHigh;state.TicketLow=ticket.EpochLow;
            state.TicketSequence=ticket.Sequence;state.Mode=0;
        }
        if(state.Mode==7)
        {
            var result=context.GetSaveResult(new(state.TicketHigh,state.TicketLow,state.TicketSequence));
            if(result.State!=SaveOperationState.Succeeded || result.Kind!=SaveKind.Save || result.Generation!=1)
                throw new InvalidOperationException("Hit fixture save intent did not complete");
            ++state.SaveSucceeded;state.Mode=0;
        }
        if(state.Mode==8)
        {
            var position=context.Get(state.Bone).Transform.Position;
            if(Math.Abs(position.X-20.1)>1e-6)throw new InvalidOperationException("Caller pose was not visible at start of Tick");
            context.SetAnimation(state.Rig,0,time:0,speed:1,loop:false,playing:false);
            var result=context.HitTest(new(20.1,0,0),new(0,0,-1),20) ?? throw new InvalidOperationException("Animated bone region missing in compiled Tick");
            if(result.Entity!=state.Rig || result.RegionEntity!=state.Bone || result.Region!="animated" || Math.Abs(result.Distance-3.975)>1e-6)
                throw new InvalidOperationException("Animated attachment or staged-pose visibility differs");
            ++state.BoneHits;++state.Queries;state.Mode=9;
        }
        else if(state.Mode==9)
        {
            if(Math.Abs(context.Get(state.Bone).Transform.Position.X-20)>1e-6 || context.HitTest(new(20.1,0,0),new(0,0,-1),20)!=null)
                throw new InvalidOperationException("Published next-Tick pose did not change bone query");
            ++state.BoneMisses;++state.Queries;state.Mode=0;
        }
        if((long)context.Tick==state.FailAt)throw new InvalidOperationException("Hit region later-tick rollback fixture");
    }
}
