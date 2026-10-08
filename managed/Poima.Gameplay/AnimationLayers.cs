// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into masked-layer and inertial services before game construction.</summary>
public interface IMaskedAnimationGame : IInertialAnimationGame { }
public enum AnimationLayerMode : uint { Override=0,Additive=1 }
public readonly record struct AnimationWeightTransition(ulong StartTick,uint DurationTicks,uint ElapsedTicks,double Source,double Target);
public readonly record struct AnimationLayerState(uint Slot,AnimationLayerMode Mode,AnimationStateExtended Playback,
    double Weight,double TargetWeight,uint MaskNodes,AnimationWeightTransition? WeightTransition);

[StructLayout(LayoutKind.Sequential)] internal struct NativeAnimationLayerCommandV1
{
    public uint Version,Bytes;public NativeAnimationCommand Command;public uint TransitionMode,Slot;
    public double Weight;public uint WeightBlendTicks,Reserved;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeAnimationLayerStateV1
{
    public uint Version,Bytes;public NativeAnimationState State;public uint TransitionMode,Slot,LayerMode,MaskNodes;
    public double Weight,TargetWeight;public ulong WeightStartTick;public uint WeightDurationTicks,WeightElapsedTicks;
    public double WeightSource,WeightTarget;public uint WeightTransitionPresent,Reserved;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeAnimationLayerServicesV1
{
    public NativeAnimationServicesV1 Animation;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,uint,NativeAnimationLayerStateV1*,NativeError*,int> AnimationLayerGet;
    public delegate* unmanaged[Cdecl]<void*,NativeAnimationLayerCommandV1*,NativeError*,int> AnimationLayerSet;
}
internal static unsafe class AnimationLayerServiceAbi
{
    internal const uint RequiredBytes=208;
    internal static bool LayoutValid()
    {
        NativeAnimationLayerServicesV1 services=default;NativeAnimationLayerCommandV1 command=default;NativeAnimationLayerStateV1 state=default;
        return AnimationServiceAbi.LayoutValid() && sizeof(NativeAnimationLayerServicesV1)==208 && sizeof(NativeAnimationLayerCommandV1)==80 && sizeof(NativeAnimationLayerStateV1)==200 &&
            (byte*)&services.Animation-(byte*)&services==0 && (byte*)&services.AnimationLayerGet-(byte*)&services==192 && (byte*)&services.AnimationLayerSet-(byte*)&services==200 &&
            (byte*)&command.Command-(byte*)&command==8 && (byte*)&command.TransitionMode-(byte*)&command==56 && (byte*)&command.Slot-(byte*)&command==60 &&
            (byte*)&command.Weight-(byte*)&command==64 && (byte*)&command.WeightBlendTicks-(byte*)&command==72 && (byte*)&command.Reserved-(byte*)&command==76 &&
            (byte*)&state.State-(byte*)&state==8 && (byte*)&state.TransitionMode-(byte*)&state==128 && (byte*)&state.Slot-(byte*)&state==132 &&
            (byte*)&state.LayerMode-(byte*)&state==136 && (byte*)&state.MaskNodes-(byte*)&state==140 && (byte*)&state.Weight-(byte*)&state==144 &&
            (byte*)&state.TargetWeight-(byte*)&state==152 && (byte*)&state.WeightStartTick-(byte*)&state==160 && (byte*)&state.WeightDurationTicks-(byte*)&state==168 &&
            (byte*)&state.WeightElapsedTicks-(byte*)&state==172 && (byte*)&state.WeightSource-(byte*)&state==176 && (byte*)&state.WeightTarget-(byte*)&state==184 &&
            (byte*)&state.WeightTransitionPresent-(byte*)&state==192 && (byte*)&state.Reserved-(byte*)&state==196;
    }
    internal static NativeAnimationLayerServicesV1* Validate(NativeServices* services)
    {
        // No tail slot is read until its complete prefix has been admitted.
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Animation layer service ABI mismatch: animation_layers_v1 requires epoch 7 with at least 208 bytes. Declare IMaskedAnimationGame.");
        _=AnimationServiceAbi.Validate(services);
        var extended=(NativeAnimationLayerServicesV1*)services;
        if(extended->AnimationLayerGet==null || extended->AnimationLayerSet==null)
            throw new ArgumentException("Versioned animation layer service callback is absent.");
        return extended;
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Reads one configured layer's committed state; this Tick's staged writes are not visible.</summary>
    public AnimationLayerState? GetAnimationLayer(EntityId entity,uint slot)
    {
        if(slot is <1 or >4)throw new ArgumentOutOfRangeException(nameof(slot));
        var extended=AnimationLayerServiceAbi.Validate(services);
        NativeAnimationLayerStateV1 result=new(){Version=1,Bytes=200};NativeError error=default;
        Check(extended->AnimationLayerGet(services->Context,&entity,slot,&result,&error),&error);
        if(result.Version!=1 || result.Bytes<200 || result.Reserved!=0 || result.Slot!=slot || result.LayerMode>1 || result.MaskNodes>10000 ||
            result.WeightTransitionPresent>1 || !UnitWeight(result.Weight) || !UnitWeight(result.TargetWeight) || !UnitWeight(result.WeightSource) || !UnitWeight(result.WeightTarget))
            throw new InvalidOperationException("Invalid versioned native animation layer state.");
        ValidateAnimationPlayback(entity,result.State,result.TransitionMode);
        bool noRamp=result.WeightStartTick==0 && result.WeightDurationTicks==0 && result.WeightElapsedTicks==0 && result.WeightSource==0 && result.WeightTarget==0;
        if(result.State.Present==0)
        {
            if(result.LayerMode!=0 || result.MaskNodes!=0 || result.Weight!=0 || result.TargetWeight!=0 || result.WeightTransitionPresent!=0 || !noRamp)
                throw new InvalidOperationException("Missing animation layer has noncanonical metadata.");
            return null;
        }
        AnimationWeightTransition? ramp=null;
        if(result.WeightTransitionPresent!=0)
        {
            if(result.WeightDurationTicks is <1 or >3600 || result.WeightElapsedTicks>=result.WeightDurationTicks ||
                result.WeightStartTick>ulong.MaxValue-result.WeightElapsedTicks || result.TargetWeight!=result.WeightTarget ||
                Math.Abs(result.Weight-(result.WeightSource+(result.WeightTarget-result.WeightSource)*((double)result.WeightElapsedTicks/result.WeightDurationTicks)))>1e-12)
                throw new InvalidOperationException("Invalid versioned native animation layer weight transition.");
            ramp=new(result.WeightStartTick,result.WeightDurationTicks,result.WeightElapsedTicks,result.WeightSource,result.WeightTarget);
        }
        else if(!noRamp || result.Weight!=result.TargetWeight)
            throw new InvalidOperationException("Inactive animation layer weight transition has noncanonical metadata.");
        var playback=DecodeAnimation(entity,result.State)!.Value;
        AnimationTransitionMode? mode=result.State.TransitionPresent!=0 ? (AnimationTransitionMode)result.TransitionMode : null;
        double? progress=result.State.TransitionPresent!=0 ? result.State.Transition.Weight : null;
        return new(slot,(AnimationLayerMode)result.LayerMode,new(playback,mode,progress),result.Weight,result.TargetWeight,result.MaskNodes,ramp);
    }
    /// <summary>Stages a complete configured-layer playback and target-weight replacement. Frozen masks and references are unchanged.</summary>
    public void SetAnimationLayer(EntityId entity,uint slot,int? clip,double weight,AnimationTransitionMode transitionMode=AnimationTransitionMode.Crossfade,
        double time=0,double speed=1,bool loop=true,bool playing=true,uint blendTicks=0,uint weightBlendTicks=0)
    {
        if(slot is <1 or >4)throw new ArgumentOutOfRangeException(nameof(slot));
        if(clip is <0)throw new ArgumentOutOfRangeException(nameof(clip),"Use null for the authored rest pose.");
        if(!UnitWeight(weight))throw new ArgumentOutOfRangeException(nameof(weight));
        if(transitionMode is not AnimationTransitionMode.Crossfade and not AnimationTransitionMode.Inertial)throw new ArgumentOutOfRangeException(nameof(transitionMode));
        if(!double.IsFinite(time) || time<0 || time>1e9)throw new ArgumentOutOfRangeException(nameof(time));
        if(!double.IsFinite(speed) || speed<0 || speed>8)throw new ArgumentOutOfRangeException(nameof(speed));
        if(blendTicks>3600)throw new ArgumentOutOfRangeException(nameof(blendTicks));
        if(weightBlendTicks>3600)throw new ArgumentOutOfRangeException(nameof(weightBlendTicks));
        var extended=AnimationLayerServiceAbi.Validate(services);
        NativeAnimationLayerCommandV1 command=new(){Version=1,Bytes=80,TransitionMode=(uint)transitionMode,Slot=slot,Weight=weight,WeightBlendTicks=weightBlendTicks,
            Command=new(){Entity=entity,Clip=clip ?? -1,Time=time,Speed=speed,Loop=loop ? 1u : 0u,Playing=playing ? 1u : 0u,BlendTicks=blendTicks}};
        NativeError error=default;Check(extended->AnimationLayerSet(services->Context,&command,&error),&error);
    }
    private static bool UnitWeight(double value)=>double.IsFinite(value) && value>=0 && value<=1;
}
