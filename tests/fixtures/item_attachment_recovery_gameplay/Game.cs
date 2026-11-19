// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct AttachmentRecoveryState
{
    public EntityId Item, Anchor, Source, Target, ConcurrentItem;
    public double PositionX, PositionY, PositionZ, RotationX, RotationY, RotationZ, RotationW;
    public double PositionLimit, RotationLimit, DistanceLimit, RetryDistance, LastDistance;
    public long LastOutcomeTick;
    public int Probe0, Probe1, Probe2, Probe3, Count, TargetTriangle;
    public int AttemptAt, FrameAt, RetryAt, InvalidAt, Ticks, LastStatus, Attempts, SaveFirst, SampleTriangle;
}

[GameModule("poima.test.item-attachment-recovery")]
public sealed class AttachmentRecoveryGame : Game<AttachmentRecoveryState>, IItemAttachmentRecoveryGame
{
    public override void Initialize(ref AttachmentRecoveryState state) { }

    public override void Tick(ref AttachmentRecoveryState state, GameContext context)
    {
        ++state.Ticks;
        var previous=context.GetItemTransferOutcome(state.Item);
        state.LastStatus=previous is { } outcome ? (int)outcome.Status : 0;
        state.LastOutcomeTick=(long)(previous?.SampledTick ?? 0);
        state.LastDistance=previous?.Surface?.DistanceM ?? 0;
        var finalTick=context.Tick+1;
        bool first=finalTick==(ulong)state.AttemptAt, frameMiss=finalTick==(ulong)state.FrameAt;
        bool retry=finalTick==(ulong)state.RetryAt, invalid=finalTick==(ulong)state.InvalidAt;
        if(!first && !frameMiss && !retry && !invalid)return;
        ++state.Attempts;

        var itemFrame=new ItemGripFrame(new(state.PositionX+(frameMiss || invalid ? 2 : 0),state.PositionY,state.PositionZ),
            new(state.RotationX,state.RotationY,state.RotationZ,state.RotationW));
        var contact=new ItemAttachmentContact(itemFrame,ItemGripFrame.Identity,state.PositionLimit,state.RotationLimit);
        var selected=state.TargetTriangle<0 ? Array.Empty<uint>() : new[]{checked((uint)state.TargetTriangle)};
        var probes=new ItemSurfaceProbe[state.Count];
        ReadOnlySpan<int> vertices=[state.Probe0,state.Probe1,state.Probe2,state.Probe3];
        for(int i=0;i<probes.Length;++i)
        {
            ItemSurfacePoint point=first && !invalid && state.SampleTriangle!=0 ? new(state.Source,3,new(.3,.6,.1))
                : new(state.Source,invalid ? uint.MaxValue : checked((uint)vertices[i]));
            probes[i]=new(point,state.Target,retry ? state.RetryDistance : state.DistanceLimit,selected);
        }
        context.AttemptItemTransfer(state.Item,state.Anchor,contact,probes);
        if(first && state.ConcurrentItem!=default)context.TransferItem(state.ConcurrentItem,null);
        if(first && state.SaveFirst!=0)context.RequestSave("recovery-auto",0);
    }
}
