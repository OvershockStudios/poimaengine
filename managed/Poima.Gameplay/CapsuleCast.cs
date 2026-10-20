// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into Tick-only capsule casts independently of other query or camera features.</summary>
public interface ICapsuleCastGame { }
/// <summary>Owned transient contact data. Points are in metres in world space; Normal is optional.
/// InitialOverlap includes touching at the query start and does not imply a clear starting volume.</summary>
public readonly record struct CapsuleCastHit(EntityId Entity,double Fraction,double Distance,
    Vector3d PointOnQuery,Vector3d PointOnObstacle,Vector3d? Normal,double PenetrationDepth,
    uint? Triangle,bool InitialOverlap);
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeCapsuleCastRequestV1
{
    public uint Version,Bytes;public Vector3d Position;public Quaterniond Rotation;
    public double Radius,HalfHeight;public Vector3d Direction;public double Distance;
    public EntityId* Ignore;public uint IgnoreCount,Reserved;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeCapsuleCastResultV1
{
    public uint Version,Bytes,Present,Flags;public EntityId Entity;
    public double Fraction,Distance;public Vector3d PointOnQuery,PointOnObstacle,Normal;
    public double PenetrationDepth;public uint Triangle,Reserved;public ulong ReservedTail;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeCapsuleQueryServicesV1
{
    public NativeCameraPoseServicesV1 CameraPose;
    public delegate* unmanaged[Cdecl]<void*,NativeCapsuleCastRequestV1*,NativeCapsuleCastResultV1*,NativeError*,int> CapsuleCast;
}
internal static unsafe class CapsuleCastServiceAbi
{
    internal const uint RequiredBytes=400;
    internal static bool LayoutValid()
    {
        NativeCapsuleQueryServicesV1 s=default;NativeCapsuleCastRequestV1 q=default;NativeCapsuleCastResultV1 r=default;
        return CameraPoseServiceAbi.LayoutValid() && sizeof(NativeCapsuleQueryServicesV1)==400 && sizeof(NativeCapsuleCastRequestV1)==128 && sizeof(NativeCapsuleCastResultV1)==144 &&
            (byte*)&s.CapsuleCast-(byte*)&s==392 && (byte*)&q.Position-(byte*)&q==8 && (byte*)&q.Rotation-(byte*)&q==32 &&
            (byte*)&q.Radius-(byte*)&q==64 && (byte*)&q.HalfHeight-(byte*)&q==72 && (byte*)&q.Direction-(byte*)&q==80 &&
            (byte*)&q.Distance-(byte*)&q==104 && (byte*)&q.Ignore-(byte*)&q==112 && (byte*)&q.IgnoreCount-(byte*)&q==120 && (byte*)&q.Reserved-(byte*)&q==124 &&
            (byte*)&r.Present-(byte*)&r==8 && (byte*)&r.Flags-(byte*)&r==12 && (byte*)&r.Entity-(byte*)&r==16 &&
            (byte*)&r.Fraction-(byte*)&r==32 && (byte*)&r.Distance-(byte*)&r==40 && (byte*)&r.PointOnQuery-(byte*)&r==48 &&
            (byte*)&r.PointOnObstacle-(byte*)&r==72 && (byte*)&r.Normal-(byte*)&r==96 && (byte*)&r.PenetrationDepth-(byte*)&r==120 &&
            (byte*)&r.Triangle-(byte*)&r==128 && (byte*)&r.Reserved-(byte*)&r==132 && (byte*)&r.ReservedTail-(byte*)&r==136;
    }
    internal static NativeCapsuleQueryServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Capsule casts require services epoch7 with at least400 bytes. Declare ICapsuleCastGame.");
        var extended=(NativeCapsuleQueryServicesV1*)services;
        if(extended->CapsuleCast==null)throw new ArgumentException("Capsule cast callback is absent.");
        return extended;
    }
    private static bool Finite(Vector3d v)=>double.IsFinite(v.X) && double.IsFinite(v.Y) && double.IsFinite(v.Z);
    private static bool Position(Vector3d v)=>Finite(v) && Math.Abs(v.X)<=1e6 && Math.Abs(v.Y)<=1e6 && Math.Abs(v.Z)<=1e6;
    // Contact/support points can lie outside the bounded query-center domain.
    private static bool Contact(Vector3d v)=>Finite(v) && Math.Abs(v.X)<=1e12 && Math.Abs(v.Y)<=1e12 && Math.Abs(v.Z)<=1e12;
    private static bool PositiveZero(double v)=>BitConverter.DoubleToInt64Bits(v)==0;
    private static bool Zero(Vector3d v)=>PositiveZero(v.X) && PositiveZero(v.Y) && PositiveZero(v.Z);
    internal static void ValidateInput(Vector3d position,Quaterniond rotation,double radius,double halfHeight,Vector3d direction,double distance,ReadOnlySpan<EntityId> ignore)
    {
        if(!Finite(position) || Math.Abs(position.X)>1e6 || Math.Abs(position.Y)>1e6 || Math.Abs(position.Z)>1e6 ||
            !double.IsFinite(radius) || radius<.001 || radius>10 || !double.IsFinite(halfHeight) || halfHeight<0 || halfHeight>10 ||
            !Finite(direction) || Math.Abs(direction.X)>1e6 || Math.Abs(direction.Y)>1e6 || Math.Abs(direction.Z)>1e6 ||
            direction.X*direction.X+direction.Y*direction.Y+direction.Z*direction.Z<1e-24 ||
            !double.IsFinite(distance) || distance<0 || distance>10000 ||
            !double.IsFinite(rotation.X) || !double.IsFinite(rotation.Y) || !double.IsFinite(rotation.Z) || !double.IsFinite(rotation.W) ||
            Math.Abs(rotation.X*rotation.X+rotation.Y*rotation.Y+rotation.Z*rotation.Z+rotation.W*rotation.W-1)>1e-5)
            throw new ArgumentException("Capsule cast requires bounded finite geometry, a unit quaternion and a nonzero direction even for overlap queries.");
        double length=Math.Sqrt(direction.X*direction.X+direction.Y*direction.Y+direction.Z*direction.Z);
        if(!Position(new(position.X+direction.X/length*distance,position.Y+direction.Y/length*distance,position.Z+direction.Z/length*distance)))
            throw new ArgumentException("Capsule endpoint must remain within +/-1000km.");
        if(ignore.Length>128)throw new ArgumentException("At most128 ignored capsule entities.",nameof(ignore));
        for(int i=0;i<ignore.Length;++i)
        {
            if(ignore[i]==default)throw new ArgumentException("Ignored capsule entities must be nonzero.",nameof(ignore));
            for(int j=0;j<i;++j)if(ignore[i]==ignore[j])throw new ArgumentException("Ignored capsule entities must be unique.",nameof(ignore));
        }
    }
    internal static CapsuleCastHit? Decode(NativeCapsuleCastResultV1* r,double maximumDistance)
    {
        if(r->Version!=1 || r->Bytes!=144 || r->Present>1 || (r->Flags & ~7u)!=0 || r->Reserved!=0 || r->ReservedTail!=0)
            throw new InvalidOperationException("Invalid native capsule cast result header.");
        if(r->Present==0)
        {
            foreach(byte value in new ReadOnlySpan<byte>((byte*)r+12,132))
                if(value!=0)throw new InvalidOperationException("Native capsule miss is not canonical.");
            return null;
        }
        if(r->Entity==default || !double.IsFinite(r->Fraction) || r->Fraction<0 || r->Fraction>1 ||
            !double.IsFinite(r->Distance) || r->Distance<0 || r->Distance>maximumDistance ||
            !Contact(r->PointOnQuery) || !Contact(r->PointOnObstacle) ||
            !double.IsFinite(r->PenetrationDepth) || r->PenetrationDepth<0 || r->PenetrationDepth>1e6 ||
            ((r->Flags & 2)!=0 ? !Finite(r->Normal) || Math.Abs(r->Normal.X*r->Normal.X+r->Normal.Y*r->Normal.Y+r->Normal.Z*r->Normal.Z-1)>1e-5 : !Zero(r->Normal)) ||
            ((r->Flags & 4)==0 && r->Triangle!=0) ||
            ((r->Flags & 1)==0 && !PositiveZero(r->PenetrationDepth)) ||
            ((r->Flags & 1)!=0 && (r->Fraction!=0 || r->Distance!=0)) ||
            (maximumDistance==0 && (r->Flags & 1)==0))
            throw new InvalidOperationException("Invalid native capsule cast contact.");
        return new(r->Entity,r->Fraction,r->Distance,r->PointOnQuery,r->PointOnObstacle,
            (r->Flags & 2)!=0 ? r->Normal : null,r->PenetrationDepth,(r->Flags & 4)!=0 ? r->Triangle : null,(r->Flags & 1)!=0);
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Casts a capsule centered at position, with its cylinder on rotation's local Y.
    /// Radius is .001..10m, cylinder half-height0..10m (zero means sphere), travel0..10000m.
    /// Zero travel queries overlap; direction is bounded to +/-1e6 per axis and has length at least1e-12.
    /// Center and endpoint stay within +/-1000km. At most32 native attempts per Tick.
    /// Reads committed collision geometry; staged writes remain invisible. No pointers escape this call.</summary>
    public CapsuleCastHit? CapsuleCast(Vector3d position,Quaterniond rotation,double radius,double halfHeight,Vector3d direction,double distance,ReadOnlySpan<EntityId> ignore=default)
    {
        RequireFeature(GameplayRequiredFeatures.CapsuleCast,nameof(ICapsuleCastGame));
        var extended=CapsuleCastServiceAbi.Validate(services);
        CapsuleCastServiceAbi.ValidateInput(position,rotation,radius,halfHeight,direction,distance,ignore);
        fixed(EntityId* ids=ignore)
        {
            NativeCapsuleCastRequestV1 request=new(){Version=1,Bytes=128,Position=position,Rotation=rotation,Radius=radius,HalfHeight=halfHeight,Direction=direction,Distance=distance,Ignore=ids,IgnoreCount=(uint)ignore.Length};
            NativeCapsuleCastResultV1 result=new(){Version=1,Bytes=144};NativeError error=default;
            Check(extended->CapsuleCast(services->Context,&request,&result,&error),&error);
            return CapsuleCastServiceAbi.Decode(&result,distance);
        }
    }
}
