// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct ManagedGroundingControlState
{
    public EntityId Rig,Actor;
    public int Mode,Ticks,Reads,Writes,JumpRequests;
    public long FailAt;
}

[GameModule("poima.test.managed-grounding-control")]
public sealed class ManagedGroundingControlGame:Game<ManagedGroundingControlState>,IGroundingControlGame,ICharacterInputGame
{
    public override void Initialize(ref ManagedGroundingControlState state)
    {state=default;state.Rig=new(0,400);state.Actor=new(0,920);state.FailAt=-1;}

    public override void Tick(ref ManagedGroundingControlState state,GameContext context)
    {
        ++state.Ticks;
        for(uint slot=1;slot<=2;++slot)
        {
            var before=context.GetGroundingControl(state.Rig,slot)
                ?? throw new InvalidOperationException("Original Knight automatic foot absent");
            ++state.Reads;
            if(state.Mode is >=1 and <=5)
            {
                bool enabled=state.Mode is 2 or 3;
                double weight=state.Mode switch {1=>.75,2=>.5,3=>1,4=>.25,_=>1};
                context.SetGroundingControl(new(state.Rig,slot,enabled,weight));
                ++state.Writes;
                var after=context.GetGroundingControl(state.Rig,slot)
                    ?? throw new InvalidOperationException("Staged control removed ownership");
                ++state.Reads;
                if(after!=before)throw new InvalidOperationException("Staged grounding control leaked into committed read");
            }
            if(state.Mode==6)
                context.SetTwoBoneIk(state.Rig,slot,new(0,0,0),new(0,0,1));
        }
        if(state.Mode==4&&(long)context.Tick==state.FailAt)
            throw new InvalidOperationException("Grounding later-tick rollback fixture");
        if(state.Mode==7)
            for(int i=0;i<1023;++i){_ = context.GetGroundingControl(state.Rig,1);++state.Reads;}
        if(state.Mode==8)
        {
            context.SetGroundingControl(new(state.Rig,1,true,.5));
            context.SetGroundingControl(new(state.Rig,1,true,.25));
        }
        if(state.Mode==5){context.SetCharacterInput(state.Actor,0,0,jump:true);++state.JumpRequests;}
        if(state.Mode is 1 or 2 or 3 or 5)state.Mode=0;
    }
}
