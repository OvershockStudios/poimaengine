// SPDX-License-Identifier: Apache-2.0
using Poima;
using System.Numerics;
namespace Poima.Verification;
public struct LayerState
{
    public EntityId Rig, Plain, Body, Emitter;
    public int Mode, Serial, AppliedSerial, Clip, Slot, QuerySlot, TransitionMode, Loop, Playing, BlendTicks, WeightBlendTicks, Count, Ticks, StagingChecks, NullProbe;
    public double Time, Speed, Weight;
    public long ThrowAt, DelayUntil, LastVoice, QueryTick;
    public int QSlot, QMode, QMaskNodes, QClip, QLoop, QPlaying, QHasTransition, QTransitionMode, QHasWeightTransition, QSourceFrozen;
    public double QWeight, QTargetWeight, QTime, QSpeed, QDuration, QProgress;
    public long QStartTick, QWeightStartTick;
    public int QDurationTicks, QElapsedTicks, QWeightDurationTicks, QWeightElapsedTicks;
    public double QWeightSource, QWeightTarget;
    public int QBaseClip, QBaseMode, QBaseMatches;
    public double QBaseTime, QBaseProgress;
}
[GameModule("poima.verification.layer-services")]
public sealed class LayerGame : Game<LayerState>, IMaskedAnimationGame
{
    private static void Stamp(string phase)
    {
        string? path = Environment.GetEnvironmentVariable("POIMA_LAYER_SENTINEL");
        if (path is not null) File.AppendAllText(path, phase+"\n");
    }
    public LayerGame() { Stamp("constructor"); }
    public override void Initialize(ref LayerState state)
    {
        Stamp("initialize"); state.Rig=new(0,1000); state.Plain=new(0,3);
        state.Body=new(0,2); state.Emitter=new(0,20);
        state.Slot=state.QuerySlot=1;state.Clip=1;state.Speed=state.Weight=1;
        state.ThrowAt=-1;
    }
    private static void Observe(ref LayerState s, AnimationLayerState? value, ulong tick)
    {
        s.QueryTick=checked((long)tick);
        if (value is not { } layer) throw new InvalidOperationException("Expected configured query layer.");
        var a=layer.Playback.State;s.QSlot=checked((int)layer.Slot);s.QMode=(int)layer.Mode;
        s.QMaskNodes=checked((int)layer.MaskNodes);s.QWeight=layer.Weight;s.QTargetWeight=layer.TargetWeight;
        s.QClip=a.Clip??-1;s.QTime=a.Time;s.QSpeed=a.Speed;s.QDuration=a.Duration;
        s.QLoop=a.Loop?1:0;s.QPlaying=a.Playing?1:0;s.QHasTransition=a.Transition.HasValue?1:0;
        s.QTransitionMode=layer.Playback.Mode is { } mode?(int)mode:-1;s.QProgress=layer.Playback.Progress??-1;
        s.QStartTick=0;s.QDurationTicks=s.QElapsedTicks=s.QSourceFrozen=0;
        if (a.Transition is { } transition) {
            s.QStartTick=checked((long)transition.StartTick);s.QDurationTicks=checked((int)transition.DurationTicks);
            s.QElapsedTicks=checked((int)transition.ElapsedTicks);s.QSourceFrozen=transition.SourceFrozen?1:0;
        }
        s.QHasWeightTransition=layer.WeightTransition.HasValue?1:0;
        s.QWeightStartTick=0;s.QWeightDurationTicks=s.QWeightElapsedTicks=0;s.QWeightSource=s.QWeightTarget=0;
        if (layer.WeightTransition is { } ramp) {
            s.QWeightStartTick=checked((long)ramp.StartTick);s.QWeightDurationTicks=checked((int)ramp.DurationTicks);
            s.QWeightElapsedTicks=checked((int)ramp.ElapsedTicks);s.QWeightSource=ramp.Source;s.QWeightTarget=ramp.Target;
        }
    }
    private static void Set(ref LayerState s,GameContext c,EntityId id,uint slot)
        =>c.SetAnimationLayer(id,slot,s.Clip<0?null:s.Clip,s.Weight,(AnimationTransitionMode)s.TransitionMode,
            s.Time,s.Speed,s.Loop!=0,s.Playing!=0,checked((uint)s.BlendTicks),checked((uint)s.WeightBlendTicks));
    private static void Base(ref LayerState s,GameContext c)
        =>c.SetAnimation(s.Rig,s.Clip<0?null:s.Clip,(AnimationTransitionMode)s.TransitionMode,
            s.Time,s.Speed,s.Loop!=0,s.Playing!=0,checked((uint)s.BlendTicks));
    public override void Tick(ref LayerState state,GameContext context)
    {
        Stamp("tick");++state.Ticks;
        var before=context.GetAnimationLayer(state.Rig,checked((uint)state.QuerySlot));
        var second=context.GetAnimationLayer(state.Rig,2);
        var legacy=context.GetAnimation(state.Rig);var extended=context.GetAnimationExtended(state.Rig);
        if(!Nullable.Equals(legacy,extended?.State))throw new InvalidOperationException("Layer marker changed baseline projection.");
        state.QBaseMatches=1;state.QBaseClip=legacy?.Clip??-1;state.QBaseTime=legacy?.Time??-1;
        state.QBaseMode=extended?.Mode is { } baseMode?(int)baseMode:-1;state.QBaseProgress=extended?.Progress??-1;
        Observe(ref state,before,context.Tick);
        if(state.Serial!=state.AppliedSerial && (state.Mode!=6 || context.Tick>=(ulong)state.DelayUntil)) {
            switch(state.Mode) {
                case 1:case 6:Set(ref state,context,state.Rig,checked((uint)state.Slot));break;
                case 2:Set(ref state,context,state.Rig,checked((uint)state.Slot));Set(ref state,context,state.Rig,checked((uint)state.Slot));break;
                case 3:
                    if(context.GetAnimationLayer(state.Plain,1) is not null || context.GetAnimationLayer(state.Rig,4) is not null)
                        throw new InvalidOperationException("Unconfigured/non-rig layer unexpectedly present.");
                    state.NullProbe=1;break;
                case 4:context.GetAnimationLayer(new(0,999999),1);break;
                case 5:
                    for(int i=0;i<state.Count;++i)Set(ref state,context,new(state.Rig.High,state.Rig.Low+(ulong)i),1);
                    break;
                case 7:
                    Set(ref state,context,state.Rig,checked((uint)state.Slot));
                    context.MoveKinematic(state.Body,new(7,1,0),Quaternion.Identity,60);
                    state.LastVoice=context.PlaySound(state.Emitter);break;
                case 8:Base(ref state,context);break;
                case 9:Base(ref state,context);Set(ref state,context,state.Rig,checked((uint)state.Slot));break;
                case 10:Set(ref state,context,state.Rig,1);Set(ref state,context,state.Rig,2);break;
            }
            if(!Nullable.Equals(before,context.GetAnimationLayer(state.Rig,checked((uint)state.QuerySlot))) ||
               !Nullable.Equals(second,context.GetAnimationLayer(state.Rig,2)) ||
               !Nullable.Equals(legacy,context.GetAnimation(state.Rig)) ||
               !Nullable.Equals(extended,context.GetAnimationExtended(state.Rig)))
                throw new InvalidOperationException("Staged layer/base write became visible during Tick.");
            ++state.StagingChecks;state.AppliedSerial=state.Serial;
        }
        if(state.ThrowAt>=0 && context.Tick>=(ulong)state.ThrowAt)
            throw new InvalidOperationException("Intentional later layer gameplay failure.");
    }
}
