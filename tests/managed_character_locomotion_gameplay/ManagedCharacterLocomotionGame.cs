// SPDX-License-Identifier: Apache-2.0
using Poima;
using System.Numerics;
namespace Poima.Tests;
public struct ManagedCharacterLocomotionState
{
    public EntityId Actor;
    public int Ticks,Mode,Crouch,Inputs,StanceWrites,Invisible,ProxyHits,SaveSucceeded;
    public long LastTick,FailAt,TicketHigh,TicketLow,TicketSequence;
    public double ObservedX,ObservedY,ObservedVelocityX,ObservedVelocityY;
}
[GameModule("poima.test.managed-character-locomotion")]
public sealed class ManagedCharacterLocomotionGame:Game<ManagedCharacterLocomotionState>,ICharacterInputGame,ICharacterStanceGame
{
    public override void Initialize(ref ManagedCharacterLocomotionState state)
    {state=default;state.Actor=new(0,100);state.FailAt=-1;}
    static void Input(GameContext context,ref ManagedCharacterLocomotionState state,ulong id,float right,bool jump=false)
    {context.SetCharacterInput(new EntityId(0,id),right,0,jump:jump);++state.Inputs;}
    public override void Tick(ref ManagedCharacterLocomotionState state,GameContext context)
    {
        ++state.Ticks;state.LastTick=(long)context.Tick;
        var actor=context.Get(state.Actor);
        if(actor.Motion!=BodyMotion.Character)throw new InvalidOperationException("Compiled locomotion actor is not a character.");
        state.ObservedX=actor.Transform.Position.X;state.ObservedY=actor.Transform.Position.Y;
        state.ObservedVelocityX=actor.Velocity.X;state.ObservedVelocityY=actor.Velocity.Y;
        if(state.Mode==1)foreach(ulong id in new ulong[]{100,200,300,400})Input(context,ref state,id,1);
        else if(state.Mode==2)Input(context,ref state,100,1);
        else if(state.Mode==3){Input(context,ref state,100,0,true);state.Mode=0;}
        else if(state.Mode==4)
        {context.MoveKinematic(new EntityId(0,20),new Vector3d(3,.5,20),Quaternion.Identity,120);state.Mode=0;}
        else if(state.Mode==5)
        {
            var before=context.GetCharacterStance(state.Actor) ?? throw new InvalidOperationException("Compiled stance absent.");
            context.SetCharacterStance(state.Actor,state.Crouch!=0);++state.StanceWrites;
            if(context.GetCharacterStance(state.Actor)!=before)throw new InvalidOperationException("Queued stance visible inside callback.");
            ++state.Invisible;state.Mode=0;
        }
        else if(state.Mode==6)Input(context,ref state,100,1);
        else if(state.Mode==8)Input(context,ref state,100,-1);
        else if(state.Mode==9)
        {
            var ticket=context.RequestSave("locomotion-intent",0);
            state.TicketHigh=ticket.EpochHigh;state.TicketLow=ticket.EpochLow;state.TicketSequence=ticket.Sequence;state.Mode=0;
        }
        else if(state.Mode==10)
        {
            var result=context.GetSaveResult(new(state.TicketHigh,state.TicketLow,state.TicketSequence));
            if(result.State!=SaveOperationState.Succeeded || result.Kind!=SaveKind.Save || result.Generation!=1)
                throw new InvalidOperationException("Compiled locomotion intent not durable.");
            ++state.SaveSucceeded;state.Mode=0;
        }
        else if(state.Mode==11)
        {context.SetCharacterStance(state.Actor,true);context.SetCharacterStance(state.Actor,false);}
        else if(state.Mode==13)
        {
            var supportActor=context.Get(new EntityId(0,500));var foot=supportActor.Transform.Position;
            var hit=context.Raycast(new Vector3d(foot.X-2,foot.Y+.9,foot.Z),new Vector3d(1,0,0),4);
            if(hit is not {} value || value.Entity!=new EntityId(0,500) || !double.IsFinite(value.Distance) || value.Distance<=1 || value.Distance>=2)
                throw new InvalidOperationException("Real compiled ray did not hit the virtual character query proxy.");
            ++state.ProxyHits;state.Mode=0;
        }
        if((long)context.Tick==state.FailAt)throw new InvalidOperationException("Locomotion later-Tick rollback fixture.");
    }
}
