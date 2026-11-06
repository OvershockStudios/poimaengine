// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into guarded upright physical handback for an authored recovery profile.
/// This grants no drive or independent animation-target control.</summary>
public interface IPhysicalRecoveryGame : IPhysicalRigGame { }
public enum PhysicalRecoveryStatus : uint
{
    Locomotion=0,HistoryUnavailable=1,NotStanding=2,NotUpright=3,Moving=4,WrapperDisplaced=5,
    MissingSupport=6,MovingSupport=7,InvalidSupport=8,ProxyBlocked=9,MotorBlocked=10,
    QueryBudget=11,InvalidNativeResult=12,Stabilizing=13,Ready=14,Returned=15,SoleOutOfRange=16
}
/// <summary>Committed admission diagnostics. Support IDs identify native bodies, not
/// scene entities or solved support forces. A stale structural assessment reports
/// HistoryUnavailable while retaining its assessment revision and historical metrics.</summary>
public readonly record struct PhysicalRecoveryState(EntityId Entity,ulong SampledTick,ulong Generation,ulong AssessmentRevision,
    PhysicalRecoveryStatus Status,bool Physical,bool TransitionActive,uint StableTicks,uint RequiredStableTicks,
    ulong? LastAttemptTick,PhysicalRecoveryStatus LastAttemptResult,ulong? ReturnedTick,
    double MaximumLinearSpeed,double MaximumAngularSpeed,double MinimumUprightDot,double WrapperDistance,double WrapperAngle,
    uint? FirstSupportBody,uint? SecondSupportBody);
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePhysicalRecoveryStateV1
{
    public uint Version,Bytes,Present,Status;public EntityId Entity;public ulong SampledTick,Generation,AssessmentRevision;
    public uint Flags,StableTicks,RequiredStableTicks,LastAttemptResult;public ulong LastAttemptTick,ReturnedTick;
    public double MaximumLinearSpeed,MaximumAngularSpeed,MinimumUprightDot,WrapperDistance,WrapperAngle;
    public fixed uint SupportBodies[2];public uint SupportMask,Reserved;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativePhysicalRecoveryCommandV1
{ public uint Version,Bytes;public EntityId Entity;public ulong Reserved; }
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativePhysicalRecoveryServicesV1
{
    public NativePhysicalRigAnimationServicesV1 Animation;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,NativePhysicalRecoveryStateV1*,NativeError*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,NativePhysicalRecoveryCommandV1*,NativeError*,int> Request;
}
internal static unsafe class PhysicalRecoveryServiceAbi
{
    internal const uint RequiredBytes=512;
    internal static bool LayoutValid()
    {
        NativePhysicalRecoveryServicesV1 s=default;NativePhysicalRecoveryStateV1 v=default;NativePhysicalRecoveryCommandV1 c=default;
        return PhysicalRigAnimationServiceAbi.LayoutValid() && sizeof(NativePhysicalRecoveryServicesV1)==512 &&
            (byte*)&s.Animation-(byte*)&s==0 && (byte*)&s.Get-(byte*)&s==496 && (byte*)&s.Request-(byte*)&s==504 &&
            sizeof(NativePhysicalRecoveryCommandV1)==32 && (byte*)&c.Entity-(byte*)&c==8 && (byte*)&c.Reserved-(byte*)&c==24 &&
            sizeof(NativePhysicalRecoveryStateV1)==144 && (byte*)&v.Entity-(byte*)&v==16 && (byte*)&v.SampledTick-(byte*)&v==32 &&
            (byte*)&v.Generation-(byte*)&v==40 && (byte*)&v.AssessmentRevision-(byte*)&v==48 && (byte*)&v.Flags-(byte*)&v==56 &&
            (byte*)&v.StableTicks-(byte*)&v==60 && (byte*)&v.RequiredStableTicks-(byte*)&v==64 && (byte*)&v.LastAttemptResult-(byte*)&v==68 &&
            (byte*)&v.LastAttemptTick-(byte*)&v==72 && (byte*)&v.ReturnedTick-(byte*)&v==80 && (byte*)&v.MaximumLinearSpeed-(byte*)&v==88 &&
            (byte*)&v.MaximumAngularSpeed-(byte*)&v==96 && (byte*)&v.MinimumUprightDot-(byte*)&v==104 && (byte*)&v.WrapperDistance-(byte*)&v==112 &&
            (byte*)&v.WrapperAngle-(byte*)&v==120 && (byte*)v.SupportBodies-(byte*)&v==128 && (byte*)&v.SupportMask-(byte*)&v==136 && (byte*)&v.Reserved-(byte*)&v==140;
    }
    internal static NativePhysicalRecoveryServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Recovery requires services epoch 7 with at least 512 bytes. Declare IPhysicalRecoveryGame.");
        _=PhysicalRigServiceAbi.Validate(services);var api=(NativePhysicalRecoveryServicesV1*)services;
        if(api->Get==null || api->Request==null)throw new ArgumentException("Physical recovery callbacks are absent.");
        return api;
    }
    private static bool Bounded(double value,double low,double high)=>double.IsFinite(value)&&value>=low&&value<=high;
    internal static PhysicalRecoveryState? Decode(NativePhysicalRecoveryStateV1* v,EntityId entity,ulong tick)
    {
        if(v->Version!=1 || v->Bytes!=144 || v->Present>1 || v->Status>16 || v->Entity!=entity ||
            (v->Flags&~15u)!=0 || (v->SupportMask&~3u)!=0 || v->Reserved!=0)
            throw new InvalidOperationException("Invalid native physical recovery header.");
        if(v->Present==0)
        {
            if(v->Status!=0)throw new InvalidOperationException("Absent recovery has a status.");
            for(int i=32;i<144;++i)if(((byte*)v)[i]!=0)throw new InvalidOperationException("Absent recovery has payload.");
            return null;
        }
        if(entity==default || v->SampledTick!=tick || tick>9007199254740991UL || v->Generation>tick ||
            v->AssessmentRevision>9007199254740991UL || v->LastAttemptResult>16 || v->RequiredStableTicks is <2 or >60 || v->StableTicks>v->RequiredStableTicks ||
            !Bounded(v->MaximumLinearSpeed,0,2e6) || !Bounded(v->MaximumAngularSpeed,0,2e4) || !Bounded(v->MinimumUprightDot,-1.000001,1.000001) ||
            !Bounded(v->WrapperDistance,0,2e6) || !Bounded(v->WrapperAngle,0,3.141592653589794))
            throw new InvalidOperationException("Invalid native physical recovery identity, chronology or metrics.");
        bool physical=(v->Flags&1)!=0,transition=(v->Flags&2)!=0;
        if(v->SupportMask==2 || (transition && (physical || v->Generation==0 || (v->Flags&8)==0)) ||
            (v->Status==0 && (physical || v->StableTicks!=0)) ||
            (v->Status==14 && (!physical || v->StableTicks!=v->RequiredStableTicks || v->SupportMask!=3)) ||
            (v->Status==13 && (!physical || v->StableTicks==0 || v->StableTicks>=v->RequiredStableTicks || v->SupportMask!=3)))
            throw new InvalidOperationException("Recovery status, stability, transition or support prefix contradicts its ownership.");
        bool attempted=(v->Flags&4)!=0,returned=(v->Flags&8)!=0;
        if(attempted ? v->LastAttemptTick==0 || v->LastAttemptTick>tick : v->LastAttemptTick!=0 || v->LastAttemptResult!=1)
            throw new InvalidOperationException("Invalid recovery attempt history.");
        if(returned ? v->ReturnedTick==0 || v->ReturnedTick>tick || !attempted || v->ReturnedTick>v->LastAttemptTick || v->Generation==0 : v->ReturnedTick!=0 || v->Generation!=0)
            throw new InvalidOperationException("Invalid recovery transfer history.");
        if(v->LastAttemptResult==15 && (!returned || v->LastAttemptTick!=v->ReturnedTick))throw new InvalidOperationException("Returned recovery has no matching transfer.");
        for(int i=0;i<2;++i)
        {
            uint body=v->SupportBodies[i];bool present=(v->SupportMask&(1u<<i))!=0;
            // Jolt: slot bits0..22, sequence bits23..30, reserved broad-phase bit31.
            if(present ? (body&0x80000000u)!=0 || (body&0x007fffffu)>=4096 : body!=0)
                throw new InvalidOperationException("Invalid recovery support body identity.");
        }
        return new(entity,tick,v->Generation,v->AssessmentRevision,(PhysicalRecoveryStatus)v->Status,(v->Flags&1)!=0,(v->Flags&2)!=0,
            v->StableTicks,v->RequiredStableTicks,attempted?v->LastAttemptTick:null,(PhysicalRecoveryStatus)v->LastAttemptResult,returned?v->ReturnedTick:null,
            v->MaximumLinearSpeed,v->MaximumAngularSpeed,v->MinimumUprightDot,v->WrapperDistance,v->WrapperAngle,
            (v->SupportMask&1)!=0?v->SupportBodies[0]:null,(v->SupportMask&2)!=0?v->SupportBodies[1]:null);
    }
}
public readonly unsafe ref partial struct GameContext
{
    private NativePhysicalRecoveryServicesV1* PhysicalRecoveryApi()
    {
        RequireFeature(GameplayRequiredFeatures.PhysicalRecovery,nameof(IPhysicalRecoveryGame));
        return PhysicalRecoveryServiceAbi.Validate(services);
    }
    public PhysicalRecoveryState? GetPhysicalRecovery(EntityId entity)
    {
        var api=PhysicalRecoveryApi();if(entity==default)throw new ArgumentException("Physical rig entity is absent.",nameof(entity));
        NativePhysicalRecoveryStateV1 value=new(){Version=1,Bytes=144};NativeError error=default;
        Check(api->Get(services->Context,&entity,&value,&error),&error);return PhysicalRecoveryServiceAbi.Decode(&value,entity,Tick);
    }
    /// <summary>Stages a guarded request for the next physical step. Inspect the next
    /// committed state for success or refusal; Ready is not a promise across edits.</summary>
    public void RequestPhysicalRecovery(EntityId entity)
    {
        var api=PhysicalRecoveryApi();if(entity==default)throw new ArgumentException("Physical rig entity is absent.",nameof(entity));
        NativePhysicalRecoveryCommandV1 command=new(){Version=1,Bytes=32,Entity=entity};NativeError error=default;
        Check(api->Request(services->Context,&command,&error),&error);
    }
}
