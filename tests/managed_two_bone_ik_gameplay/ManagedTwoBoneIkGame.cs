// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;
public struct ManagedTwoBoneIkState
{
    public EntityId Rig,Bone;
    public long FailAt,TicketHigh,TicketLow,TicketSequence;
    public int Ticks,Reads,Writes,Mode,Invisible,SaveSucceeded,CorrectedHits;
    public double LastTargetX,LastEndX,LastEndY,LastError;
}
[GameModule("poima.test.managed-two-bone-ik")]
public sealed class ManagedTwoBoneIkGame:Game<ManagedTwoBoneIkState>,ITwoBoneIkGame,IHitRegionsGame
{
    public override void Initialize(ref ManagedTwoBoneIkState state)
    {state=default;state.Rig=new(0,400);state.FailAt=-1;}
    private static IkState Read(GameContext context,ref ManagedTwoBoneIkState state)
    {
        var value=context.GetTwoBoneIk(state.Rig,1) ?? throw new InvalidOperationException("Compiled frozen IK chain absent");
        if(value.Entity!=state.Rig || value.Slot!=1 || value.Root!=2 || value.Middle!=1 || value.End!=3 ||
            !double.IsFinite(value.TargetError) || value.TargetError<0)throw new InvalidOperationException("Compiled IK identity/diagnostics differ");
        ++state.Reads;state.LastTargetX=value.Target.X;state.LastEndX=value.EndPosition.X;state.LastEndY=value.EndPosition.Y;state.LastError=value.TargetError;
        return value;
    }
    public override void Tick(ref ManagedTwoBoneIkState state,GameContext context)
    {
        ++state.Ticks;var before=Read(context,ref state);
        if(state.Mode is 1 or 9 or 10)
        {
            if(state.Mode==10)context.SetAnimation(state.Rig,0,time:0,speed:1,loop:false,playing:false);
            context.SetTwoBoneIk(state.Rig,1,state.Mode==10?new(1,1,0):new(0,2,0),new(0,0,1),space:IkSpace.Rig);
            ++state.Writes;
            if(context.GetTwoBoneIk(state.Rig,1)!=before)throw new InvalidOperationException("Staged IK became visible inside Tick");
            ++state.Reads;++state.Invisible;
            if(state.Bone!=default)
            {
                var position=context.Get(state.Bone).Transform.Position;
                if(Math.Abs(position.X-before.EndPosition.X)>1e-6 || Math.Abs(position.Y-before.EndPosition.Y)>1e-6)
                    throw new InvalidOperationException("IK getter and committed actual RigNode disagree");
                var hit=context.HitTest(new(before.EndPosition.X,before.EndPosition.Y,2),new(0,0,-1),10) ?? throw new InvalidOperationException("Committed effector region missing");
                if(hit.Entity!=state.Rig || hit.RegionEntity!=state.Bone || hit.Region!="effector" || Math.Abs(hit.Distance-1.9)>1e-6)
                    throw new InvalidOperationException("Compiled hit did not follow committed IK pose");
                ++state.CorrectedHits;
            }
            state.Mode=0;
        }
        else if(state.Mode==2)context.SetTwoBoneIk(state.Rig,9,default,default);
        else if(state.Mode is 3 or 4)
        {
            int extra=state.Mode==3?1024:1023;
            for(int i=0;i<extra;++i)_=Read(context,ref state);
        }
        else if(state.Mode is 6 or 7)
        {
            int count=state.Mode==6?65:64;
            for(int i=0;i<count;++i){context.SetTwoBoneIk(new(0,(ulong)(400+i)),1,new(1,1,0),new(0,0,1),space:IkSpace.Rig);++state.Writes;}
            state.Mode=0;
        }
        else if(state.Mode==5)
        {
            var ticket=context.RequestSave("ik-intent",0);state.TicketHigh=ticket.EpochHigh;state.TicketLow=ticket.EpochLow;
            state.TicketSequence=ticket.Sequence;state.Mode=0;
        }
        else if(state.Mode==8)
        {
            var result=context.GetSaveResult(new(state.TicketHigh,state.TicketLow,state.TicketSequence));
            if(result.State!=SaveOperationState.Succeeded || result.Kind!=SaveKind.Save || result.Generation!=1)
                throw new InvalidOperationException("IK durable intent did not finish");
            ++state.SaveSucceeded;state.Mode=0;
        }
        if((long)context.Tick==state.FailAt)throw new InvalidOperationException("IK later-tick rollback fixture");
    }
}
