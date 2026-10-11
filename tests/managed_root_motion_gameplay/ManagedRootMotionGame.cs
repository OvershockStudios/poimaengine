// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;
public struct ManagedRootMotionState
{
    public EntityId Rig,Actor,Bone;
    public long FailAt,TicketHigh,TicketLow,TicketSequence;
    public int Ticks,Reads,Writes,Mode,Invisible,SaveSucceeded,PhysicalInputs;
    public double LastLocalX,LastRequestedX,LastAppliedX,LastYaw,LastError;
}
[GameModule("poima.test.managed-root-motion")]
public sealed class ManagedRootMotionGame:Game<ManagedRootMotionState>,IRootMotionGame,ICharacterInputGame
{
    public override void Initialize(ref ManagedRootMotionState state)
    {state=default;state.Rig=new(0,400);state.Actor=new(0,100);state.FailAt=-1;}
    private static RootMotionState Read(GameContext context,ref ManagedRootMotionState state)
    {
        var value=context.GetRootMotion(state.Rig) ?? throw new InvalidOperationException("Compiled root driver absent");
        if(value.Entity!=state.Rig || value.Actor!=state.Actor || value.Node!=2 || value.Mode!=RootMotionMode.Character || !value.Yaw ||
            value.ToTick!=context.Tick || value.FromTick>value.ToTick || value.ToTick-value.FromTick>1)
            throw new InvalidOperationException("Compiled committed root identity/interval differ");
        ++state.Reads;state.LastLocalX=value.LocalDelta.X;state.LastRequestedX=value.WorldRequested.X;
        state.LastAppliedX=value.WorldApplied.X;state.LastYaw=value.AppliedYaw;state.LastError=value.TranslationError;
        if(!value.Enabled && (value.LocalDelta!=default || value.YawDelta!=0 || value.WorldRequested!=default || value.WorldApplied!=default || value.AppliedYaw!=0 || value.TranslationError!=0))
            throw new InvalidOperationException("Disabled cached root interval is not neutral");
        return value;
    }
    private static void Write(GameContext context,ref ManagedRootMotionState state,bool enabled)
    {context.SetRootMotion(state.Rig,enabled);++state.Writes;}
    public override void Tick(ref ManagedRootMotionState state,GameContext context)
    {
        ++state.Ticks;state.PhysicalInputs+=context.Inputs.Length;var before=Read(context,ref state);
        if(state.Mode is 1 or 2 or 9 or 13 or 16)
        {
            if(state.Mode==2)context.SetAnimation(state.Rig,1,time:0,speed:1,loop:true,playing:true);
            if(state.Mode==13)context.SetAnimation(state.Rig,2,time:0,speed:1,loop:true,playing:true);
            Write(context,ref state,state.Mode!=1 && state.Mode!=16);
            if(context.GetRootMotion(state.Rig)!=before)throw new InvalidOperationException("Staged root control visible in Tick");
            ++state.Reads;++state.Invisible;state.Mode=0;
        }
        else if(state.Mode is 3 or 4)
        {
            for(int i=0;i<(state.Mode==3?1024:1023);++i)_=Read(context,ref state);
        }
        else if(state.Mode is 6 or 7)
        {
            for(int i=0;i<(state.Mode==6?65:64);++i){context.SetRootMotion(new EntityId(0,(ulong)(400+i)));++state.Writes;}
            state.Mode=0;
        }
        else if(state.Mode==5)
        {
            Write(context,ref state,false);var ticket=context.RequestSave("root-intent",0);
            state.TicketHigh=ticket.EpochHigh;state.TicketLow=ticket.EpochLow;state.TicketSequence=ticket.Sequence;state.Mode=0;
        }
        else if(state.Mode==8)
        {
            var result=context.GetSaveResult(new(state.TicketHigh,state.TicketLow,state.TicketSequence));
            if(result.State!=SaveOperationState.Succeeded || result.Kind!=SaveKind.Save || result.Generation!=1)
                throw new InvalidOperationException("Root durable intent did not finish");
            ++state.SaveSucceeded;state.Mode=0;
        }
        else if(state.Mode==10)context.SetCharacterInput(state.Actor,1,0);
        else if(state.Mode==11)context.SetCharacterInput(state.Actor,0,0,lookYaw:1);
        else if(state.Mode==12)context.SetCharacterInput(state.Actor,-1,0);
        else if(state.Mode==14){context.SetCharacterInput(state.Actor,0,0,lookPitch:1,jump:true);state.Mode=0;}
        else if(state.Mode==15){Write(context,ref state,true);Write(context,ref state,true);}
        if(state.Bone!=default)
        {
            var actor=context.Get(state.Actor).Transform.Position;var bone=context.Get(state.Bone).Transform.Position;
            if(Math.Abs(actor.X-bone.X)>1e-5 || Math.Abs(actor.Z-bone.Z)>1e-5)
                throw new InvalidOperationException("Committed selected root pose was not stripped to actor rest");
        }
        if((long)context.Tick==state.FailAt)throw new InvalidOperationException("Root later-tick rollback fixture");
    }
}
