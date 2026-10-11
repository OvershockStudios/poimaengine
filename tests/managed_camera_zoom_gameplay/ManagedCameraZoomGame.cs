// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

// Every field is a known primitive or EntityId. No mutable state lives on Game.
public struct ManagedCameraZoomState
{
    public EntityId Camera;
    public int Mode,Ticks,Reads,Writes,Invisible,Retargets,SaveRequests,SaveSucceeded;
    public int DurationTicks,ActiveDurationTicks,ElapsedTicks,TransitionActive,ScriptActive,ScriptStep;
    public long TransitionTick,ScriptTick,LastWriteTick,FailAt,TicketHigh,TicketLow,TicketSequence;
    public double WorldStart,WorldTarget,WorldCurrent,ViewmodelStart,ViewmodelTarget,ViewmodelCurrent;
    public double ObservedWorld,ObservedViewmodel;
}

[GameModule("poima.test.managed-camera-zoom")]
public sealed class ManagedCameraZoomGame:Game<ManagedCameraZoomState>,ICameraLensGame
{
    public override void Initialize(ref ManagedCameraZoomState state)
    {
        state=default;state.Camera=new EntityId(0,101);state.DurationTicks=12;
        state.TransitionTick=state.ScriptTick=state.LastWriteTick=state.FailAt=-1;
        state.WorldStart=state.WorldTarget=state.WorldCurrent=70;
        state.ViewmodelStart=state.ViewmodelTarget=state.ViewmodelCurrent=75;
    }

    static void Retarget(ref ManagedCameraZoomState state,CameraLensState before,ulong tick,bool aiming,int duration)
    {
        if(duration is <0 or >3600)throw new InvalidOperationException("Zoom duration must be within0..3600 Ticks.");
        state.WorldStart=before.World.VerticalFov;state.ViewmodelStart=before.Viewmodel!.Value.VerticalFov;
        state.WorldTarget=aiming?35:before.AuthoredWorld.VerticalFov;state.ViewmodelTarget=aiming?50:75;
        state.WorldCurrent=state.WorldStart;state.ViewmodelCurrent=state.ViewmodelStart;
        state.ActiveDurationTicks=duration;state.ElapsedTicks=0;state.TransitionTick=checked((long)tick);
        state.TransitionActive=1;++state.Retargets;
    }

    public override void Tick(ref ManagedCameraZoomState state,GameContext context)
    {
        ++state.Ticks;
        var before=context.GetCameraLens(state.Camera) ?? throw new InvalidOperationException("Zoom target is not a Camera.");
        ++state.Reads;
        if(before.Viewmodel is null)throw new InvalidOperationException("Zoom target requires an authored foreground lens.");
        state.ObservedWorld=before.World.VerticalFov;state.ObservedViewmodel=before.Viewmodel.Value.VerticalFov;
        if(state.Mode is <0 or >6)throw new InvalidOperationException("Unknown zoom fixture mode.");
        bool requestSave=state.Mode==4;
        if(state.Mode is 1 or 2 or 4 or 6)
        {
            state.ScriptActive=0;
            Retarget(ref state,before,context.Tick,state.Mode!=2,state.Mode==6?0:state.DurationTicks);
        }
        else if(state.Mode==3)
        {
            state.ScriptActive=1;state.ScriptStep=0;state.ScriptTick=checked((long)context.Tick);
        }
        else if(state.Mode==5)
        {
            var result=context.GetSaveResult(new SaveTicket(state.TicketHigh,state.TicketLow,state.TicketSequence));
            if(result.State!=SaveOperationState.Succeeded || result.Kind!=SaveKind.Save || result.Generation!=1)
                throw new InvalidOperationException("Zoom durable intent not complete.");
            ++state.SaveSucceeded;
        }
        state.Mode=0;
        if(state.ScriptActive!=0)
        {
            var age=checked((long)context.Tick)-state.ScriptTick;
            if(age is 0 or 3 or 5 or 7)
            {
                Retarget(ref state,before,context.Tick,age is 0 or 5,state.DurationTicks);
                ++state.ScriptStep;
                if(age==7)state.ScriptActive=0;
            }
        }
        if(state.TransitionActive!=0)
        {
            if(state.ActiveDurationTicks is <0 or >3600 || state.TransitionTick<0 || (long)context.Tick<state.TransitionTick)
                throw new InvalidOperationException("Invalid saved zoom transition phase.");
            var age=checked((long)context.Tick)-state.TransitionTick+1;
            state.ElapsedTicks=(int)Math.Min(age,state.ActiveDurationTicks);
            state.WorldCurrent=CameraFovTransition.Evaluate(state.WorldStart,state.WorldTarget,(uint)state.ElapsedTicks,(uint)state.ActiveDurationTicks);
            state.ViewmodelCurrent=CameraFovTransition.Evaluate(state.ViewmodelStart,state.ViewmodelTarget,(uint)state.ElapsedTicks,(uint)state.ActiveDurationTicks);
            context.SetCameraLens(state.Camera,
                new CameraLens(state.WorldCurrent,before.World.Near,before.World.Far),
                new CameraLens(state.ViewmodelCurrent,before.Viewmodel.Value.Near,before.Viewmodel.Value.Far));
            ++state.Writes;state.LastWriteTick=checked((long)context.Tick);
            if(context.GetCameraLens(state.Camera)!=before)throw new InvalidOperationException("Staged zoom leaked into callback read.");
            ++state.Reads;++state.Invisible;
            if(state.ElapsedTicks==state.ActiveDurationTicks)state.TransitionActive=0;
        }
        if(requestSave)
        {
            var ticket=context.RequestSave("zoom-intent",0);
            state.TicketHigh=ticket.EpochHigh;state.TicketLow=ticket.EpochLow;state.TicketSequence=ticket.Sequence;++state.SaveRequests;
        }
        if((long)context.Tick==state.FailAt)throw new InvalidOperationException("Zoom later-Tick rollback fixture.");
    }
}
