// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;

namespace Poima;

/// <summary>Declares final-tick arm clearance checks on attempted pickups.</summary>
public interface IItemPickupClearanceGame : IItemAttachmentPickupGame { }

/// <summary>Declares same-tick arm pose fallback when a sampled pickup is obstructed.</summary>
public interface IItemPickupPoseGame : IItemPickupClearanceGame { }

/// <summary>World-space capsule radii for the live manual chain at the receiving hand.</summary>
public readonly record struct ItemPickupClearance(EntityId Rig,uint Slot,double UpperRadius,double LowerRadius);
public readonly record struct ItemPickupBlocker(EntityId Item,EntityId Hand,ulong SampledTick,IkClearanceBlocker Blocker);

[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeItemPickupClearanceV7
{
    public NativeItemAttachmentSurfaceCommandV3 Sampled;
    public EntityId Rig;
    public double UpperRadius,LowerRadius;
    public uint Slot,Reserved;
}

[StructLayout(LayoutKind.Sequential)]
internal struct NativeItemPickupBlockerV5
{
    public uint Version,Bytes,Segment,Interval;
    public EntityId Item,Anchor,Obstacle;
    public ulong SampledTick;
    public double Fraction,Penetration;
}

internal static unsafe class ItemPickupClearanceAbi
{
    internal static bool LayoutValid()
    {
        NativeItemPickupClearanceV7 command=default;
        NativeItemPickupBlockerV5 result=default;
        return sizeof(NativeItemPickupClearanceV7)==312 && (byte*)&command.Rig-(byte*)&command==272 &&
            (byte*)&command.UpperRadius-(byte*)&command==288 && (byte*)&command.Slot-(byte*)&command==304 &&
            sizeof(NativeItemPickupBlockerV5)==88 && (byte*)&result.Item-(byte*)&result==16 &&
            (byte*)&result.Obstacle-(byte*)&result==48 && (byte*)&result.SampledTick-(byte*)&result==64;
    }
}

public readonly unsafe ref partial struct GameContext
{
    /// <summary>Capture the arm at the callback boundary, then check the swept arm and
    /// sampled grip against final poses and physics before transferring the prop.</summary>
    public void AttemptItemPickup(EntityId item,EntityId hand,ItemAttachmentContact contact,
        ReadOnlySpan<ItemSurfaceProbe> probes,ItemPickupClearance clearance)
    {
        RequireFeature(GameplayRequiredFeatures.ItemPickupClearance,nameof(IItemPickupClearanceGame));
        ValidatePickupClearance(clearance);
        StageSampledTransfer(item,hand,contact,probes,7,clearance);
    }

    /// <summary>On final obstruction, try the prior arm controls and then the incoming animation pose.
    /// The pickup is still refused and the committed arm can be read through the usual IK state.</summary>
    public void AttemptItemPickupWithPoseFallback(EntityId item,EntityId hand,ItemAttachmentContact contact,
        ReadOnlySpan<ItemSurfaceProbe> probes,ItemPickupClearance clearance)
    {
        RequireFeature(GameplayRequiredFeatures.ItemPickupPose,nameof(IItemPickupPoseGame));
        ValidatePickupClearance(clearance);
        StageSampledTransfer(item,hand,contact,probes,8,clearance);
    }

    private static void ValidatePickupClearance(ItemPickupClearance clearance)
    {
        if(clearance.Rig==default || clearance.Slot is <1 or >8 ||
            !double.IsFinite(clearance.UpperRadius) || clearance.UpperRadius is <.001 or >1 ||
            !double.IsFinite(clearance.LowerRadius) || clearance.LowerRadius is <.001 or >1)
            throw new ArgumentException("Supply a live manual IK chain and two radii of .001..1 metres.",nameof(clearance));
    }

    /// <summary>Read a pickup blocked by the arm corridor on the committed callback-entry tick.</summary>
    public ItemPickupBlocker? GetItemPickupBlocker(EntityId item)
    {
        RequireFeature(GameplayRequiredFeatures.ItemPickupClearance,nameof(IItemPickupClearanceGame));
        if(item==default)throw new ArgumentException("Item identity is absent.",nameof(item));
        var api=ItemAttachmentServiceAbi.Validate(services);
        NativeItemPickupBlockerV5 result=new(){Version=5,Bytes=88};
        NativeError error=default;
        Check(api->Get(services->Context,&item,(NativeItemAttachmentStateV1*)&result,&error),&error);
        if(result.Version!=5 || result.Bytes!=88 || result.Item!=item || result.SampledTick!=Tick ||
            result.Segment>2 || (result.Segment==0
                ? result.Anchor!=default || result.Obstacle!=default || result.Interval!=0 ||
                  result.Fraction!=0 || result.Penetration!=0
                : result.Anchor==default || result.Anchor==item || result.Obstacle==default ||
                  result.Interval is <1 or >4 || !double.IsFinite(result.Fraction) || result.Fraction is <0 or >1 ||
                  !double.IsFinite(result.Penetration) || result.Penetration is <0 or >1e6 ||
                  (result.Fraction>0 && result.Penetration!=0)))
            throw new InvalidOperationException("Invalid committed pickup clearance witness.");
        return result.Segment==0 ? null : new(item,result.Anchor,result.SampledTick,
            new(result.Obstacle,(IkClearanceSegment)result.Segment,result.Interval,result.Fraction,result.Penetration));
    }
}
