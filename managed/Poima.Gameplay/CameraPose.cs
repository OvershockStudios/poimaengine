// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into Tick-only camera presentation pose controls independently of lens controls.</summary>
public interface ICameraPoseGame { }
/// <summary>Rigid offset in metres, applied in local camera space. Identity explicitly clears a channel.</summary>
public readonly record struct CameraPoseOffset(Vector3d Position,Quaterniond Rotation)
{
    public static CameraPoseOffset Identity=>new(new(0,0,0),Quaterniond.Identity);
}
/// <summary>Committed offsets and the camera world matrix without any camera presentation offsets,
/// including ancestor camera offsets. BaseWorld includes native look, stance and physical motion.
/// Use BaseWorld plus gameplay aim recoil for shot logic; EntitySnapshot.Transform includes visual effects.</summary>
public readonly record struct CameraPoseState(EntityId Entity,CameraPoseOffset World,CameraPoseOffset? Viewmodel,WorldTransform BaseWorld);
/// <summary>Null channels preserve committed offsets. A selected viewmodel channel requires an authored foreground lens.</summary>
public readonly record struct CameraPoseCommand(EntityId Entity,CameraPoseOffset? World=null,CameraPoseOffset? Viewmodel=null);
[StructLayout(LayoutKind.Sequential)] internal struct NativeCameraPoseV1
{
    public Vector3d Position;public Quaterniond Rotation;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeCameraPoseCommandV1
{
    public uint Version,Bytes;public EntityId Entity;public uint Channels,Reserved;
    public NativeCameraPoseV1 World,Viewmodel;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeCameraPoseStateV1
{
    public uint Version,Bytes,Present,Flags;public EntityId Entity;
    public NativeCameraPoseV1 World,Viewmodel;public WorldTransform BaseWorld;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeCameraPoseServicesV1
{
    public NativeAnimationRateServicesV1 AnimationRate;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,NativeCameraPoseStateV1*,NativeError*,int> CameraPoseGet;
    public delegate* unmanaged[Cdecl]<void*,NativeCameraPoseCommandV1*,NativeError*,int> CameraPoseSet;
}
internal static unsafe class CameraPoseServiceAbi
{
    internal const uint RequiredBytes=392;
    internal static bool LayoutValid()
    {
        NativeCameraPoseServicesV1 services=default;NativeCameraPoseCommandV1 command=default;NativeCameraPoseStateV1 state=default;
        return AnimationRateServiceAbi.LayoutValid() && sizeof(NativeCameraPoseV1)==56 && sizeof(NativeCameraPoseServicesV1)==392 && sizeof(NativeCameraPoseCommandV1)==144 && sizeof(NativeCameraPoseStateV1)==272 &&
            (byte*)&services.CameraPoseGet-(byte*)&services==376 && (byte*)&services.CameraPoseSet-(byte*)&services==384 &&
            (byte*)&command.Entity-(byte*)&command==8 && (byte*)&command.Channels-(byte*)&command==24 && (byte*)&command.Reserved-(byte*)&command==28 &&
            (byte*)&command.World-(byte*)&command==32 && (byte*)&command.Viewmodel-(byte*)&command==88 &&
            (byte*)&state.Present-(byte*)&state==8 && (byte*)&state.Flags-(byte*)&state==12 && (byte*)&state.Entity-(byte*)&state==16 &&
            (byte*)&state.World-(byte*)&state==32 && (byte*)&state.Viewmodel-(byte*)&state==88 && (byte*)&state.BaseWorld-(byte*)&state==144;
    }
    internal static NativeCameraPoseServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Camera poses require services epoch7 with at least392 bytes. Declare ICameraPoseGame.");
        var extended=(NativeCameraPoseServicesV1*)services;
        if(extended->CameraPoseGet==null || extended->CameraPoseSet==null)throw new ArgumentException("Camera pose callback is absent.");
        return extended;
    }
    internal static bool Valid(CameraPoseOffset value)
    {
        var p=value.Position;var q=value.Rotation;
        return double.IsFinite(p.X) && Math.Abs(p.X)<=10 && double.IsFinite(p.Y) && Math.Abs(p.Y)<=10 && double.IsFinite(p.Z) && Math.Abs(p.Z)<=10 &&
            double.IsFinite(q.X) && Math.Abs(q.X)<=1.00001 && double.IsFinite(q.Y) && Math.Abs(q.Y)<=1.00001 &&
            double.IsFinite(q.Z) && Math.Abs(q.Z)<=1.00001 && double.IsFinite(q.W) && Math.Abs(q.W)<=1.00001 &&
            Math.Abs(q.X*q.X+q.Y*q.Y+q.Z*q.Z+q.W*q.W-1)<=1e-5;
    }
    internal static NativeCameraPoseV1 Encode(CameraPoseOffset value)
    {
        if(!Valid(value))throw new ArgumentException("Camera pose requires finite translation within10 metres per axis and a unit quaternion (norm-squared tolerance1e-5).");
        return new(){Position=value.Position,Rotation=value.Rotation};
    }
    private static CameraPoseOffset Value(NativeCameraPoseV1 value)=>new(value.Position,value.Rotation);
    internal static CameraPoseState? Decode(NativeCameraPoseStateV1* result,EntityId entity)
    {
        if(result->Version!=1 || result->Bytes!=272 || result->Present>1 || result->Flags>1 || result->Entity!=entity)
            throw new InvalidOperationException("Invalid native camera pose state header.");
        if(result->Present==0)
        {
            if(result->Flags!=0)throw new InvalidOperationException("Absent camera pose has flags.");
            foreach(byte value in new ReadOnlySpan<byte>((byte*)result+32,240))
                if(value!=0)throw new InvalidOperationException("Absent camera pose is not canonical.");
            return null;
        }
        var world=Value(result->World);
        if(entity==default || !Valid(world))throw new InvalidOperationException("Invalid native world camera pose.");
        CameraPoseOffset? viewmodel=null;
        if(result->Flags==1)
        {
            viewmodel=Value(result->Viewmodel);
            if(!Valid(viewmodel.Value))throw new InvalidOperationException("Invalid native viewmodel camera pose.");
        }
        else foreach(byte value in new ReadOnlySpan<byte>(&result->Viewmodel,56))
            if(value!=0)throw new InvalidOperationException("Absent viewmodel pose is not canonical.");
        for(int column=0;column<4;++column)for(int row=0;row<4;++row)
        {
            double value=result->BaseWorld[column,row];
            if(!double.IsFinite(value) || Math.Abs(value)>1e12)throw new InvalidOperationException("Invalid native base camera matrix.");
        }
        if(result->BaseWorld[0,3]!=0 || result->BaseWorld[1,3]!=0 || result->BaseWorld[2,3]!=0 || result->BaseWorld[3,3]!=1)
            throw new InvalidOperationException("Native base camera matrix must be affine.");
        return new(entity,world,viewmodel,result->BaseWorld);
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Reads callback-entry committed offsets/base matrix, at most256 reads per Tick.
    /// A live noncamera returns null. This does not expose staged changes.</summary>
    public CameraPoseState? GetCameraPose(EntityId entity)
    {
        RequireFeature(GameplayRequiredFeatures.CameraPose,nameof(ICameraPoseGame));
        var extended=CameraPoseServiceAbi.Validate(services);
        NativeCameraPoseStateV1 result=new(){Version=1,Bytes=272};NativeError error=default;
        Check(extended->CameraPoseGet(services->Context,&entity,&result,&error),&error);
        return CameraPoseServiceAbi.Decode(&result,entity);
    }
    /// <summary>Stages offsets atomically with the Tick. At most64 unique camera targets;
    /// world effects cannot move physics descendants. Offsets persist until changed or reset to identity.</summary>
    public void SetCameraPose(EntityId entity,CameraPoseOffset? world=null,CameraPoseOffset? viewmodel=null)=>SetCameraPose(new CameraPoseCommand(entity,world,viewmodel));
    public void SetCameraPose(in CameraPoseCommand source)
    {
        RequireFeature(GameplayRequiredFeatures.CameraPose,nameof(ICameraPoseGame));
        var extended=CameraPoseServiceAbi.Validate(services);
        if(source.Entity==default || (!source.World.HasValue && !source.Viewmodel.HasValue))throw new ArgumentException("Camera pose command requires an entity and at least one channel.");
        NativeCameraPoseCommandV1 command=new(){Version=1,Bytes=144,Entity=source.Entity};
        if(source.World is { } world){command.Channels|=1;command.World=CameraPoseServiceAbi.Encode(world);}
        if(source.Viewmodel is { } viewmodel){command.Channels|=2;command.Viewmodel=CameraPoseServiceAbi.Encode(viewmodel);}
        NativeError error=default;Check(extended->CameraPoseSet(services->Context,&command,&error),&error);
    }
}
