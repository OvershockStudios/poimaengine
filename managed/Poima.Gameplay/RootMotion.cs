// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into independent Tick-only root-motion controls before construction.</summary>
public interface IRootMotionGame { }
public enum RootMotionMode : uint { Extract=0,Character=1 }
/// <summary>Persistent enable control; source, actor, mode and yaw are frozen authored bindings.</summary>
public readonly record struct RootMotionCommand(EntityId Entity,bool Enabled=true);
/// <summary>Immutable diagnostics from the last committed physical interval. Reads never advance or consume motion.</summary>
public readonly record struct RootMotionState(EntityId Entity,EntityId Actor,uint Node,RootMotionMode Mode,
    bool Enabled,bool Yaw,ulong FromTick,ulong ToTick,Vector3d LocalDelta,double YawDelta,
    Vector3d WorldRequested,Vector3d WorldApplied,double AppliedYaw,double TranslationError);
[StructLayout(LayoutKind.Sequential)] internal struct NativeRootMotionCommandV1
{
    public uint Version,Bytes;public EntityId Entity;public uint Enabled,Reserved;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeRootMotionStateV1
{
    public uint Version,Bytes,Present,Reserved;public EntityId Entity,Actor;
    public uint Node,Mode,Enabled,Yaw;public ulong FromTick,ToTick;
    public Vector3d LocalDelta;public double YawDelta;public Vector3d WorldRequested,WorldApplied;
    public double AppliedYaw,TranslationError;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeRootMotionServicesV1
{
    public NativeTwoBoneIkServicesV1 Ik;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,NativeRootMotionStateV1*,NativeError*,int> RootMotionGet;
    public delegate* unmanaged[Cdecl]<void*,NativeRootMotionCommandV1*,NativeError*,int> RootMotionSet;
}
internal static unsafe class RootMotionServiceAbi
{
    internal const uint RequiredBytes=304;
    internal static NativeRootMotionServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Gameplay root motion requires services epoch 7 with at least 304 bytes.");
        var extended=(NativeRootMotionServicesV1*)services;
        if(extended->RootMotionGet==null || extended->RootMotionSet==null)throw new ArgumentException("Gameplay root motion callback is absent.");
        return extended;
    }
    internal static bool LayoutValid()
    {
        NativeRootMotionServicesV1 services=default;NativeRootMotionCommandV1 command=default;NativeRootMotionStateV1 state=default;
        return sizeof(NativeRootMotionServicesV1)==304 && sizeof(NativeRootMotionCommandV1)==32 && sizeof(NativeRootMotionStateV1)==176 &&
            (byte*)&services.RootMotionGet-(byte*)&services==288 && (byte*)&services.RootMotionSet-(byte*)&services==296 &&
            (byte*)&command.Entity-(byte*)&command==8 && (byte*)&command.Enabled-(byte*)&command==24 && (byte*)&command.Reserved-(byte*)&command==28 &&
            (byte*)&state.Present-(byte*)&state==8 && (byte*)&state.Reserved-(byte*)&state==12 && (byte*)&state.Entity-(byte*)&state==16 && (byte*)&state.Actor-(byte*)&state==32 &&
            (byte*)&state.Node-(byte*)&state==48 && (byte*)&state.Mode-(byte*)&state==52 && (byte*)&state.Enabled-(byte*)&state==56 && (byte*)&state.Yaw-(byte*)&state==60 &&
            (byte*)&state.FromTick-(byte*)&state==64 && (byte*)&state.ToTick-(byte*)&state==72 && (byte*)&state.LocalDelta-(byte*)&state==80 && (byte*)&state.YawDelta-(byte*)&state==104 &&
            (byte*)&state.WorldRequested-(byte*)&state==112 && (byte*)&state.WorldApplied-(byte*)&state==136 && (byte*)&state.AppliedYaw-(byte*)&state==160 && (byte*)&state.TranslationError-(byte*)&state==168;
    }
    private static bool Zero(Vector3d v)=>v.X==0 && v.Y==0 && v.Z==0;
    internal static RootMotionState? Decode(NativeRootMotionStateV1* result,EntityId entity)
    {
        if(result->Version!=1 || result->Bytes!=176 || result->Present>1 || result->Reserved!=0)
            throw new InvalidOperationException("Invalid native root motion state header.");
        if(result->Present==0)
        {
            foreach(byte value in new ReadOnlySpan<byte>((byte*)result+8,168))
                if(value!=0)throw new InvalidOperationException("Absent native root motion state is not canonical.");
            return null;
        }
        if(result->Entity!=entity || result->Entity==default || result->Node>=10000 || result->Mode>1 || result->Enabled>1 || result->Yaw>1 ||
            result->FromTick>result->ToTick || result->ToTick-result->FromTick>1 ||
            !TwoBoneIkServiceAbi.Finite(result->LocalDelta) || result->LocalDelta.Y!=0 ||
            !TwoBoneIkServiceAbi.Finite(result->WorldRequested) || (result->Mode==1 && result->WorldRequested.Y!=0) || !TwoBoneIkServiceAbi.Finite(result->WorldApplied) ||
            !double.IsFinite(result->YawDelta) || !double.IsFinite(result->AppliedYaw) || !double.IsFinite(result->TranslationError) || result->TranslationError<0 ||
            (result->Yaw==0 && result->YawDelta!=0) ||
            (result->FromTick==result->ToTick && (!Zero(result->LocalDelta) || result->YawDelta!=0 || !Zero(result->WorldRequested) || !Zero(result->WorldApplied) || result->AppliedYaw!=0 || result->TranslationError!=0)) ||
            (result->Enabled==0 && (!Zero(result->LocalDelta) || result->YawDelta!=0 || !Zero(result->WorldRequested) || !Zero(result->WorldApplied) || result->AppliedYaw!=0 || result->TranslationError!=0)) ||
            (result->Mode==0 && (result->Actor!=default || !Zero(result->WorldApplied) || result->AppliedYaw!=0 || result->TranslationError!=0)) ||
            (result->Mode==1 && result->Actor==default))
            throw new InvalidOperationException("Invalid native root motion state.");
        return new(result->Entity,result->Actor,result->Node,(RootMotionMode)result->Mode,result->Enabled!=0,result->Yaw!=0,
            result->FromTick,result->ToTick,result->LocalDelta,result->YawDelta,result->WorldRequested,result->WorldApplied,result->AppliedYaw,result->TranslationError);
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Reads the cached committed interval during Tick, at most 1,024 admitted reads. Staged enable writes remain invisible.</summary>
    public RootMotionState? GetRootMotion(EntityId entity)
    {
        RequireFeature(GameplayRequiredFeatures.RootMotion,nameof(IRootMotionGame));
        var extended=RootMotionServiceAbi.Validate(services);
        NativeRootMotionStateV1 result=new(){Version=1,Bytes=176};NativeError error=default;
        Check(extended->RootMotionGet(services->Context,&entity,&result,&error),&error);
        return RootMotionServiceAbi.Decode(&result,entity);
    }
    /// <summary>Stages persistent enable controls, at most 64 admitted writes per Tick. Pose stripping remains active when disabled.</summary>
    public void SetRootMotion(EntityId entity,bool enabled=true)=>SetRootMotion(new RootMotionCommand(entity,enabled));
    public void SetRootMotion(in RootMotionCommand source)
    {
        RequireFeature(GameplayRequiredFeatures.RootMotion,nameof(IRootMotionGame));
        var extended=RootMotionServiceAbi.Validate(services);
        NativeRootMotionCommandV1 command=new(){Version=1,Bytes=32,Entity=source.Entity,Enabled=source.Enabled ? 1u : 0u};
        NativeError error=default;Check(extended->RootMotionSet(services->Context,&command,&error),&error);
    }
}
