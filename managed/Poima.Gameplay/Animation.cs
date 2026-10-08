// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Declares the versioned animation service requirement before a game is constructed.</summary>
public interface IInertialAnimationGame { }
public enum AnimationTransitionMode : uint { Crossfade=0,Inertial=1 }
/// <summary>Legacy playback state plus the active transition mode and elapsed-duration progress.</summary>
public readonly record struct AnimationStateExtended(AnimationState State,AnimationTransitionMode? Mode,double? Progress);

// The baseline structs and callback signatures are deliberately unchanged.
[StructLayout(LayoutKind.Sequential)] internal struct NativeAnimationCommandV1
{ public uint Version,Bytes;public NativeAnimationCommand Command;public uint TransitionMode,Reserved; }
[StructLayout(LayoutKind.Sequential)] internal struct NativeAnimationStateV1
{ public uint Version,Bytes;public NativeAnimationState State;public uint TransitionMode,Reserved; }
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeAnimationServicesV1
{
    public NativeServices Baseline;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,NativeAnimationStateV1*,NativeError*,int> AnimationGetExtended;
    public delegate* unmanaged[Cdecl]<void*,NativeAnimationCommandV1*,NativeError*,int> AnimationSetExtended;
}
internal static unsafe class AnimationServiceAbi
{
    internal const uint RequiredBytes=192;
    internal static bool LayoutValid()
    {
        NativeAnimationServicesV1 services=default;NativeAnimationCommandV1 command=default;NativeAnimationStateV1 state=default;
        return sizeof(NativeServices)==176 && sizeof(NativeAnimationServicesV1)==192 &&
            sizeof(NativeAnimationCommandV1)==64 && sizeof(NativeAnimationStateV1)==136 &&
            (byte*)&services.Baseline-(byte*)&services==0 &&
            (byte*)&services.AnimationGetExtended-(byte*)&services==176 && (byte*)&services.AnimationSetExtended-(byte*)&services==184 &&
            (byte*)&command.Command-(byte*)&command==8 && (byte*)&command.TransitionMode-(byte*)&command==56 && (byte*)&command.Reserved-(byte*)&command==60 &&
            (byte*)&state.State-(byte*)&state==8 && (byte*)&state.TransitionMode-(byte*)&state==128 && (byte*)&state.Reserved-(byte*)&state==132;
    }
    internal static NativeAnimationServicesV1* Validate(NativeServices* services)
    {
        // The extent is checked before either extension callback slot is read.
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Animation service ABI mismatch: animation_inertial_v1 requires epoch 7 with at least 192 bytes. Declare IInertialAnimationGame.");
        var extended=(NativeAnimationServicesV1*)services;
        if(extended->AnimationGetExtended==null || extended->AnimationSetExtended==null)
            throw new ArgumentException("Versioned animation service callback is absent.");
        return extended;
    }
}
public readonly unsafe ref partial struct GameContext
{
    // Strict playback validation for the new layer reply. Existing legacy getters
    // retain their established decoding behavior and signatures.
    private static void ValidateAnimationPlayback(EntityId entity,in NativeAnimationState state,uint mode)
    {
        var t=state.Transition;
        bool emptyTransition=t.StartTick==0 && t.DurationTicks==0 && t.ElapsedTicks==0 && t.Weight==0 &&
            t.SourceClip==-1 && t.SourceTime==0 && t.SourceSpeed==0 && t.SourceFrozen==0 && t.SourceLoop==0 && t.SourcePlaying==0;
        if(state.Entity!=entity || state.Reserved!=0 || state.Present>1 || state.Loop>1 || state.Playing>1 || state.TransitionPresent>1 || mode>1 ||
            state.Clip< -1 || !double.IsFinite(state.Time) || state.Time<0 || !double.IsFinite(state.Speed) || state.Speed<0 || state.Speed>8 ||
            !double.IsFinite(state.Duration) || state.Duration<0)
            throw new InvalidOperationException("Invalid versioned native animation layer playback.");
        if(state.Present==0)
        {
            if(state.Clip!=-1 || state.Time!=0 || state.Speed!=0 || state.Duration!=0 || state.Loop!=0 || state.Playing!=0 || state.TransitionPresent!=0 || mode!=0 || !emptyTransition)
                throw new InvalidOperationException("Missing animation layer has noncanonical playback.");
            return;
        }
        if((state.Clip==-1 && (state.Time!=0 || state.Duration!=0 || state.Playing!=0)) ||
            state.Time>state.Duration || (state.Duration==0 && state.Playing!=0) ||
            (state.Loop!=0 && state.Duration>0 && state.Time>=state.Duration) ||
            (state.Loop==0 && state.Time==state.Duration && state.Playing!=0))
            throw new InvalidOperationException("Invalid versioned native animation layer clock.");
        if(state.TransitionPresent==0)
        {
            if(mode!=0 || !emptyTransition)throw new InvalidOperationException("Inactive animation layer transition has noncanonical playback.");
            return;
        }
        if(t.DurationTicks is <1 or >3600 || t.ElapsedTicks>=t.DurationTicks || t.StartTick>ulong.MaxValue-t.ElapsedTicks ||
            !double.IsFinite(t.Weight) || t.Weight!=(double)t.ElapsedTicks/t.DurationTicks || t.SourceFrozen>1 || t.SourceLoop>1 || t.SourcePlaying>1 ||
            t.SourceClip< -1 || !double.IsFinite(t.SourceTime) || t.SourceTime<0 || !double.IsFinite(t.SourceSpeed) || t.SourceSpeed<0 || t.SourceSpeed>8 ||
            (mode==1 && t.SourceFrozen!=1) ||
            (t.SourceFrozen!=0 && (t.SourceClip!=-1 || t.SourceTime!=0 || t.SourceSpeed!=0 || t.SourceLoop!=0 || t.SourcePlaying!=0)) ||
            (t.SourceFrozen==0 && t.SourceClip==-1 && (t.SourceTime!=0 || t.SourcePlaying!=0)))
            throw new InvalidOperationException("Invalid versioned native animation layer clip transition.");
    }
    /// <summary>Reads committed animation state; writes staged by this callback are not visible yet.</summary>
    public AnimationStateExtended? GetAnimationExtended(EntityId entity)
    {
        var extended=AnimationServiceAbi.Validate(services);
        NativeAnimationStateV1 result=new(){Version=1,Bytes=136};NativeError error=default;
        Check(extended->AnimationGetExtended(services->Context,&entity,&result,&error),&error);
        if(result.Version!=1 || result.Bytes<136 || result.Reserved!=0 || result.TransitionMode>1 ||
            result.State.Reserved!=0 || result.State.Present>1 || result.State.TransitionPresent>1 ||
            (result.State.TransitionPresent==0 && result.TransitionMode!=0))
            throw new InvalidOperationException("Invalid versioned native animation state.");
        var state=DecodeAnimation(entity,result.State);
        if(state is null)
        {
            if(result.State.TransitionPresent!=0)throw new InvalidOperationException("Missing animation cannot have an active transition.");
            return null;
        }
        AnimationTransitionMode? mode=null;double? progress=null;
        if(result.State.TransitionPresent!=0)
        {
            var transition=result.State.Transition;
            if(transition.DurationTicks==0 || transition.ElapsedTicks>=transition.DurationTicks ||
                !double.IsFinite(transition.Weight) || transition.Weight!=(double)transition.ElapsedTicks/transition.DurationTicks ||
                (result.TransitionMode==1 && transition.SourceFrozen!=1))
                throw new InvalidOperationException("Invalid versioned native animation transition.");
            mode=(AnimationTransitionMode)result.TransitionMode;progress=transition.Weight;
        }
        return new(state.Value,mode,progress);
    }
    /// <summary>Stages a complete playback replacement through the separately negotiated animation service.</summary>
    // Zero/default also convert to enum: keep existing numeric-time calls on
    // the original overload when both signatures would otherwise apply.
    [System.Runtime.CompilerServices.OverloadResolutionPriority(-1)]
    public void SetAnimation(EntityId entity,int? clip,AnimationTransitionMode transitionMode,double time=0,double speed=1,bool loop=true,bool playing=true,uint blendTicks=0)
    {
        if(transitionMode is not AnimationTransitionMode.Crossfade and not AnimationTransitionMode.Inertial)
            throw new ArgumentOutOfRangeException(nameof(transitionMode));
        if(clip is <0)throw new ArgumentOutOfRangeException(nameof(clip),"Use null for the authored rest pose.");
        var extended=AnimationServiceAbi.Validate(services);
        NativeAnimationCommandV1 command=new(){Version=1,Bytes=64,TransitionMode=(uint)transitionMode,
            Command=new(){Entity=entity,Clip=clip ?? -1,Time=time,Speed=speed,Loop=loop ? 1u : 0u,Playing=playing ? 1u : 0u,BlendTicks=blendTicks}};
        NativeError error=default;Check(extended->AnimationSetExtended(services->Context,&command,&error),&error);
    }
}
