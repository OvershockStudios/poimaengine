// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into independent Tick-only two-bone IK controls before construction.</summary>
public interface ITwoBoneIkGame { }
public enum IkSpace : uint { World=0,Rig=1 }
/// <summary>Complete controls for a frozen chain slot. Target and pole are points in Space.</summary>
public readonly record struct IkCommand(EntityId Entity,uint Slot,Vector3d Target,Vector3d Pole,
    bool Enabled=true,IkSpace Space=IkSpace.World,double Weight=1);
/// <summary>Committed controls and cached diagnostics; reading does not solve or advance animation.</summary>
public readonly record struct IkState(EntityId Entity,uint Slot,uint Root,uint Middle,uint End,
    bool Enabled,IkSpace Space,double Weight,Vector3d Target,Vector3d Pole,Vector3d EndPosition,
    double TargetError,bool Clamped,bool PoleFallback);

[StructLayout(LayoutKind.Sequential)] internal struct NativeTwoBoneIkCommandV1
{
    public uint Version,Bytes;public EntityId Entity;
    public uint Slot,Enabled,Space,Reserved;public double Weight;public Vector3d Target,Pole;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeTwoBoneIkStateV1
{
    public uint Version,Bytes,Present,Slot;public EntityId Entity;
    public uint Root,Middle,End,Enabled,Space,Reserved;public double Weight;
    public Vector3d Target,Pole,EndPosition;public double TargetError;
    public uint Clamped,PoleFallback;public ulong ReservedTail;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeTwoBoneIkServicesV1
{
    public NativeHitRegionServicesV1 Regions;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,uint,NativeTwoBoneIkStateV1*,NativeError*,int> IkGet;
    public delegate* unmanaged[Cdecl]<void*,NativeTwoBoneIkCommandV1*,NativeError*,int> IkSet;
}
internal static unsafe class TwoBoneIkServiceAbi
{
    internal const uint RequiredBytes=288;
    internal static NativeTwoBoneIkServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Gameplay two-bone IK requires services epoch 7 with at least 288 bytes.");
        var extended=(NativeTwoBoneIkServicesV1*)services;
        if(extended->IkGet==null || extended->IkSet==null)throw new ArgumentException("Gameplay two-bone IK callback is absent.");
        return extended;
    }
    internal static bool LayoutValid()
    {
        NativeTwoBoneIkServicesV1 services=default;NativeTwoBoneIkCommandV1 command=default;NativeTwoBoneIkStateV1 state=default;
        return sizeof(NativeTwoBoneIkServicesV1)==288 && sizeof(NativeTwoBoneIkCommandV1)==96 && sizeof(NativeTwoBoneIkStateV1)==160 &&
            (byte*)&services.IkGet-(byte*)&services==272 && (byte*)&services.IkSet-(byte*)&services==280 &&
            (byte*)&command.Entity-(byte*)&command==8 && (byte*)&command.Slot-(byte*)&command==24 &&
            (byte*)&command.Enabled-(byte*)&command==28 && (byte*)&command.Space-(byte*)&command==32 &&
            (byte*)&command.Reserved-(byte*)&command==36 && (byte*)&command.Weight-(byte*)&command==40 &&
            (byte*)&command.Target-(byte*)&command==48 && (byte*)&command.Pole-(byte*)&command==72 &&
            (byte*)&state.Present-(byte*)&state==8 && (byte*)&state.Slot-(byte*)&state==12 &&
            (byte*)&state.Entity-(byte*)&state==16 && (byte*)&state.Root-(byte*)&state==32 &&
            (byte*)&state.Middle-(byte*)&state==36 && (byte*)&state.End-(byte*)&state==40 &&
            (byte*)&state.Enabled-(byte*)&state==44 && (byte*)&state.Space-(byte*)&state==48 &&
            (byte*)&state.Reserved-(byte*)&state==52 && (byte*)&state.Weight-(byte*)&state==56 &&
            (byte*)&state.Target-(byte*)&state==64 && (byte*)&state.Pole-(byte*)&state==88 &&
            (byte*)&state.EndPosition-(byte*)&state==112 && (byte*)&state.TargetError-(byte*)&state==136 &&
            (byte*)&state.Clamped-(byte*)&state==144 && (byte*)&state.PoleFallback-(byte*)&state==148 &&
            (byte*)&state.ReservedTail-(byte*)&state==152;
    }
    internal static bool Finite(Vector3d value)=>double.IsFinite(value.X) && double.IsFinite(value.Y) && double.IsFinite(value.Z);
    internal static bool Point(Vector3d value)=>Finite(value) && Math.Abs(value.X)<=1e6 && Math.Abs(value.Y)<=1e6 && Math.Abs(value.Z)<=1e6;
    internal static IkState? Decode(NativeTwoBoneIkStateV1* result,EntityId entity,uint slot)
    {
        if(result->Version!=1 || result->Bytes!=160 || result->Present>1 || result->Reserved!=0 || result->ReservedTail!=0)
            throw new InvalidOperationException("Invalid native two-bone IK state header.");
        if(result->Present==0)
        {
            foreach(byte value in new ReadOnlySpan<byte>((byte*)result+8,152))
                if(value!=0)throw new InvalidOperationException("Absent native two-bone IK state is not canonical.");
            return null;
        }
        if(result->Entity!=entity || result->Entity==default || result->Slot!=slot || result->Slot is <1 or >8 ||
            result->Root>=10000 || result->Middle>=10000 || result->End>=10000 ||
            result->Root==result->Middle || result->Root==result->End || result->Middle==result->End ||
            result->Enabled>1 || result->Space>1 || result->Clamped>1 || result->PoleFallback>1 ||
            !double.IsFinite(result->Weight) || result->Weight<0 || result->Weight>1 ||
            !Point(result->Target) || !Point(result->Pole) || !Finite(result->EndPosition) ||
            !double.IsFinite(result->TargetError) || result->TargetError<0)
            throw new InvalidOperationException("Invalid native two-bone IK state.");
        return new(result->Entity,result->Slot,result->Root,result->Middle,result->End,result->Enabled!=0,
            (IkSpace)result->Space,result->Weight,result->Target,result->Pole,result->EndPosition,
            result->TargetError,result->Clamped!=0,result->PoleFallback!=0);
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Reads committed controls and diagnostics during Tick, at most 1,024 admitted reads. Staged writes remain invisible.</summary>
    public IkState? GetTwoBoneIk(EntityId entity,uint slot)
    {
        RequireFeature(GameplayRequiredFeatures.TwoBoneIk,nameof(ITwoBoneIkGame));
        if(slot is <1 or >8)throw new ArgumentOutOfRangeException(nameof(slot));
        var extended=TwoBoneIkServiceAbi.Validate(services);
        NativeTwoBoneIkStateV1 result=new(){Version=1,Bytes=160};NativeError error=default;
        Check(extended->IkGet(services->Context,&entity,slot,&result,&error),&error);
        return TwoBoneIkServiceAbi.Decode(&result,entity,slot);
    }
    /// <summary>Stages complete persistent controls for one frozen chain. At most 64 admitted writes per Tick; publication follows the whole native batch.</summary>
    public void SetTwoBoneIk(EntityId entity,uint slot,Vector3d target,Vector3d pole,
        bool enabled=true,IkSpace space=IkSpace.World,double weight=1)
        =>SetTwoBoneIk(new(entity,slot,target,pole,enabled,space,weight));
    public void SetTwoBoneIk(in IkCommand source)
    {
        RequireFeature(GameplayRequiredFeatures.TwoBoneIk,nameof(ITwoBoneIkGame));
        if(source.Slot is <1 or >8)throw new ArgumentOutOfRangeException(nameof(source.Slot));
        if(source.Space is not IkSpace.World and not IkSpace.Rig)throw new ArgumentOutOfRangeException(nameof(source.Space));
        if(!double.IsFinite(source.Weight) || source.Weight<0 || source.Weight>1)throw new ArgumentOutOfRangeException(nameof(source.Weight));
        if(!TwoBoneIkServiceAbi.Point(source.Target))throw new ArgumentOutOfRangeException(nameof(source.Target));
        if(!TwoBoneIkServiceAbi.Point(source.Pole))throw new ArgumentOutOfRangeException(nameof(source.Pole));
        var extended=TwoBoneIkServiceAbi.Validate(services);
        NativeTwoBoneIkCommandV1 command=new(){Version=1,Bytes=96,Entity=source.Entity,Slot=source.Slot,
            Enabled=source.Enabled ? 1u : 0u,Space=(uint)source.Space,Weight=source.Weight,Target=source.Target,Pole=source.Pole};
        NativeError error=default;Check(extended->IkSet(services->Context,&command,&error),&error);
    }
}
