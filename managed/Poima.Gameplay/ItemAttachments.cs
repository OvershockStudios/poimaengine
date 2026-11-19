// SPDX-License-Identifier: Apache-2.0
using System.Buffers;
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into Tick-only rigid item transfers and committed attachment reads.</summary>
public interface IItemAttachmentGame { }

/// <summary>Opts into transfers that require the receiving grip to be aligned at the end of the tick.</summary>
public interface IItemAttachmentContactGame : IItemAttachmentGame { }

/// <summary>Opts into checked handoffs that also require surface contact at the end of the tick.</summary>
public interface IItemAttachmentSurfaceGame : IItemAttachmentContactGame { }

/// <summary>Opts into recoverable sampled grabs and committed outcome reads.</summary>
public interface IItemAttachmentRecoveryGame : IItemAttachmentSurfaceGame { }

/// <summary>Opts into release to world physics and reading the committed result.</summary>
public interface IItemAttachmentReleaseGame : IItemAttachmentGame { }

/// <summary>Opts into checked pickups that retire a released item's collision body.</summary>
public interface IItemAttachmentPickupGame : IItemAttachmentRecoveryGame { }

/// <summary>Opts into identifying a released item through its nearest physical hit.</summary>
public interface IItemAttachmentAimGame : IItemAttachmentGame { }

/// <summary>The first collider and the released item it represents, when applicable.</summary>
public readonly record struct ReleasedItemAim(EntityId? Item, RayHit? Hit, double? Fraction, uint? Triangle,
    ulong SampledTick, ulong StructureRevision);

/// <summary>A local contact frame in metres with a normalized XYZW rotation.</summary>
public readonly record struct ItemGripFrame(Vector3d Position, Quaterniond Rotation)
{
    public static ItemGripFrame Identity => new(default,Quaterniond.Identity);
}

/// <summary>Corresponding calibrated frames on the original item and the receiving anchor.</summary>
public readonly record struct ItemAttachmentContact(ItemGripFrame Item, ItemGripFrame Anchor,
    double PositionLimitM, double RotationLimitDegrees);

/// <summary>A cooked vertex, or a triangle ordinal and barycentric weights, on a live drawable.</summary>
public readonly record struct ItemSurfacePoint(EntityId Drawable, uint Index, Vector3d? Barycentric = null);

/// <summary>A point that must reach the target drawable within DistanceLimitM.</summary>
public readonly record struct ItemSurfaceProbe(ItemSurfacePoint Source, EntityId Target, double DistanceLimitM,
    ReadOnlyMemory<uint> TargetTriangles = default);

public enum ItemTransferStatus : uint { Transferred=1, FrameMiss=2, SurfaceMiss=3 }
public readonly record struct ItemTransferFrameMiss(double PositionErrorM, double PositionLimitM,
    double? RotationErrorDegrees, double RotationLimitDegrees);
public readonly record struct ItemTransferSurfaceMiss(uint ProbeIndex, ulong TargetTriangle,
    double DistanceM, double DistanceLimitM);
public readonly record struct ItemTransferOutcome(EntityId Item, EntityId AttemptedAnchor, ulong SampledTick,
    ItemTransferStatus Status, ItemTransferFrameMiss? Frame, ItemTransferSurfaceMiss? Surface);

public enum ItemReleaseStatus : uint { Released=1, Blocked=2 }
/// <summary>Anchor is the new collision body on release, or the current holder on obstruction.</summary>
public readonly record struct ItemReleaseOutcome(EntityId Item, EntityId Anchor, ulong SampledTick,
    ItemReleaseStatus Status, EntityId? Obstacle, double? PenetrationM);

/// <summary>A committed binding. A null anchor holds the item in world space.</summary>
public readonly record struct ItemAttachment(EntityId Item, EntityId? Anchor, ulong SampledTick, ulong StructureRevision);

/// <summary>A socket-local transform in metres with a normalized XYZW rotation and positive scale.</summary>
public readonly record struct ItemSocketOffset(Vector3d Position, Quaterniond Rotation, Vector3d Scale)
{
    public static ItemSocketOffset Identity => new(default, Quaterniond.Identity, new(1, 1, 1));
}

[StructLayout(LayoutKind.Sequential)] internal struct NativeItemAttachmentStateV1
{
    public uint Version, Bytes, Present, Reserved;
    public EntityId Item, Anchor;
    public ulong SampledTick, StructureRevision;
}

[StructLayout(LayoutKind.Sequential)] internal struct NativeItemAttachmentCommandV1
{
    public uint Version, Bytes;
    public EntityId Item, Anchor;
    public uint Mode, Reserved;
    public Vector3d Position;
    public Quaterniond Rotation;
    public Vector3d Scale;
}

[StructLayout(LayoutKind.Sequential)] internal struct NativeItemAttachmentContactCommandV2
{
    public NativeItemAttachmentCommandV1 Command;
    public Vector3d ItemPosition;
    public Quaterniond ItemRotation;
    public Vector3d AnchorPosition;
    public Quaterniond AnchorRotation;
    public double PositionLimitM, RotationLimitDegrees;
}

[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeItemSurfaceProbeV1
{
    public EntityId Source, Target;
    public uint SourceIndex, SourceKind, TargetCount, Reserved;
    public Vector3d Barycentric;
    public double DistanceLimitM;
    public uint* TargetTriangles;
}

[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeItemAttachmentSurfaceCommandV3
{
    public NativeItemAttachmentContactCommandV2 Contact;
    public NativeItemSurfaceProbeV1* Probes;
    public uint ProbeCount, Reserved;
}

[StructLayout(LayoutKind.Sequential)] internal struct NativeItemAttachmentOutcomeV2
{
    public uint Version, Bytes, Status, ProbeIndex;
    public EntityId Item, Anchor;
    public ulong SampledTick, TargetTriangle;
    public double PositionErrorM, PositionLimitM, RotationErrorDegrees, RotationLimitDegrees, DistanceM, DistanceLimitM;
    public uint RotationAvailable, Reserved;
}

[StructLayout(LayoutKind.Sequential)] internal struct NativeItemReleaseCommandV5
{
    public uint Version, Bytes;
    public EntityId Item, BodyTemplate;
    public uint Reserved, ReservedTail;
    public Vector3d BodyPosition;
    public Quaterniond BodyRotation;
    public Vector3d LinearVelocityMps, AngularVelocityRadps;
}

[StructLayout(LayoutKind.Sequential)] internal struct NativeItemReleaseOutcomeV3
{
    public uint Version, Bytes, Status, Reserved;
    public EntityId Item, Anchor, Obstacle;
    public ulong SampledTick;
    public double PenetrationM;
}

[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeReleasedItemAimV4
{
    public uint Version, Bytes, TrianglePresent, Triangle;
    public NativeRay Ray;
    public NativeHit Hit;
    public EntityId Item;
    public ulong SampledTick, StructureRevision;
}

[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeItemAttachmentServicesV1
{
    public NativePhysicalRecoveryServicesV1 Recovery;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,NativeItemAttachmentStateV1*,NativeError*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,NativeItemAttachmentCommandV1*,NativeError*,int> Set;
}

internal static unsafe class ItemAttachmentServiceAbi
{
    internal const uint RequiredBytes=528;

    internal static bool LayoutValid()
    {
        NativeItemAttachmentServicesV1 api=default;
        NativeItemAttachmentStateV1 state=default;
        NativeItemAttachmentCommandV1 command=default;
        NativeItemAttachmentContactCommandV2 guarded=default;
        NativeItemAttachmentSurfaceCommandV3 sampled=default;
        NativeItemSurfaceProbeV1 probe=default;
        NativeItemAttachmentOutcomeV2 outcome=default;
        NativeItemReleaseCommandV5 release=default;
        NativeItemReleaseOutcomeV3 releaseOutcome=default;
        NativeReleasedItemAimV4 aim=default;
        return PhysicalRecoveryServiceAbi.LayoutValid() && sizeof(NativeItemAttachmentServicesV1)==528 &&
            sizeof(NativeReleasedItemAimV4)==208 && (byte*)&aim.Ray-(byte*)&aim==16 && (byte*)&aim.Hit-(byte*)&aim==88 &&
            (byte*)&aim.Item-(byte*)&aim==176 && (byte*)&aim.SampledTick-(byte*)&aim==192 &&
            (byte*)&aim.StructureRevision-(byte*)&aim==200 &&
            (byte*)&api.Recovery-(byte*)&api==0 && (byte*)&api.Get-(byte*)&api==512 && (byte*)&api.Set-(byte*)&api==520 &&
            sizeof(NativeItemAttachmentStateV1)==64 && (byte*)&state.Item-(byte*)&state==16 && (byte*)&state.Anchor-(byte*)&state==32 &&
            (byte*)&state.SampledTick-(byte*)&state==48 && (byte*)&state.StructureRevision-(byte*)&state==56 &&
            sizeof(NativeItemAttachmentCommandV1)==128 && (byte*)&command.Item-(byte*)&command==8 && (byte*)&command.Anchor-(byte*)&command==24 &&
            (byte*)&command.Mode-(byte*)&command==40 && (byte*)&command.Position-(byte*)&command==48 &&
            (byte*)&command.Rotation-(byte*)&command==72 && (byte*)&command.Scale-(byte*)&command==104 &&
            sizeof(NativeItemAttachmentContactCommandV2)==256 && (byte*)&guarded.Command-(byte*)&guarded==0 &&
            (byte*)&guarded.ItemPosition-(byte*)&guarded==128 && (byte*)&guarded.ItemRotation-(byte*)&guarded==152 &&
            (byte*)&guarded.AnchorPosition-(byte*)&guarded==184 && (byte*)&guarded.AnchorRotation-(byte*)&guarded==208 &&
            (byte*)&guarded.PositionLimitM-(byte*)&guarded==240 && (byte*)&guarded.RotationLimitDegrees-(byte*)&guarded==248 &&
            sizeof(NativeItemAttachmentSurfaceCommandV3)==272 && (byte*)&sampled.Contact-(byte*)&sampled==0 &&
            (byte*)&sampled.Probes-(byte*)&sampled==256 && (byte*)&sampled.ProbeCount-(byte*)&sampled==264 &&
            (byte*)&sampled.Reserved-(byte*)&sampled==268 && sizeof(NativeItemSurfaceProbeV1)==88 &&
            (byte*)&probe.Target-(byte*)&probe==16 && (byte*)&probe.SourceIndex-(byte*)&probe==32 &&
            (byte*)&probe.TargetCount-(byte*)&probe==40 && (byte*)&probe.Barycentric-(byte*)&probe==48 &&
            (byte*)&probe.DistanceLimitM-(byte*)&probe==72 && (byte*)&probe.TargetTriangles-(byte*)&probe==80 &&
            sizeof(NativeItemAttachmentOutcomeV2)==120 && (byte*)&outcome.Item-(byte*)&outcome==16 &&
            (byte*)&outcome.Anchor-(byte*)&outcome==32 && (byte*)&outcome.SampledTick-(byte*)&outcome==48 &&
            (byte*)&outcome.TargetTriangle-(byte*)&outcome==56 && (byte*)&outcome.PositionErrorM-(byte*)&outcome==64 &&
            (byte*)&outcome.DistanceM-(byte*)&outcome==96 && (byte*)&outcome.RotationAvailable-(byte*)&outcome==112 &&
            sizeof(NativeItemReleaseCommandV5)==152 && (byte*)&release.Item-(byte*)&release==8 &&
            (byte*)&release.BodyTemplate-(byte*)&release==24 && (byte*)&release.BodyPosition-(byte*)&release==48 &&
            (byte*)&release.BodyRotation-(byte*)&release==72 && (byte*)&release.LinearVelocityMps-(byte*)&release==104 &&
            (byte*)&release.AngularVelocityRadps-(byte*)&release==128 && sizeof(NativeItemReleaseOutcomeV3)==80 &&
            (byte*)&releaseOutcome.Item-(byte*)&releaseOutcome==16 && (byte*)&releaseOutcome.Anchor-(byte*)&releaseOutcome==32 &&
            (byte*)&releaseOutcome.Obstacle-(byte*)&releaseOutcome==48 && (byte*)&releaseOutcome.SampledTick-(byte*)&releaseOutcome==64 &&
            (byte*)&releaseOutcome.PenetrationM-(byte*)&releaseOutcome==72;
    }

    internal static NativeItemAttachmentServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Item attachments require epoch 7 with at least 528 bytes. Declare IItemAttachmentGame.");
        var api=(NativeItemAttachmentServicesV1*)services;
        if(api->Get==null || api->Set==null)throw new ArgumentException("Item attachment callbacks are absent.");
        return api;
    }
}

public readonly unsafe ref partial struct GameContext
{
    /// <summary>Release at the end of this tick with explicit world velocities. An obstructed
    /// release keeps the current holder and leaves a result for the next Tick.</summary>
    public void AttemptItemRelease(EntityId item,EntityId bodyTemplate,Vector3d linearVelocityMps,
        Vector3d angularVelocityRadps,ItemGripFrame? bodyInItem=null)
    {
        RequireFeature(GameplayRequiredFeatures.ItemAttachmentRelease,nameof(IItemAttachmentReleaseGame));
        if(item==default || bodyTemplate==default)throw new ArgumentException("Release requires live item and template identities.");
        var api=ItemAttachmentServiceAbi.Validate(services);
        var frame=bodyInItem ?? ItemGripFrame.Identity;
        NativeItemReleaseCommandV5 command=new(){Version=5,Bytes=152,Item=item,BodyTemplate=bodyTemplate,
            BodyPosition=frame.Position,BodyRotation=frame.Rotation,
            LinearVelocityMps=linearVelocityMps,AngularVelocityRadps=angularVelocityRadps};
        NativeError error=default;
        Check(api->Set(services->Context,(NativeItemAttachmentCommandV1*)&command,&error),&error);
    }

    /// <summary>Read the release attempted on the committed tick entering this callback.</summary>
    public ItemReleaseOutcome? GetItemReleaseOutcome(EntityId item)
    {
        RequireFeature(GameplayRequiredFeatures.ItemAttachmentRelease,nameof(IItemAttachmentReleaseGame));
        if(item==default)throw new ArgumentException("Item identity is absent.",nameof(item));
        var api=ItemAttachmentServiceAbi.Validate(services);
        NativeItemReleaseOutcomeV3 result=new(){Version=3,Bytes=80};
        NativeError error=default;
        Check(api->Get(services->Context,&item,(NativeItemAttachmentStateV1*)&result,&error),&error);
        bool blocked=result.Status==(uint)ItemReleaseStatus.Blocked;
        if(result.Version!=3 || result.Bytes!=80 || result.Reserved!=0 || result.Item!=item ||
            result.SampledTick!=Tick || result.Status>2 || result.Anchor==item ||
            (result.Status==0)!=(result.Anchor==default) ||
            (blocked ? result.Obstacle==default || !double.IsFinite(result.PenetrationM) || result.PenetrationM<=(double)0.001f :
                result.Obstacle!=default || result.PenetrationM!=0))
            throw new InvalidOperationException("Invalid committed item release outcome.");
        if(result.Status==0)return null;
        return new(item,result.Anchor,result.SampledTick,(ItemReleaseStatus)result.Status,
            blocked ? result.Obstacle : null,blocked ? result.PenetrationM : null);
    }

    /// <summary>Read the binding at the start of this tick. Null means the live item is unbound.</summary>
    public ItemAttachment? GetItemAttachment(EntityId item)
    {
        RequireFeature(GameplayRequiredFeatures.ItemAttachment,nameof(IItemAttachmentGame));
        if(item==default)throw new ArgumentException("Item identity is absent.",nameof(item));
        var api=ItemAttachmentServiceAbi.Validate(services);
        NativeItemAttachmentStateV1 state=new(){Version=1,Bytes=64};
        NativeError error=default;
        Check(api->Get(services->Context,&item,&state,&error),&error);
        if(state.Version!=1 || state.Bytes!=64 || state.Present>1 || state.Reserved!=0 || state.Item!=item ||
            state.SampledTick!=Tick || state.StructureRevision>9007199254740991UL || state.Anchor==item ||
            (state.Present==0 && state.Anchor!=default))
            throw new InvalidOperationException("Invalid committed item attachment observation.");
        if(state.Present==0)return null;
        return new(item,state.Anchor==default ? null : state.Anchor,state.SampledTick,state.StructureRevision);
    }

    /// <summary>Trace from a caller-selected world-space pose at Tick entry.</summary>
    public ReleasedItemAim AimReleasedItem(Vector3d origin,Vector3d direction,double distance,
        ReadOnlySpan<EntityId> ignore=default)
    {
        RequireFeature(GameplayRequiredFeatures.ItemAttachmentAim,nameof(IItemAttachmentAimGame));
        var api=ItemAttachmentServiceAbi.Validate(services);
        NativeReleasedItemAimV4 result=new(){Version=4,Bytes=208};
        NativeError error=default;
        fixed(EntityId* ids=ignore)
        {
            result.Ray=new(){Origin=origin,Direction=direction,Distance=distance,Ignore=ids,IgnoreCount=(uint)ignore.Length};
            Check(api->Get(services->Context,null,(NativeItemAttachmentStateV1*)&result,&error),&error);
        }
        var hit=result.Hit;
        if(result.Version!=4 || result.Bytes!=208 || result.SampledTick!=Tick ||
            result.StructureRevision>9007199254740991UL || hit.Hit>1 || hit.NormalValid>1 ||
            result.TrianglePresent>1 || (result.TrianglePresent==0 && result.Triangle!=0) ||
            (hit.Hit==0 && (hit.Entity!=default || hit.NormalValid!=0 || result.TrianglePresent!=0 || result.Item!=default)) ||
            (hit.Hit==1 && (hit.Entity==default || hit.Entity==result.Item ||
                !double.IsFinite(hit.Fraction) || hit.Fraction<0 || hit.Fraction>1 ||
                !double.IsFinite(hit.Distance) || hit.Distance<0)))
            throw new InvalidOperationException("Invalid released-item aim observation.");
        RayHit? collider=hit.Hit==0 ? null : new(hit.Entity,hit.Distance,hit.Position,hit.NormalValid!=0 ? hit.Normal : null);
        return new(result.Item==default ? null : result.Item,collider,hit.Hit==0 ? null : hit.Fraction,
            result.TrianglePresent==0 ? null : result.Triangle,result.SampledTick,result.StructureRevision);
    }

    /// <summary>Read this item's completed attempt from the tick entering this callback.</summary>
    public ItemTransferOutcome? GetItemTransferOutcome(EntityId item)
    {
        RequireFeature(GameplayRequiredFeatures.ItemAttachmentRecovery,nameof(IItemAttachmentRecoveryGame));
        if(item==default)throw new ArgumentException("Item identity is absent.",nameof(item));
        var api=ItemAttachmentServiceAbi.Validate(services);
        NativeItemAttachmentOutcomeV2 result=new(){Version=2,Bytes=120};
        NativeError error=default;
        Check(api->Get(services->Context,&item,(NativeItemAttachmentStateV1*)&result,&error),&error);
        if(result.Version!=2 || result.Bytes!=120 || result.Item!=item || result.SampledTick!=Tick ||
            result.Reserved!=0 || result.RotationAvailable>1 || result.Status>3 || result.Anchor==item ||
            (result.Status==0)!=(result.Anchor==default))
            throw new InvalidOperationException("Invalid committed item transfer outcome.");
        if(result.Status==0)return null;
        var status=(ItemTransferStatus)result.Status;
        ItemTransferFrameMiss? frame=status==ItemTransferStatus.FrameMiss
            ? new(result.PositionErrorM,result.PositionLimitM,
                result.RotationAvailable==1 ? result.RotationErrorDegrees : null,result.RotationLimitDegrees) : null;
        ItemTransferSurfaceMiss? surface=status==ItemTransferStatus.SurfaceMiss
            ? new(result.ProbeIndex,result.TargetTriangle,result.DistanceM,result.DistanceLimitM) : null;
        return new(item,result.Anchor,result.SampledTick,status,frame,surface);
    }

    /// <summary>Transfer without a pose jump after this tick moves the current holder. Null pins to world space.</summary>
    public void TransferItem(EntityId item, EntityId? anchor) => StageItemAttachment(item,anchor,0,default);

    /// <summary>Commit only if both contact limits hold after this tick's movement.
    /// A rejected contact rolls back the simulation step batch.</summary>
    public void TransferItem(EntityId item, EntityId anchor, ItemAttachmentContact contact)
    {
        RequireFeature(GameplayRequiredFeatures.ItemAttachmentContact,nameof(IItemAttachmentContactGame));
        var api=ItemAttachmentServiceAbi.Validate(services);
        var command=ContactCommand(item,anchor,contact,2,256);
        NativeError error=default;
        Check(api->Set(services->Context,&command.Command,&error),&error);
    }

    /// <summary>Stage a checked handoff with 1..8 surface probes. The host owns the copied
    /// selectors and checks them after solving the tick. A miss rolls back the step batch.</summary>
    public void TransferItem(EntityId item, EntityId anchor, ItemAttachmentContact contact,
        ReadOnlySpan<ItemSurfaceProbe> probes)
    {
        RequireFeature(GameplayRequiredFeatures.ItemAttachmentSurface,nameof(IItemAttachmentSurfaceGame));
        StageSampledTransfer(item,anchor,contact,probes,3);
    }

    /// <summary>Stage a handoff whose contact miss leaves the item with its current holder.
    /// Read the committed result in a later Tick. Invalid commands roll back the step.</summary>
    public void AttemptItemTransfer(EntityId item, EntityId anchor, ItemAttachmentContact contact,
        ReadOnlySpan<ItemSurfaceProbe> probes)
    {
        RequireFeature(GameplayRequiredFeatures.ItemAttachmentRecovery,nameof(IItemAttachmentRecoveryGame));
        StageSampledTransfer(item,anchor,contact,probes,4);
    }

    /// <summary>Pick up a released item if the final grip reaches it this tick.
    /// A successful pickup retires the release body with the transfer.</summary>
    public void AttemptItemPickup(EntityId item, EntityId anchor, ItemAttachmentContact contact,
        ReadOnlySpan<ItemSurfaceProbe> probes)
    {
        RequireFeature(GameplayRequiredFeatures.ItemAttachmentPickup,nameof(IItemAttachmentPickupGame));
        StageSampledTransfer(item,anchor,contact,probes,6);
    }

    private void StageSampledTransfer(EntityId item, EntityId anchor, ItemAttachmentContact contact,
        ReadOnlySpan<ItemSurfaceProbe> probes, uint version,ItemPickupClearance? clearance=null)
    {
        var api=ItemAttachmentServiceAbi.Validate(services);
        var guarded=ContactCommand(item,anchor,contact,version,clearance.HasValue ? 312u : 272u);
        if(probes.Length is <1 or >8)
            throw new ArgumentException("Supply 1..8 item surface probes.",nameof(probes));

        int count=0;
        foreach(var probe in probes)
        {
            if(probe.Source.Drawable==default || probe.Target==default ||
                !(probe.DistanceLimitM>=0 && probe.DistanceLimitM<=1e9) || probe.TargetTriangles.Length>4096)
                throw new ArgumentException("Invalid item surface probe.",nameof(probes));
            count+=probe.TargetTriangles.Length;
        }
        uint[]? triangles=count==0 ? null : ArrayPool<uint>.Shared.Rent(count);
        try
        {
            NativeItemSurfaceProbeV1* selectors=stackalloc NativeItemSurfaceProbeV1[probes.Length];
            fixed(uint* selected=triangles)
            {
                int offset=0;
                for(int i=0;i<probes.Length;++i)
                {
                    var probe=probes[i];
                    var selection=probe.TargetTriangles.Span;
                    selection.CopyTo(new Span<uint>(selected+offset,selection.Length));
                    selectors[i]=new(){Source=probe.Source.Drawable,Target=probe.Target,SourceIndex=probe.Source.Index,
                        SourceKind=probe.Source.Barycentric.HasValue ? 1u : 0u, Barycentric=probe.Source.Barycentric.GetValueOrDefault(),
                        TargetCount=(uint)selection.Length,TargetTriangles=selection.IsEmpty ? null : selected+offset,
                        DistanceLimitM=probe.DistanceLimitM};
                    offset+=selection.Length;
                }
                NativeItemAttachmentSurfaceCommandV3 command=new(){Contact=guarded,Probes=selectors,ProbeCount=(uint)probes.Length};
                NativeError error=default;
                if(clearance is { } arm)
                {
                    NativeItemPickupClearanceV7 checkedPickup=new(){Sampled=command,Rig=arm.Rig,
                        UpperRadius=arm.UpperRadius,LowerRadius=arm.LowerRadius,Slot=arm.Slot};
                    Check(api->Set(services->Context,&checkedPickup.Sampled.Contact.Command,&error),&error);
                }
                else Check(api->Set(services->Context,&command.Contact.Command,&error),&error);
            }
        }
        finally
        {
            if(triangles!=null)ArrayPool<uint>.Shared.Return(triangles);
        }
    }

    private static NativeItemAttachmentContactCommandV2 ContactCommand(EntityId item, EntityId anchor,
        ItemAttachmentContact contact, uint version, uint bytes)
    {
        if(item==default)throw new ArgumentException("Item identity is absent.",nameof(item));
        if(anchor==default)throw new ArgumentException("Anchor identity is absent.",nameof(anchor));
        return new(){
            Command=new(){Version=version,Bytes=bytes,Item=item,Anchor=anchor},
            ItemPosition=contact.Item.Position,ItemRotation=contact.Item.Rotation,
            AnchorPosition=contact.Anchor.Position,AnchorRotation=contact.Anchor.Rotation,
            PositionLimitM=contact.PositionLimitM,RotationLimitDegrees=contact.RotationLimitDegrees};
    }

    /// <summary>Place the item at a complete socket offset after this tick moves the destination anchor.</summary>
    public void SnapItem(EntityId item, EntityId anchor, ItemSocketOffset offset)
    {
        if(anchor==default)throw new ArgumentException("Socket anchor identity is absent.",nameof(anchor));
        StageItemAttachment(item,anchor,1,offset);
    }

    /// <summary>Restore a bound item's authored parent and local transform after this tick.</summary>
    public void ResetItemAttachment(EntityId item) => StageItemAttachment(item,null,2,default);

    private void StageItemAttachment(EntityId item, EntityId? anchor, uint mode, ItemSocketOffset offset)
    {
        RequireFeature(GameplayRequiredFeatures.ItemAttachment,nameof(IItemAttachmentGame));
        if(item==default)throw new ArgumentException("Item identity is absent.",nameof(item));
        if(anchor.HasValue && anchor.Value==default)throw new ArgumentException("Anchor identity is absent.",nameof(anchor));
        var api=ItemAttachmentServiceAbi.Validate(services);
        NativeItemAttachmentCommandV1 command=new(){Version=1,Bytes=128,Item=item,Anchor=anchor.GetValueOrDefault(),
            Mode=mode,Position=offset.Position,Rotation=offset.Rotation,Scale=offset.Scale};
        NativeError error=default;
        Check(api->Set(services->Context,&command,&error),&error);
    }
}
