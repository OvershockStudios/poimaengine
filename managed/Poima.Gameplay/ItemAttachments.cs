// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into Tick-only rigid item transfers and committed attachment reads.</summary>
public interface IItemAttachmentGame { }

/// <summary>Opts into transfers that require the receiving grip to be aligned at the end of the tick.</summary>
public interface IItemAttachmentContactGame : IItemAttachmentGame { }

/// <summary>A local contact frame in metres with a normalized XYZW rotation.</summary>
public readonly record struct ItemGripFrame(Vector3d Position, Quaterniond Rotation)
{
    public static ItemGripFrame Identity => new(default,Quaterniond.Identity);
}

/// <summary>Corresponding calibrated frames on the original item and the receiving anchor.</summary>
public readonly record struct ItemAttachmentContact(ItemGripFrame Item, ItemGripFrame Anchor,
    double PositionLimitM, double RotationLimitDegrees);

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
        return PhysicalRecoveryServiceAbi.LayoutValid() && sizeof(NativeItemAttachmentServicesV1)==528 &&
            (byte*)&api.Recovery-(byte*)&api==0 && (byte*)&api.Get-(byte*)&api==512 && (byte*)&api.Set-(byte*)&api==520 &&
            sizeof(NativeItemAttachmentStateV1)==64 && (byte*)&state.Item-(byte*)&state==16 && (byte*)&state.Anchor-(byte*)&state==32 &&
            (byte*)&state.SampledTick-(byte*)&state==48 && (byte*)&state.StructureRevision-(byte*)&state==56 &&
            sizeof(NativeItemAttachmentCommandV1)==128 && (byte*)&command.Item-(byte*)&command==8 && (byte*)&command.Anchor-(byte*)&command==24 &&
            (byte*)&command.Mode-(byte*)&command==40 && (byte*)&command.Position-(byte*)&command==48 &&
            (byte*)&command.Rotation-(byte*)&command==72 && (byte*)&command.Scale-(byte*)&command==104 &&
            sizeof(NativeItemAttachmentContactCommandV2)==256 && (byte*)&guarded.Command-(byte*)&guarded==0 &&
            (byte*)&guarded.ItemPosition-(byte*)&guarded==128 && (byte*)&guarded.ItemRotation-(byte*)&guarded==152 &&
            (byte*)&guarded.AnchorPosition-(byte*)&guarded==184 && (byte*)&guarded.AnchorRotation-(byte*)&guarded==208 &&
            (byte*)&guarded.PositionLimitM-(byte*)&guarded==240 && (byte*)&guarded.RotationLimitDegrees-(byte*)&guarded==248;
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

    /// <summary>Transfer without a pose jump after this tick moves the current holder. Null pins to world space.</summary>
    public void TransferItem(EntityId item, EntityId? anchor) => StageItemAttachment(item,anchor,0,default);

    /// <summary>Commit only if both contact limits hold after this tick's movement.
    /// A rejected contact rolls back the simulation step batch.</summary>
    public void TransferItem(EntityId item, EntityId anchor, ItemAttachmentContact contact)
    {
        RequireFeature(GameplayRequiredFeatures.ItemAttachmentContact,nameof(IItemAttachmentContactGame));
        if(item==default)throw new ArgumentException("Item identity is absent.",nameof(item));
        if(anchor==default)throw new ArgumentException("Anchor identity is absent.",nameof(anchor));
        var api=ItemAttachmentServiceAbi.Validate(services);
        NativeItemAttachmentContactCommandV2 command=new(){
            Command=new(){Version=2,Bytes=256,Item=item,Anchor=anchor},
            ItemPosition=contact.Item.Position,ItemRotation=contact.Item.Rotation,
            AnchorPosition=contact.Anchor.Position,AnchorRotation=contact.Anchor.Rotation,
            PositionLimitM=contact.PositionLimitM,RotationLimitDegrees=contact.RotationLimitDegrees};
        NativeError error=default;
        Check(api->Set(services->Context,&command.Command,&error),&error);
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
