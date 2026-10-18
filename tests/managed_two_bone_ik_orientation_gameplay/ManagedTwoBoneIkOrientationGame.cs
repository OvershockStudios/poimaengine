// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;
public struct ManagedTwoBoneIkOrientationState
{
    public EntityId Rig,Spawned;
    public long FailAt,TemplateHigh,TemplateLow,TicketHigh,TicketLow,TicketSequence;
    public int Mode,Ticks,Reads,Writes,Invisible,HasOrientation,SaveSucceeded;
    public double GoalX,GoalY,GoalZ,GoalW,ActualX,ActualY,ActualZ,ActualW,OrientationWeight,AngularError;
    public double PointWeight,TargetX,TargetY,TargetZ,EndX,EndY,EndZ,TargetError;
}
[GameModule("poima.test.managed-two-bone-ik-orientation")]
public sealed class ManagedTwoBoneIkOrientationGame:Game<ManagedTwoBoneIkOrientationState>,ITwoBoneIkOrientationGame
{
    public override void Initialize(ref ManagedTwoBoneIkOrientationState state)
    {state=default;state.Rig=new(0,400);state.FailAt=-1;}
    static IkOrientedState Read(GameContext context,ref ManagedTwoBoneIkOrientationState state)
    {
        var value=context.GetTwoBoneIkOrientation(state.Rig,1) ?? throw new InvalidOperationException("Compiled oriented IK chain absent");
        ++state.Reads;state.HasOrientation=value.EndOrientation.HasValue?1:0;
        state.PointWeight=value.Point.Weight;state.TargetX=value.Point.Target.X;state.TargetY=value.Point.Target.Y;state.TargetZ=value.Point.Target.Z;
        state.EndX=value.Point.EndPosition.X;state.EndY=value.Point.EndPosition.Y;state.EndZ=value.Point.EndPosition.Z;state.TargetError=value.Point.TargetError;
        var orientation=value.EndOrientation;state.GoalX=orientation?.Goal.Rotation.X??0;state.GoalY=orientation?.Goal.Rotation.Y??0;
        state.GoalZ=orientation?.Goal.Rotation.Z??0;state.GoalW=orientation?.Goal.Rotation.W??0;
        state.ActualX=orientation?.ActualRotation.X??0;state.ActualY=orientation?.ActualRotation.Y??0;
        state.ActualZ=orientation?.ActualRotation.Z??0;state.ActualW=orientation?.ActualRotation.W??0;
        state.OrientationWeight=orientation?.Goal.Weight??0;state.AngularError=orientation?.AngularError??0;return value;
    }
    static void Write(GameContext context,ref ManagedTwoBoneIkOrientationState state,EntityId rig,double pointWeight,IkEndOrientationGoal? goal)
    {
        context.SetTwoBoneIkOrientation(new(rig,1,new(1,1,0),new(0,0,1),true,IkSpace.Rig,pointWeight,goal));++state.Writes;
    }
    public override void Tick(ref ManagedTwoBoneIkOrientationState state,GameContext context)
    {
        ++state.Ticks;var before=Read(context,ref state);var full=new IkEndOrientationGoal(new(0,0,Math.Sqrt(.5),Math.Sqrt(.5)),1);
        switch(state.Mode)
        {
            case 1:Write(context,ref state,state.Rig,1,full);break;
            case 2:Write(context,ref state,state.Rig,0,new(new(.5,.5,.5,.5),1));break;
            case 3:Write(context,ref state,state.Rig,.5,new(new(.5,.5,.5,.5),.25));break;
            case 4:context.SetTwoBoneIk(state.Rig,1,new(1,1,0),new(0,0,1),space:IkSpace.Rig);++state.Writes;break;
            case 5:Write(context,ref state,state.Rig,1,null);break;
            case 6:Write(context,ref state,state.Rig,0,new(new(0,0,0,2),0));break;
            case 7:
                Write(context,ref state,state.Rig,1,full);context.SetTwoBoneIk(state.Rig,1,new(1,1,0),new(0,0,1),space:IkSpace.Rig);++state.Writes;break;
            case 8:
                context.SetTwoBoneIk(state.Rig,1,new(1,1,0),new(0,0,1),space:IkSpace.Rig);++state.Writes;Write(context,ref state,state.Rig,1,full);break;
            case 9:case 10:
                for(int i=0;i<(state.Mode==9?64:65);++i)Write(context,ref state,new(0,(ulong)(400+i)),0,full);break;
            case 11:case 12:
                for(int i=0;i<(state.Mode==11?1023:1024);++i)
                {if((i&1)==0){_=context.GetTwoBoneIk(state.Rig,1);++state.Reads;}else _=Read(context,ref state);}break;
            case 13:Write(context,ref state,state.Rig,0,new(default,1));break;
            case 14:
                var ticket=context.RequestSave("ik-orientation-intent",0);state.TicketHigh=ticket.EpochHigh;state.TicketLow=ticket.EpochLow;state.TicketSequence=ticket.Sequence;
                Write(context,ref state,state.Rig,1,full);break;
            case 15:
                var result=context.GetSaveResult(new(state.TicketHigh,state.TicketLow,state.TicketSequence));
                if(result.State!=SaveOperationState.Succeeded || result.Kind!=SaveKind.Save || result.Generation!=1)throw new InvalidOperationException("Oriented IK durable intent did not finish");
                ++state.SaveSucceeded;break;
            case 16:case 17:
                state.Spawned=context.Spawn(new TemplateId(unchecked((ulong)state.TemplateHigh),unchecked((ulong)state.TemplateLow)));
                Write(context,ref state,state.Spawned,0,full);
                if(state.Mode==17){context.SetTwoBoneIk(state.Spawned,1,new(1,1,0),new(0,0,1),space:IkSpace.Rig);++state.Writes;}break;
        }
        if(state.Mode is >=1 and <=6 or 14 or 16)
        {
            if(context.GetTwoBoneIkOrientation(state.Rig,1)!=before)throw new InvalidOperationException("Staged oriented IK became visible inside Tick");
            ++state.Reads;++state.Invisible;
        }
        state.Mode=0;
        if((long)context.Tick==state.FailAt)throw new InvalidOperationException("Oriented IK later-tick rollback fixture");
    }
}
