// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;
public struct ManagedCameraLensState
{
    public EntityId Camera,Born;
    public int Ticks,Reads,Writes,Invisible,Mode,SaveSucceeded;
    public long FailAt,TicketHigh,TicketLow,TicketSequence;
    public double ObservedWorld,ObservedViewmodel;
}
[GameModule("poima.test.managed-camera-lens")]
public sealed class ManagedCameraLensGame:Game<ManagedCameraLensState>,ICameraLensGame
{
    public override void Initialize(ref ManagedCameraLensState state)
    {state=default;state.Camera=new(0,101);state.FailAt=-1;}
    static CameraLensState Read(GameContext context,ref ManagedCameraLensState state,EntityId id)
    {
        var value=context.GetCameraLens(id) ?? throw new InvalidOperationException("Compiled lens target is not a Camera.");
        ++state.Reads;return value;
    }
    static void Write(GameContext context,ref ManagedCameraLensState state,EntityId id,CameraLens? world=null,CameraLens? viewmodel=null)
    {context.SetCameraLens(id,world,viewmodel);++state.Writes;}
    public override void Tick(ref ManagedCameraLensState state,GameContext context)
    {
        ++state.Ticks;var before=Read(context,ref state,state.Camera);
        state.ObservedWorld=before.World.VerticalFov;state.ObservedViewmodel=before.Viewmodel?.VerticalFov ?? 0;
        if(state.Mode is 1 or 2 or 3 or 7 or 8)
        {
            Write(context,ref state,state.Camera,state.Mode==3?null:new CameraLens(35,.02,300),state.Mode==2?null:new CameraLens(55,.005,20));
            if(context.GetCameraLens(state.Camera)!=before)throw new InvalidOperationException("Staged lens write leaked into Tick read.");
            ++state.Reads;++state.Invisible;
            if(state.Mode==7)Write(context,ref state,state.Camera,new CameraLens(40,.02,300));
            if(state.Mode==8){var ticket=context.RequestSave("lens-intent",0);state.TicketHigh=ticket.EpochHigh;state.TicketLow=ticket.EpochLow;state.TicketSequence=ticket.Sequence;}
            state.Mode=0;
        }
        else if(state.Mode is 4 or 5)
        {
            for(int i=0;i<(state.Mode==4?256:255);++i)_=Read(context,ref state,state.Camera);
        }
        else if(state.Mode is 6 or 15)
        {
            for(int i=0;i<(state.Mode==6?65:64);++i)Write(context,ref state,new EntityId(0,(ulong)(1000+i)),new CameraLens(45,.03,250));
            state.Mode=0;
        }
        else if(state.Mode==9)
        {
            var result=context.GetSaveResult(new(state.TicketHigh,state.TicketLow,state.TicketSequence));
            if(result.State!=SaveOperationState.Succeeded || result.Kind!=SaveKind.Save || result.Generation!=1)throw new InvalidOperationException("Lens durable intent not complete.");
            ++state.SaveSucceeded;state.Mode=0;
        }
        else if(state.Mode==10)
        {
            state.Born=context.Spawn(new TemplateId(0,700));var original=Read(context,ref state,state.Born);
            if(original.World!=new CameraLens(60,.1,100) || original.Viewmodel!=new CameraLens(75,.01,10) || original.AuthoredWorld!=original.World)throw new InvalidOperationException("Pending Camera authored baseline differs.");
            Write(context,ref state,state.Born,new CameraLens(25,.02,200),new CameraLens(50,.005,15));
            if(context.GetCameraLens(state.Born)!=original)throw new InvalidOperationException("Pending staged lens visible before publish.");
            ++state.Reads;++state.Invisible;state.Mode=0;
        }
        else if(state.Mode==11){Write(context,ref state,state.Born,new CameraLens(30,.02,200));context.Despawn(state.Born);}
        else if(state.Mode==12)Write(context,ref state,state.Camera,new CameraLens(double.NaN,.02,200));
        else if(state.Mode==13)Write(context,ref state,new EntityId(0,200),null,new CameraLens(50,.01,10));
        else if(state.Mode==14)
        {
            if(context.GetCameraLens(new EntityId(0,100))!=null)throw new InvalidOperationException("Live noncamera was not absent.");
            ++state.Reads;state.Mode=0;
        }
        if((long)context.Tick==state.FailAt)throw new InvalidOperationException("Lens later-Tick rollback fixture.");
    }
}
