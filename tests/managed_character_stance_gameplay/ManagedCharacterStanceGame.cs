// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;
public struct ManagedCharacterStanceState
{
    public EntityId Actor,Born;
    public int Ticks,Reads,Writes,Invisible,Mode,SaveSucceeded,ObservedRequested,ObservedCrouched,ObservedBlocked;
    public float ObservedHeight,ObservedSpeed,ObservedDrop;
    public long FailAt,TicketHigh,TicketLow,TicketSequence;
}
[GameModule("poima.test.managed-character-stance")]
public sealed class ManagedCharacterStanceGame:Game<ManagedCharacterStanceState>,ICharacterStanceGame
{
    public override void Initialize(ref ManagedCharacterStanceState state)
    {state=default;state.Actor=new(0,100);state.FailAt=-1;}
    static CharacterStanceState Read(GameContext context,ref ManagedCharacterStanceState state,EntityId entity)
    {var value=context.GetCharacterStance(entity) ?? throw new InvalidOperationException("Compiled stance controller absent.");++state.Reads;return value;}
    static void Write(GameContext context,ref ManagedCharacterStanceState state,EntityId entity,bool crouch)
    {context.SetCharacterStance(entity,crouch);++state.Writes;}
    public override void Tick(ref ManagedCharacterStanceState state,GameContext context)
    {
        ++state.Ticks;var before=Read(context,ref state,state.Actor);
        state.ObservedRequested=before.RequestedCrouch?1:0;state.ObservedCrouched=before.Crouched?1:0;state.ObservedBlocked=before.StandBlocked?1:0;
        state.ObservedHeight=before.Height;state.ObservedSpeed=before.Speed;state.ObservedDrop=before.CameraDrop;
        if(state.Mode is 1 or 2 or 7 or 8)
        {
            Write(context,ref state,state.Actor,state.Mode!=2);
            if(Read(context,ref state,state.Actor)!=before)throw new InvalidOperationException("Staged stance became visible within Tick.");
            ++state.Invisible;
            if(state.Mode==7)Write(context,ref state,state.Actor,false);
            if(state.Mode==8){var ticket=context.RequestSave("stance-intent",0);state.TicketHigh=ticket.EpochHigh;state.TicketLow=ticket.EpochLow;state.TicketSequence=ticket.Sequence;}
            state.Mode=0;
        }
        else if(state.Mode is 3 or 4)
        {for(int i=0;i<(state.Mode==3?255:256);++i)_=Read(context,ref state,state.Actor);state.Mode=0;}
        else if(state.Mode is 5 or 6)
        {Write(context,ref state,state.Actor,true);for(int i=0;i<(state.Mode==5?31:32);++i)Write(context,ref state,new EntityId(0,(ulong)(1000+i)),true);state.Mode=0;}
        else if(state.Mode==9)
        {
            var result=context.GetSaveResult(new(state.TicketHigh,state.TicketLow,state.TicketSequence));
            if(result.State!=SaveOperationState.Succeeded || result.Kind!=SaveKind.Save || result.Generation!=1)throw new InvalidOperationException("Stance durable intent not complete.");
            ++state.SaveSucceeded;state.Mode=0;
        }
        else if(state.Mode==10)
        {
            if(context.GetCharacterStance(new EntityId(0,500))!=null)throw new InvalidOperationException("Live noncontroller stance not null.");++state.Reads;
            var legacy=Read(context,ref state,new EntityId(0,300));
            if(legacy.Configured || legacy.RequestedCrouch || legacy.Crouched || legacy.StandBlocked || legacy.CrouchedHeight!=legacy.StandingHeight || legacy.Height!=legacy.StandingHeight || legacy.Speed!=3 || legacy.CameraDrop!=0)throw new InvalidOperationException("Legacy stance changed.");
            bool denied=false;try{context.SetCharacterInput(state.Actor,0,0);}catch(ArgumentException e){denied=e.Message.Contains("Declare ICharacterInputGame");}
            if(!denied)throw new InvalidOperationException("Stance marker granted character input.");state.Mode=0;
        }
        else if(state.Mode==11)Write(context,ref state,new EntityId(0,300),true);
        else if(state.Mode==12)Write(context,ref state,new EntityId(0,500),true);
        else if(state.Mode==13)Write(context,ref state,new EntityId(0,999999),true);
        else if(state.Mode==14)
        {
            state.Born=context.Spawn(new TemplateId(0,700));var original=Read(context,ref state,state.Born);
            if(!original.Configured || original.RequestedCrouch || original.Crouched || original.StandBlocked || original.Height!=2 || original.CameraDrop!=0)throw new InvalidOperationException("Pending stance authored state differs.");
            Write(context,ref state,state.Born,true);if(Read(context,ref state,state.Born)!=original)throw new InvalidOperationException("Pending staged stance exposed.");++state.Invisible;state.Mode=0;
        }
        else if(state.Mode==15){Write(context,ref state,state.Born,false);context.Despawn(state.Born);}
        if((long)context.Tick==state.FailAt)throw new InvalidOperationException("Stance later-Tick rollback fixture.");
    }
}
