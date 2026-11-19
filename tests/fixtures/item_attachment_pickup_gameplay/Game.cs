// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct AttachmentPickupState
{
    public EntityId Item, Holder, Source, Target, Actor, PinnedBody;
    public double BlockedLimit, PickupLimit, FrameLimit, LastGap;
    public long TemplateLow, LastOutcomeTick;
    public int DropAt, MissAt, PickupAt, BadAt, SaveMiss, SavePickup;
    public int Ticks, PickupAttempts, LastOutcome;
}

[GameModule("poima.test.item-attachment-pickup")]
public sealed class AttachmentPickupGame : Game<AttachmentPickupState>,
    IItemAttachmentReleaseGame, IItemAttachmentPickupGame, IInputActionsGame
{
    public override void Initialize(ref AttachmentPickupState state) { }

    public override void Tick(ref AttachmentPickupState state, GameContext context)
    {
        ++state.Ticks;
        var prior=context.GetItemTransferOutcome(state.Item);
        state.LastOutcome=prior is { } outcome ? (int)outcome.Status : 0;
        state.LastOutcomeTick=(long)(prior?.SampledTick ?? 0);
        state.LastGap=prior?.Surface?.DistanceM ?? 0;
        ulong committedTick=context.Tick+1;
        if(committedTick==(ulong)state.DropAt)
        {
            var template=new EntityId(0,unchecked((ulong)state.TemplateLow));
            context.AttemptItemRelease(state.Item,template,new(.3,0,0),new(0,0,0));
            return;
        }

        bool miss=committedTick==(ulong)state.MissAt;
        bool invalid=committedTick==(ulong)state.BadAt;
        bool pressed=state.Actor!=default && context.GetInputAction(state.Actor,"pickup").Pressed;
        bool scripted=committedTick==(ulong)state.PickupAt;
        if(!miss && !pressed && !scripted && !invalid)return;

        ++state.PickupAttempts;
        var guard=new ItemAttachmentContact(new ItemGripFrame(new(0,1,0),Quaterniond.Identity),ItemGripFrame.Identity,state.FrameLimit,5);
        ItemSurfaceProbe[] probes=[new(new ItemSurfacePoint(state.Source,3,new(.25,.5,.25)),
            state.Target,miss ? state.BlockedLimit : state.PickupLimit,new uint[]{2,3})];
        context.AttemptItemPickup(state.Item,state.Holder,guard,probes);
        if(miss && state.SaveMiss!=0)context.RequestSave("pickup-miss",0);
        if(!miss && !invalid && state.SavePickup!=0)context.RequestSave("pickup-complete",0);
    }
}
