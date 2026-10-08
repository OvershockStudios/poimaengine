// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into native fixed-Tick navigation queries independently of movement and animation.</summary>
public interface INavigationGame { }
public enum NavigationPathStatus : uint { Complete=0,Partial=1,Unreachable=2,BufferLimit=3,OutOfNodes=4 }
/// <summary>One owned float-precision world-space corner. Not borrowed native storage.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly record struct NavigationPoint(float X,float Y,float Z);
/// <summary>A content SHA-256 identity; this is not an entity handle.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly record struct NavigationAssetId(ulong Word0,ulong Word1,ulong Word2,ulong Word3)
{
    public override string ToString()=>$"{Word0:x16}{Word1:x16}{Word2:x16}{Word3:x16}";
}
public readonly record struct NavigationPathResult(NavigationAssetId Asset,NavigationPathStatus Status,int CornerCount,uint Polygons,
    NavigationPoint RequestedStart,NavigationPoint? ProjectedStart,NavigationPoint? ProjectedEnd,NavigationPoint? ReachableEnd,
    double? StartProjectionDistance,double? EndProjectionDistance)
{
    /// <summary>Only Complete grants a complete route; partial and budget-limited corners are prefixes.</summary>
    public bool Complete=>Status==NavigationPathStatus.Complete;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeNavigationRequestV1
{
    public uint Version,Bytes;public EntityId Agent;public NavigationPoint Goal,Extents;
    public uint MaxPolygons,MaxCorners,MaxNodes,Reserved;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeNavigationResultV1
{
    public uint Version,Bytes;public NavigationAssetId Asset;public uint Status,Flags,CornerCount,Polygons;
    public NavigationPoint RequestedStart,ProjectedStart,ProjectedEnd,ReachableEnd;
    public double StartProjectionDistance,EndProjectionDistance;public uint Reserved0,Reserved1;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeNavigationServicesV1
{
    public NativeCharacterServicesV1 Character;
    public delegate* unmanaged[Cdecl]<void*,NativeNavigationRequestV1*,NavigationPoint*,uint,NativeNavigationResultV1*,NativeError*,int> NavigationPath;
}
internal static unsafe class NavigationServiceAbi
{
    internal const uint RequiredBytes=224;
    internal static bool LayoutValid()
    {
        NativeNavigationServicesV1 services=default;NativeNavigationRequestV1 request=default;NativeNavigationResultV1 result=default;
        return CharacterInputServiceAbi.LayoutValid() && sizeof(NativeNavigationServicesV1)==224 && sizeof(NativeNavigationRequestV1)==64 &&
            sizeof(NativeNavigationResultV1)==128 && sizeof(NavigationPoint)==12 && sizeof(NavigationAssetId)==32 &&
            (byte*)&services.Character-(byte*)&services==0 && (byte*)&services.NavigationPath-(byte*)&services==216 &&
            (byte*)&request.Agent-(byte*)&request==8 && (byte*)&request.Goal-(byte*)&request==24 && (byte*)&request.Extents-(byte*)&request==36 &&
            (byte*)&request.MaxPolygons-(byte*)&request==48 && (byte*)&request.MaxCorners-(byte*)&request==52 &&
            (byte*)&request.MaxNodes-(byte*)&request==56 && (byte*)&request.Reserved-(byte*)&request==60 &&
            (byte*)&result.Asset-(byte*)&result==8 && (byte*)&result.Status-(byte*)&result==40 && (byte*)&result.Flags-(byte*)&result==44 &&
            (byte*)&result.CornerCount-(byte*)&result==48 && (byte*)&result.Polygons-(byte*)&result==52 &&
            (byte*)&result.RequestedStart-(byte*)&result==56 && (byte*)&result.ProjectedStart-(byte*)&result==68 &&
            (byte*)&result.ProjectedEnd-(byte*)&result==80 && (byte*)&result.ReachableEnd-(byte*)&result==92 &&
            (byte*)&result.StartProjectionDistance-(byte*)&result==104 && (byte*)&result.EndProjectionDistance-(byte*)&result==112 &&
            (byte*)&result.Reserved0-(byte*)&result==120 && (byte*)&result.Reserved1-(byte*)&result==124;
    }
    internal static NativeNavigationServicesV1* Validate(NativeServices* services)
    {
        // A larger allocation grants only this opted-in callback. Do not read or
        // validate unrelated animation/character tails in a navigation-only game.
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Navigation service ABI mismatch: navigation_query_v1 requires epoch 7 with at least 224 bytes. Declare INavigationGame.");
        var extended=(NativeNavigationServicesV1*)services;
        if(extended->NavigationPath==null)throw new ArgumentException("Versioned navigation service callback is absent.");
        return extended;
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Plans from a live character's committed native foot position. Tick-only; at most eight attempts per Tick. Reads do not move the character. On any failure the destination span is unchanged.</summary>
    public NavigationPathResult FindNavigationPath(EntityId agent,Vector3d goal,Span<NavigationPoint> corners,Vector3d? extents=null,uint maxPolygons=256,uint maxNodes=4096)
    {
        RequireFeature(GameplayRequiredFeatures.Navigation,nameof(INavigationGame));
        if(corners.Length is <2 or >256)throw new ArgumentOutOfRangeException(nameof(corners),"Corner capacity must be 2..256.");
        if(maxPolygons is <1 or >256)throw new ArgumentOutOfRangeException(nameof(maxPolygons));
        if(maxNodes is <32 or >4096)throw new ArgumentOutOfRangeException(nameof(maxNodes));
        var target=NavigationVector(goal,-1000000,1000000,nameof(goal));
        var search=NavigationVector(extents ?? new Vector3d(2,4,2),.01,100,nameof(extents));
        var extended=NavigationServiceAbi.Validate(services);
        // Query into owned scratch. Even a native callback that reports success
        // with malformed output cannot partially overwrite the caller's route.
        Span<NavigationPoint> scratch=stackalloc NavigationPoint[corners.Length];
        scratch.Fill(new(float.NaN,float.NaN,float.NaN));
        NativeNavigationRequestV1 request=new(){Version=1,Bytes=64,Agent=agent,Goal=target,Extents=search,
            MaxPolygons=maxPolygons,MaxCorners=(uint)corners.Length,MaxNodes=maxNodes};
        NativeNavigationResultV1 result=new(){Version=1,Bytes=128};NativeError error=default;
        fixed(NavigationPoint* output=scratch)
            Check(extended->NavigationPath(services->Context,&request,output,(uint)scratch.Length,&result,&error),&error);
        if(result.Version!=1 || result.Bytes!=128 || result.Reserved0!=0 || result.Reserved1!=0 || result.Status>4 || (result.Flags & ~7u)!=0 ||
            result.CornerCount>(uint)corners.Length || result.Polygons>maxPolygons || !NavigationPosition(result.RequestedStart) ||
            !NavigationProjection(result.ProjectedStart,result.StartProjectionDistance,(result.Flags & 1)!=0) ||
            !NavigationProjection(result.ProjectedEnd,result.EndProjectionDistance,(result.Flags & 2)!=0) ||
            (((result.Flags & 4)!=0) ? !NavigationPosition(result.ReachableEnd) : result.ReachableEnd!=default))
            throw new InvalidOperationException("Invalid versioned native navigation result.");
        bool corridor=result.Polygons!=0;
        if(((result.Flags & 4)!=0)!=corridor || (corridor && ((result.Flags & 3)!=3 || result.CornerCount==0)) ||
            (result.CornerCount!=0 && !corridor) ||
            (result.Status==0 && ((result.Flags & 7)!=7 || !corridor || result.CornerCount==0)) ||
            (result.Status==2 && (corridor || result.CornerCount!=0)) || (!corridor && result.Status!=2))
            throw new InvalidOperationException("Inconsistent versioned native navigation result.");
        for(int index=0;index<(int)result.CornerCount;++index)
            if(!NavigationPosition(scratch[index]))throw new InvalidOperationException("Invalid native navigation corner.");
        scratch[..(int)result.CornerCount].CopyTo(corners);
        return new(result.Asset,(NavigationPathStatus)result.Status,(int)result.CornerCount,result.Polygons,result.RequestedStart,
            (result.Flags & 1)!=0 ? result.ProjectedStart : null,(result.Flags & 2)!=0 ? result.ProjectedEnd : null,
            (result.Flags & 4)!=0 ? result.ReachableEnd : null,(result.Flags & 1)!=0 ? result.StartProjectionDistance : null,
            (result.Flags & 2)!=0 ? result.EndProjectionDistance : null);
    }
    private static NavigationPoint NavigationVector(Vector3d value,double minimum,double maximum,string name)
    {
        if(!double.IsFinite(value.X) || !double.IsFinite(value.Y) || !double.IsFinite(value.Z) ||
            value.X<minimum || value.X>maximum || value.Y<minimum || value.Y>maximum || value.Z<minimum || value.Z>maximum)
            throw new ArgumentOutOfRangeException(name);
        return new((float)value.X,(float)value.Y,(float)value.Z);
    }
    private static bool NavigationPosition(NavigationPoint value)=>float.IsFinite(value.X) && float.IsFinite(value.Y) && float.IsFinite(value.Z) &&
        Math.Abs(value.X)<=1000000 && Math.Abs(value.Y)<=1000000 && Math.Abs(value.Z)<=1000000;
    private static bool NavigationProjection(NavigationPoint point,double distance,bool present)=>present ?
        NavigationPosition(point) && double.IsFinite(distance) && distance>=0 : point==default && distance==0;
}
