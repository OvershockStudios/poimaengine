// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using System.Text;
namespace Poima;

/// <summary>Opts into independent, read-only composite hit tests during Tick.</summary>
public interface IHitRegionsGame { }

/// <summary>Closest geometry or hit-region result. Entity owns the hit; RegionEntity identifies its attachment. Optional values are absent when unavailable.</summary>
public readonly record struct HitTestHit(EntityId Entity,EntityId? RegionEntity,string? Region,
    double Fraction,double Distance,Vector3d Position,Vector3d? Normal,uint? Triangle);

[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeHitRegionResultV1
{
    public uint Version,Bytes;public NativeHit Hit;public EntityId RegionEntity;
    public uint RegionNameBytes,TrianglePresent,Triangle,Reserved;
    public fixed byte RegionName[64];
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeHitRegionServicesV1
{
    public NativeInputActionServicesV1 Actions;
    public delegate* unmanaged[Cdecl]<void*,NativeRay*,NativeHitRegionResultV1*,NativeError*,int> HitTest;
}
internal static unsafe class HitRegionServiceAbi
{
    internal const uint RequiredBytes=272;
    internal static NativeHitRegionServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Gameplay hit regions require services epoch 7 with at least 272 bytes.");
        var extended=(NativeHitRegionServicesV1*)services;
        if(extended->HitTest==null)throw new ArgumentException("Gameplay hit region callback is absent.");
        return extended;
    }
    internal static bool LayoutValid()
    {
        NativeHitRegionServicesV1 services=default;NativeHitRegionResultV1 result=default;
        return sizeof(NativeHitRegionServicesV1)==272 && sizeof(NativeHitRegionResultV1)==192 &&
            (byte*)&services.HitTest-(byte*)&services==264 && (byte*)&result.Hit-(byte*)&result==8 &&
            (byte*)&result.RegionEntity-(byte*)&result==96 && (byte*)&result.RegionNameBytes-(byte*)&result==112 &&
            (byte*)&result.TrianglePresent-(byte*)&result==116 && (byte*)&result.Triangle-(byte*)&result==120 &&
            (byte*)&result.Reserved-(byte*)&result==124 && result.RegionName-(byte*)&result==128;
    }
    internal static bool Finite(Vector3d value)=>double.IsFinite(value.X) && double.IsFinite(value.Y) && double.IsFinite(value.Z);
    internal static bool Zero(Vector3d value)=>value.X==0 && value.Y==0 && value.Z==0;
    internal static HitTestHit? Decode(NativeHitRegionResultV1* result,double maximumDistance)
    {
        if(result->Version!=1 || result->Bytes!=192 || result->Reserved!=0 || result->Hit.Hit>1 ||
            result->Hit.NormalValid>1 || result->TrianglePresent>1 || result->RegionNameBytes>64)
            throw new InvalidOperationException("Invalid native hit region result header.");
        if(result->Hit.Hit==0)
        {
            foreach(byte value in new ReadOnlySpan<byte>((byte*)result+8,184))
                if(value!=0)throw new InvalidOperationException("Native hit region miss is not canonical.");
            return null;
        }
        var hit=result->Hit;
        if(hit.Entity==default || !double.IsFinite(hit.Fraction) || hit.Fraction<0 || hit.Fraction>1 ||
            !double.IsFinite(hit.Distance) || hit.Distance<0 || hit.Distance>maximumDistance || !Finite(hit.Position) ||
            !Finite(hit.Normal) || (hit.NormalValid==0 && !Zero(hit.Normal)) ||
            (hit.NormalValid==1 && Zero(hit.Normal)) || (result->TrianglePresent==0 && result->Triangle!=0))
            throw new InvalidOperationException("Invalid native hit region geometry.");
        string? region=null;EntityId? attachment=null;
        if(result->RegionNameBytes==0)
        {
            if(result->RegionEntity!=default)throw new InvalidOperationException("Geometry hit has a region entity without a name.");
        }
        else
        {
            if(result->RegionEntity==default || result->TrianglePresent!=0)
                throw new InvalidOperationException("Invalid native hit region identity.");
            for(int i=0;i<result->RegionNameBytes;++i)
            {
                byte c=result->RegionName[i];
                bool letter=c>=(byte)'a' && c<=(byte)'z';
                if(!(letter || (i>0 && ((c>=(byte)'0' && c<=(byte)'9') || c==(byte)'_' || c==(byte)'.' || c==(byte)'-'))))
                    throw new InvalidOperationException("Invalid native ASCII hit region name.");
            }
            region=Encoding.ASCII.GetString(new ReadOnlySpan<byte>(result->RegionName,(int)result->RegionNameBytes));attachment=result->RegionEntity;
        }
        for(int i=(int)result->RegionNameBytes;i<64;++i)
            if(result->RegionName[i]!=0)throw new InvalidOperationException("Native hit region name padding is not zero.");
        return new(hit.Entity,attachment,region,hit.Fraction,hit.Distance,hit.Position,
            hit.NormalValid==1 ? hit.Normal : null,result->TrianglePresent==1 ? result->Triangle : null);
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Tests current committed geometry and enabled regions without advancing simulation. At most 1,024 admitted queries per Tick. Staged commands remain invisible until Tick returns.</summary>
    public HitTestHit? HitTest(Vector3d origin,Vector3d direction,double distance,ReadOnlySpan<EntityId> ignore=default)
    {
        RequireFeature(GameplayRequiredFeatures.HitRegions,nameof(IHitRegionsGame));
        if(ignore.Length>128)throw new ArgumentException("At most 128 explicitly ignored hit-test entities.",nameof(ignore));
        var extended=HitRegionServiceAbi.Validate(services);
        fixed(EntityId* ids=ignore)
        {
            NativeRay ray=new(){Origin=origin,Direction=direction,Distance=distance,Ignore=ids,IgnoreCount=(uint)ignore.Length};
            NativeHitRegionResultV1 result=new(){Version=1,Bytes=192};NativeError error=default;
            Check(extended->HitTest(services->Context,&ray,&result,&error),&error);
            return HitRegionServiceAbi.Decode(&result,distance);
        }
    }
}
