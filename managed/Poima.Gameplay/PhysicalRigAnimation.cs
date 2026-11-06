// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into an independent animation target player for authored physical drives.
/// This does not grant ordinary animation, IK or recovery-to-animation operations.</summary>
public interface IPhysicalRigAnimationGame : IPhysicalRigDriveGame { }
public enum PhysicalRigDriveSource : uint { Manual=0,Animation=1 }
public enum PhysicalRigTargetLimitPolicy : uint { Strict=0,Fit=1 }
/// <summary>Canonical source/fitted joint orientations and the explicit fitting diagnostics.
/// These are target intent, not the solved body pose or the preceding applied torque.</summary>
public readonly record struct PhysicalRigAnimationTarget(Quaterniond RequestedRotation,Quaterniond TargetRotation,
    double CorrectionRadians,double ChartCondition,bool HingeProjected,bool TwistClamped,bool SwingClamped);
/// <summary>Committed source selection. Manual has no animation sample. Read actual
/// effective target, strength and gains through GetPhysicalRigDrive.</summary>
public readonly record struct PhysicalRigDriveSourceState(EntityId Entity,string Joint,PhysicalRigDriveSource Source,
    PhysicalRigTargetLimitPolicy LimitPolicy,ulong SampledTick,PhysicalRigAnimationTarget? Sample);

[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePhysicalRigDriveSourceStateV1
{
    public uint Version,Bytes,Present,Flags;public EntityId Entity;public uint JointBytes,ReservedName;public fixed byte Joint[64];
    public uint Source,LimitPolicy;public ulong SampledTick;public Quaterniond RequestedRotation,TargetRotation;
    public double CorrectionRadians,ChartCondition;public ulong ReservedTail;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePhysicalRigDriveSourceCommandV1
{
    public uint Version,Bytes;public EntityId Entity;public byte* Joint;public uint JointBytes,Reserved;
    public uint Source,ReservedSource;public double Strength;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePhysicalRigAnimationServicesV1
{
    public NativePhysicalRigDriveServicesV1 Drive;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,NativeAnimationStateV1*,NativeError*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,uint,NativeAnimationLayerStateV1*,NativeError*,int> LayerGet;
    public delegate* unmanaged[Cdecl]<void*,NativeAnimationLayerCommandV1*,NativeError*,int> Set;
    public delegate* unmanaged[Cdecl]<void*,NativeAnimationRateCommandV1*,NativeError*,int> RateSet;
    public delegate* unmanaged[Cdecl]<void*,NativePhysicalRigDriveRequestV1*,NativePhysicalRigDriveSourceStateV1*,NativeError*,int> SourceGet;
    public delegate* unmanaged[Cdecl]<void*,NativePhysicalRigDriveSourceCommandV1*,NativeError*,int> SourceSet;
}
internal static unsafe class PhysicalRigAnimationServiceAbi
{
    internal const uint RequiredBytes=496;
    internal static bool LayoutValid()
    {
        NativePhysicalRigAnimationServicesV1 s=default;NativePhysicalRigDriveSourceStateV1 v=default;NativePhysicalRigDriveSourceCommandV1 c=default;
        return PhysicalRigDriveServiceAbi.LayoutValid() && AnimationServiceAbi.LayoutValid() && AnimationLayerServiceAbi.LayoutValid() && AnimationRateServiceAbi.LayoutValid() &&
            sizeof(NativePhysicalRigAnimationServicesV1)==496 && (byte*)&s.Drive-(byte*)&s==0 &&
            (byte*)&s.Get-(byte*)&s==448 && (byte*)&s.LayerGet-(byte*)&s==456 && (byte*)&s.Set-(byte*)&s==464 &&
            (byte*)&s.RateSet-(byte*)&s==472 && (byte*)&s.SourceGet-(byte*)&s==480 && (byte*)&s.SourceSet-(byte*)&s==488 &&
            sizeof(NativePhysicalRigDriveSourceStateV1)==208 && (byte*)&v.Entity-(byte*)&v==16 && (byte*)&v.JointBytes-(byte*)&v==32 &&
            (byte*)&v.ReservedName-(byte*)&v==36 && (byte*)v.Joint-(byte*)&v==40 && (byte*)&v.Source-(byte*)&v==104 &&
            (byte*)&v.LimitPolicy-(byte*)&v==108 && (byte*)&v.SampledTick-(byte*)&v==112 && (byte*)&v.RequestedRotation-(byte*)&v==120 &&
            (byte*)&v.TargetRotation-(byte*)&v==152 && (byte*)&v.CorrectionRadians-(byte*)&v==184 && (byte*)&v.ChartCondition-(byte*)&v==192 && (byte*)&v.ReservedTail-(byte*)&v==200 &&
            sizeof(NativePhysicalRigDriveSourceCommandV1)==56 && (byte*)&c.Entity-(byte*)&c==8 && (byte*)&c.Joint-(byte*)&c==24 &&
            (byte*)&c.JointBytes-(byte*)&c==32 && (byte*)&c.Reserved-(byte*)&c==36 && (byte*)&c.Source-(byte*)&c==40 &&
            (byte*)&c.ReservedSource-(byte*)&c==44 && (byte*)&c.Strength-(byte*)&c==48;
    }
    internal static NativePhysicalRigAnimationServicesV1* Validate(NativeServices* services)
    {
        // Check the whole known extent before touching any appended pointer.
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Physical animation requires services epoch 7 with at least 496 bytes. Declare IPhysicalRigAnimationGame.");
        _=PhysicalRigDriveServiceAbi.Validate(services);var api=(NativePhysicalRigAnimationServicesV1*)services;
        if(api->Get==null || api->LayerGet==null || api->Set==null || api->RateSet==null || api->SourceGet==null || api->SourceSet==null)
            throw new ArgumentException("Physical animation callbacks are absent.");
        return api;
    }
    internal static PhysicalRigDriveSourceState? Decode(NativePhysicalRigDriveSourceStateV1* value,EntityId entity,string joint,ulong tick)
    {
        if(value->Version!=1 || value->Bytes!=208 || value->Present>1 || (value->Flags&~15u)!=0 || value->Entity!=entity || value->ReservedName!=0 || value->ReservedTail!=0)
            throw new InvalidOperationException("Invalid native physical animation source header.");
        if(value->Present==0)
        {
            if(value->Flags!=0)throw new InvalidOperationException("Absent physical animation source has flags.");
            for(int i=32;i<208;++i)if(((byte*)value)[i]!=0)throw new InvalidOperationException("Absent physical animation source has payload.");
            return null;
        }
        if(entity==default || value->JointBytes!=(uint)joint.Length || value->Source>1 || value->LimitPolicy>1 ||
            value->SampledTick!=tick || tick>9007199254740991UL)
            throw new InvalidOperationException("Native physical animation source identity, policy or tick differs.");
        for(int i=0;i<64;++i)if(value->Joint[i]!=(i<joint.Length?(byte)joint[i]:(byte)0))
            throw new InvalidOperationException("Native physical animation joint name differs.");
        PhysicalRigAnimationTarget? sample=null;
        if(value->Source==0)
        {
            if(value->Flags!=0)throw new InvalidOperationException("Manual target source has animation flags.");
            for(int i=120;i<200;++i)if(((byte*)value)[i]!=0)throw new InvalidOperationException("Manual target source has an animation sample.");
        }
        else
        {
            if((value->Flags&1)==0 || (value->Flags&10)==10 || (value->LimitPolicy==0 && (value->Flags&14)!=0) ||
                !PhysicalRigDriveServiceAbi.Rotation(value->RequestedRotation) || !PhysicalRigDriveServiceAbi.Rotation(value->TargetRotation) ||
                !double.IsFinite(value->CorrectionRadians) || value->CorrectionRadians<0 || value->CorrectionRadians>Math.PI ||
                !double.IsFinite(value->ChartCondition) || value->ChartCondition<0 || value->ChartCondition>1.000001)
                throw new InvalidOperationException("Invalid native physical animation sample.");
            sample=new(value->RequestedRotation,value->TargetRotation,value->CorrectionRadians,value->ChartCondition,
                (value->Flags&2)!=0,(value->Flags&4)!=0,(value->Flags&8)!=0);
        }
        return new(entity,joint,(PhysicalRigDriveSource)value->Source,(PhysicalRigTargetLimitPolicy)value->LimitPolicy,value->SampledTick,sample);
    }
}
public readonly unsafe ref partial struct GameContext
{
    private NativePhysicalRigAnimationServicesV1* PhysicalAnimationApi()
    {
        RequireFeature(GameplayRequiredFeatures.PhysicalRigAnimation,nameof(IPhysicalRigAnimationGame));
        return PhysicalRigAnimationServiceAbi.Validate(services);
    }
    /// <summary>Reads the independent committed target-player base clock, including an active transition.</summary>
    public AnimationStateExtended? GetPhysicalRigAnimation(EntityId entity)
    {
        var api=PhysicalAnimationApi();if(entity==default)throw new ArgumentException("Physical rig entity is absent.",nameof(entity));
        NativeAnimationStateV1 result=new(){Version=1,Bytes=136};NativeError error=default;
        Check(api->Get(services->Context,&entity,&result,&error),&error);
        if(result.Version!=1 || result.Bytes!=136 || result.Reserved!=0)throw new InvalidOperationException("Invalid physical animation playback header.");
        ValidateAnimationPlayback(entity,result.State,result.TransitionMode);var playback=DecodeAnimation(entity,result.State);
        if(playback is null)return null;
        return new(playback.Value,result.State.TransitionPresent!=0?(AnimationTransitionMode)result.TransitionMode:null,
            result.State.TransitionPresent!=0?result.State.Transition.Weight:null);
    }
    /// <summary>Reads one configured target layer. Ordinary animation layers are independent.</summary>
    public AnimationLayerState? GetPhysicalRigAnimationLayer(EntityId entity,uint slot)
    {
        var api=PhysicalAnimationApi();if(entity==default)throw new ArgumentException("Physical rig entity is absent.",nameof(entity));
        if(slot is <1 or >4)throw new ArgumentOutOfRangeException(nameof(slot));
        NativeAnimationLayerStateV1 result=new(){Version=1,Bytes=200};NativeError error=default;
        Check(api->LayerGet(services->Context,&entity,slot,&result,&error),&error);
        if(result.Bytes!=200)throw new InvalidOperationException("Invalid physical animation layer size.");
        return DecodeAnimationLayer(entity,slot,result);
    }
    public PhysicalRigDriveSourceState? GetPhysicalRigDriveSource(EntityId entity,string joint)
    {
        var api=PhysicalAnimationApi();if(entity==default)throw new ArgumentException("Physical rig entity is absent.",nameof(entity));PhysicalRigServiceAbi.Name(joint);
        byte* name=stackalloc byte[64];for(int i=0;i<joint.Length;++i)name[i]=(byte)joint[i];
        NativePhysicalRigDriveRequestV1 request=new(){Version=1,Bytes=40,Entity=entity,Joint=name,JointBytes=(uint)joint.Length};
        NativePhysicalRigDriveSourceStateV1 result=new(){Version=1,Bytes=208};NativeError error=default;
        Check(api->SourceGet(services->Context,&request,&result,&error),&error);
        return PhysicalRigAnimationServiceAbi.Decode(&result,entity,joint,Tick);
    }
    /// <summary>Stages a target-player base replacement. Physical pose ownership is unchanged.</summary>
    public void SetPhysicalRigAnimation(EntityId entity,int? clip,AnimationTransitionMode transitionMode=AnimationTransitionMode.Crossfade,
        double time=0,double speed=1,bool loop=true,bool playing=true,uint blendTicks=0)
        =>SetPhysicalAnimationPlayback(entity,0,clip,1,transitionMode,time,speed,loop,playing,blendTicks,0);
    public void SetPhysicalRigAnimationLayer(EntityId entity,uint slot,int? clip,double weight,AnimationTransitionMode transitionMode=AnimationTransitionMode.Crossfade,
        double time=0,double speed=1,bool loop=true,bool playing=true,uint blendTicks=0,uint weightBlendTicks=0)
    {
        if(slot is <1 or >4)throw new ArgumentOutOfRangeException(nameof(slot));
        SetPhysicalAnimationPlayback(entity,slot,clip,weight,transitionMode,time,speed,loop,playing,blendTicks,weightBlendTicks);
    }
    private void SetPhysicalAnimationPlayback(EntityId entity,uint slot,int? clip,double weight,AnimationTransitionMode mode,
        double time,double speed,bool loop,bool playing,uint blendTicks,uint weightBlendTicks)
    {
        var api=PhysicalAnimationApi();if(entity==default)throw new ArgumentException("Physical rig entity is absent.",nameof(entity));
        if(clip is <0)throw new ArgumentOutOfRangeException(nameof(clip),"Use null for the authored rest pose.");
        if(mode is not AnimationTransitionMode.Crossfade and not AnimationTransitionMode.Inertial)throw new ArgumentOutOfRangeException(nameof(mode));
        if(!double.IsFinite(time) || time<0 || time>1e9)throw new ArgumentOutOfRangeException(nameof(time));
        if(!double.IsFinite(speed) || speed<0 || speed>8)throw new ArgumentOutOfRangeException(nameof(speed));
        if(!UnitWeight(weight))throw new ArgumentOutOfRangeException(nameof(weight));
        if(blendTicks>3600)throw new ArgumentOutOfRangeException(nameof(blendTicks));
        if(weightBlendTicks>3600)throw new ArgumentOutOfRangeException(nameof(weightBlendTicks));
        NativeAnimationLayerCommandV1 command=new(){Version=1,Bytes=80,Slot=slot,TransitionMode=(uint)mode,Weight=weight,WeightBlendTicks=weightBlendTicks,
            Command=new(){Entity=entity,Clip=clip??-1,Time=time,Speed=speed,Loop=loop?1u:0u,Playing=playing?1u:0u,BlendTicks=blendTicks}};
        NativeError error=default;Check(api->Set(services->Context,&command,&error),&error);
    }
    /// <summary>Changes target base speed without seeking or restarting a fade.</summary>
    public void SetPhysicalRigAnimationSpeed(EntityId entity,double speed)=>SetPhysicalAnimationRate(entity,0,speed);
    public void SetPhysicalRigAnimationLayerSpeed(EntityId entity,uint slot,double speed)
    {
        if(slot is <1 or >4)throw new ArgumentOutOfRangeException(nameof(slot));SetPhysicalAnimationRate(entity,slot,speed);
    }
    private void SetPhysicalAnimationRate(EntityId entity,uint layer,double speed)
    {
        var api=PhysicalAnimationApi();if(entity==default)throw new ArgumentException("Physical rig entity is absent.",nameof(entity));
        if(!double.IsFinite(speed) || speed<0 || speed>8)throw new ArgumentOutOfRangeException(nameof(speed));
        NativeAnimationRateCommandV1 command=new(){Version=1,Bytes=40,Entity=entity,Layer=layer,Speed=speed};
        NativeError error=default;Check(api->RateSet(services->Context,&command,&error),&error);
    }
    /// <summary>Stages per-joint source and strength. Manual holds the current effective
    /// target; Animation samples on the next successful tick. Neither activates physics.</summary>
    public void SetPhysicalRigDriveSource(EntityId entity,string joint,PhysicalRigDriveSource source,double strength)
    {
        var api=PhysicalAnimationApi();if(entity==default)throw new ArgumentException("Physical rig entity is absent.",nameof(entity));PhysicalRigServiceAbi.Name(joint);
        if(source is not PhysicalRigDriveSource.Manual and not PhysicalRigDriveSource.Animation)throw new ArgumentOutOfRangeException(nameof(source));
        if(!UnitWeight(strength))throw new ArgumentOutOfRangeException(nameof(strength));
        byte* name=stackalloc byte[64];for(int i=0;i<joint.Length;++i)name[i]=(byte)joint[i];
        NativePhysicalRigDriveSourceCommandV1 command=new(){Version=1,Bytes=56,Entity=entity,Joint=name,JointBytes=(uint)joint.Length,Source=(uint)source,Strength=strength};
        NativeError error=default;Check(api->SourceSet(services->Context,&command,&error),&error);
    }
}
