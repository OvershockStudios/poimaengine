// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into independent Tick-only live camera lens controls before construction.</summary>
public interface ICameraLensGame { }
/// <summary>Vertical FOV in degrees and positive near/far distances. No smoothing is implicit.</summary>
public readonly record struct CameraLens(double VerticalFov,double Near,double Far);
/// <summary>Committed lenses; authored world lens remains the frozen player-FOV composition baseline.</summary>
public readonly record struct CameraLensState(EntityId Entity,CameraLens World,CameraLens? Viewmodel,CameraLens AuthoredWorld);
/// <summary>Null channels preserve their current values. At least one channel is required.</summary>
public readonly record struct CameraLensCommand(EntityId Entity,CameraLens? World=null,CameraLens? Viewmodel=null);
[StructLayout(LayoutKind.Sequential)] internal struct NativeCameraLensV1
{
    public double VerticalFov,Near,Far;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeCameraLensCommandV1
{
    public uint Version,Bytes;public EntityId Entity;public uint Channels,Reserved;
    public NativeCameraLensV1 World,Viewmodel;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeCameraLensStateV1
{
    public uint Version,Bytes,Present,Flags;public EntityId Entity;
    public NativeCameraLensV1 World,Viewmodel,AuthoredWorld;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeCameraLensServicesV1
{
    public NativeRootMotionServicesV1 RootMotion;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,NativeCameraLensStateV1*,NativeError*,int> CameraLensGet;
    public delegate* unmanaged[Cdecl]<void*,NativeCameraLensCommandV1*,NativeError*,int> CameraLensSet;
}
internal static unsafe class CameraLensServiceAbi
{
    internal const uint RequiredBytes=320;
    internal static NativeCameraLensServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Gameplay camera lens requires services epoch 7 with at least 320 bytes.");
        var extended=(NativeCameraLensServicesV1*)services;
        if(extended->CameraLensGet==null || extended->CameraLensSet==null)throw new ArgumentException("Gameplay camera lens callback is absent.");
        return extended;
    }
    internal static bool LayoutValid()
    {
        NativeCameraLensServicesV1 services=default;NativeCameraLensCommandV1 command=default;NativeCameraLensStateV1 state=default;
        return sizeof(NativeCameraLensV1)==24 && sizeof(NativeCameraLensServicesV1)==320 && sizeof(NativeCameraLensCommandV1)==80 && sizeof(NativeCameraLensStateV1)==104 &&
            (byte*)&services.CameraLensGet-(byte*)&services==304 && (byte*)&services.CameraLensSet-(byte*)&services==312 &&
            (byte*)&command.Entity-(byte*)&command==8 && (byte*)&command.Channels-(byte*)&command==24 && (byte*)&command.Reserved-(byte*)&command==28 &&
            (byte*)&command.World-(byte*)&command==32 && (byte*)&command.Viewmodel-(byte*)&command==56 &&
            (byte*)&state.Present-(byte*)&state==8 && (byte*)&state.Flags-(byte*)&state==12 && (byte*)&state.Entity-(byte*)&state==16 &&
            (byte*)&state.World-(byte*)&state==32 && (byte*)&state.Viewmodel-(byte*)&state==56 && (byte*)&state.AuthoredWorld-(byte*)&state==80;
    }
    internal static bool Valid(CameraLens value)=>double.IsFinite(value.VerticalFov) && value.VerticalFov>=5 && value.VerticalFov<=150 &&
        double.IsFinite(value.Near) && double.IsFinite(value.Far) && value.Near>=.001 && value.Far<=1e7 && value.Far>value.Near;
    private static CameraLens Value(NativeCameraLensV1 value)=>new(value.VerticalFov,value.Near,value.Far);
    internal static NativeCameraLensV1 Encode(CameraLens value)
    {
        if(!Valid(value))throw new ArgumentException("Camera lens requires finite FOV5..150 and0.001<=near<far<=10000000.");
        return new(){VerticalFov=value.VerticalFov,Near=value.Near,Far=value.Far};
    }
    internal static CameraLensState? Decode(NativeCameraLensStateV1* result,EntityId entity)
    {
        if(result->Version!=1 || result->Bytes!=104 || result->Present>1 || result->Flags>1)
            throw new InvalidOperationException("Invalid native camera lens state header.");
        if(result->Present==0)
        {
            foreach(byte value in new ReadOnlySpan<byte>((byte*)result+8,96))
                if(value!=0)throw new InvalidOperationException("Absent native camera lens state is not canonical.");
            return null;
        }
        var world=Value(result->World);var authored=Value(result->AuthoredWorld);
        if(result->Entity!=entity || entity==default || !Valid(world) || !Valid(authored))
            throw new InvalidOperationException("Invalid native committed camera lens.");
        CameraLens? viewmodel=null;
        if(result->Flags==1)
        {
            viewmodel=Value(result->Viewmodel);
            if(!Valid(viewmodel.Value))throw new InvalidOperationException("Invalid native viewmodel lens.");
        }
        else foreach(byte value in new ReadOnlySpan<byte>(&result->Viewmodel,24))
            if(value!=0)throw new InvalidOperationException("Absent native viewmodel lens is not canonical.");
        return new(entity,world,viewmodel,authored);
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Reads callback-entry committed lenses during Tick; at most256 admitted reads. Live noncameras return null.</summary>
    public CameraLensState? GetCameraLens(EntityId entity)
    {
        RequireFeature(GameplayRequiredFeatures.CameraLens,nameof(ICameraLensGame));
        var extended=CameraLensServiceAbi.Validate(services);
        NativeCameraLensStateV1 result=new(){Version=1,Bytes=104};NativeError error=default;
        Check(extended->CameraLensGet(services->Context,&entity,&result,&error),&error);
        return CameraLensServiceAbi.Decode(&result,entity);
    }
    /// <summary>Stages persistent world/viewmodel values; at most64 distinct camera writes per Tick. Frozen viewmodel presence cannot change.</summary>
    public void SetCameraLens(EntityId entity,CameraLens? world=null,CameraLens? viewmodel=null)=>SetCameraLens(new CameraLensCommand(entity,world,viewmodel));
    public void SetCameraLens(in CameraLensCommand source)
    {
        RequireFeature(GameplayRequiredFeatures.CameraLens,nameof(ICameraLensGame));
        var extended=CameraLensServiceAbi.Validate(services);
        if(source.Entity==default || (!source.World.HasValue && !source.Viewmodel.HasValue))throw new ArgumentException("Camera lens command requires an entity and at least one channel.");
        NativeCameraLensCommandV1 command=new(){Version=1,Bytes=80,Entity=source.Entity};
        if(source.World is { } world){command.Channels|=1;command.World=CameraLensServiceAbi.Encode(world);}
        if(source.Viewmodel is { } viewmodel){command.Channels|=2;command.Viewmodel=CameraLensServiceAbi.Encode(viewmodel);}
        NativeError error=default;Check(extended->CameraLensSet(services->Context,&command,&error),&error);
    }
}
