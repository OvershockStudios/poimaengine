// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct ManagedAnimationRateState
{
    public EntityId Actor,Rig;
    public int Mode,Ticks,IdleClip,RunClip,TransitionMode,BlendTicks,Drive,AutoRate;
    public int BaseWrites,LayerWrites,DuringFadeWrites,StagedReads,ObservedMoving,PreviousPositionValid;
    public long FailAt,PreviousSampleTick;
    public double Move,NominalSpeed,RequestedSpeed,ObservedSpeed,ObservedTime,ObservedVelocity;
    public double PreviousX,PreviousZ;
}

[GameModule("poima.test.managed-animation-rate")]
public sealed class ManagedAnimationRateGame:Game<ManagedAnimationRateState>,IAnimationRateGame,IMaskedAnimationGame,ICharacterInputGame
{
    public override void Initialize(ref ManagedAnimationRateState state)
    {
        state=default;state.Actor=new(0,920);state.Rig=new(0,400);
        state.FailAt=-1;state.BlendTicks=120;state.NominalSpeed=3;state.RequestedSpeed=1;
    }
    static void BaseRate(ref ManagedAnimationRateState state,GameContext context,double speed)
    {
        var before=context.GetAnimationExtended(state.Rig) ?? throw new InvalidOperationException("Rate rig absent");
        context.SetAnimationSpeed(state.Rig,speed);++state.BaseWrites;
        if(before.State.Transition is not null)++state.DuringFadeWrites;
        if(context.GetAnimationExtended(state.Rig)!=before)throw new InvalidOperationException("Staged rate leaked into committed read");
        ++state.StagedReads;
    }
    public override void Tick(ref ManagedAnimationRateState state,GameContext context)
    {
        ++state.Ticks;
        var before=context.GetAnimationExtended(state.Rig) ?? throw new InvalidOperationException("Rate rig absent");
        var position=context.Get(state.Actor).Transform.Position;
        // Input preparation precedes Tick, so Get().Velocity is not a sample
        // of the last achieved movement. Retain consecutive committed positions
        // just as Locomotion Yard does, including this history in every save.
        long tick=checked((long)context.Tick);
        if(!double.IsFinite(context.DeltaTime)||context.DeltaTime<=0)throw new InvalidOperationException("Invalid fixed tick interval");
        double dx=position.X-state.PreviousX,dz=position.Z-state.PreviousZ;
        state.ObservedVelocity=state.PreviousPositionValid!=0&&state.PreviousSampleTick+1==tick
            ? Math.Sqrt(dx*dx+dz*dz)/context.DeltaTime:0;
        if(!double.IsFinite(state.ObservedVelocity))throw new InvalidOperationException("Invalid committed displacement speed");
        state.PreviousX=position.X;state.PreviousZ=position.Z;state.PreviousSampleTick=tick;state.PreviousPositionValid=1;
        state.ObservedSpeed=before.State.Speed;state.ObservedTime=before.State.Time;
        if(state.ObservedVelocity>.01)++state.ObservedMoving;
        if(state.Drive!=0)context.SetCharacterInput(state.Actor,(float)state.Move,0);
        if(state.Mode is 1 or 2)
        {
            context.SetAnimation(state.Rig,state.Mode==1?state.IdleClip:state.RunClip,
                state.TransitionMode==0?AnimationTransitionMode.Crossfade:AnimationTransitionMode.Inertial,
                speed:1,loop:true,playing:true,blendTicks:(uint)state.BlendTicks);
            state.Mode=0;
        }
        else if(state.Mode==3){BaseRate(ref state,context,state.RequestedSpeed);state.Mode=0;}
        else if(state.Mode==4)
        {
            var layer=context.GetAnimationLayer(state.Rig,1) ?? throw new InvalidOperationException("Rate layer absent");
            context.SetAnimationLayerSpeed(state.Rig,1,state.RequestedSpeed);++state.LayerWrites;
            if(context.GetAnimationLayer(state.Rig,1)!=layer)throw new InvalidOperationException("Staged layer rate leaked into committed read");
            ++state.StagedReads;state.Mode=0;
        }
        else if(state.Mode==5)
        {BaseRate(ref state,context,state.RequestedSpeed);BaseRate(ref state,context,state.RequestedSpeed);}
        else if(state.Mode==6)
        {
            context.SetAnimation(state.Rig,state.RunClip,AnimationTransitionMode.Inertial);
            BaseRate(ref state,context,state.RequestedSpeed);
        }
        else if(state.AutoRate!=0)
        {
            if(!double.IsFinite(state.NominalSpeed)||state.NominalSpeed<=0)throw new InvalidOperationException("Invalid nominal motion speed");
            double speed=Math.Clamp(state.ObservedVelocity/state.NominalSpeed,0,8);
            if(Math.Abs(speed-before.State.Speed)>=.01)BaseRate(ref state,context,speed);
        }
        if((long)context.Tick==state.FailAt)throw new InvalidOperationException("Animation rate later-Tick rollback fixture");
    }
}
