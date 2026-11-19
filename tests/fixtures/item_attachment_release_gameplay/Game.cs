// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct AttachmentReleaseState
{
    public EntityId Item, Actor, LastAnchor, LastObstacle;
    public double OffsetX, OffsetY, OffsetZ, BlockedOffsetY, LinearX, LinearY, LinearZ, AngularX, AngularY, AngularZ, LastPenetration;
    public long TemplateHigh, TemplateLow, LastOutcomeTick;
    public int BlockedAt, ReleaseAt, InvalidAt, DuplicateAt, SaveBlocked, SaveRelease, Ticks, Attempts, LastStatus;
}

[GameModule("poima.test.item-attachment-release")]
public sealed class AttachmentReleaseGame : Game<AttachmentReleaseState>, IItemAttachmentReleaseGame, IInputActionsGame
{
    public override void Initialize(ref AttachmentReleaseState state) { }

    public override void Tick(ref AttachmentReleaseState state, GameContext context)
    {
        ++state.Ticks;
        var previous=context.GetItemReleaseOutcome(state.Item);
        state.LastStatus=previous is { } outcome ? (int)outcome.Status : 0;
        state.LastOutcomeTick=(long)(previous?.SampledTick ?? 0);
        state.LastAnchor=previous?.Anchor ?? default;
        state.LastObstacle=previous?.Obstacle ?? default;
        state.LastPenetration=previous?.PenetrationM ?? 0;

        var committedTick=context.Tick+1;
        bool blocked=committedTick==(ulong)state.BlockedAt;
        bool invalid=committedTick==(ulong)state.InvalidAt;
        bool duplicate=committedTick==(ulong)state.DuplicateAt;
        bool pressed=state.Actor!=default && context.GetInputAction(state.Actor,"drop").Pressed;
        if(!blocked && !invalid && !duplicate && !pressed && committedTick!=(ulong)state.ReleaseAt)return;

        ++state.Attempts;
        var bodyInItem=new ItemGripFrame(new(state.OffsetX,blocked ? state.BlockedOffsetY : state.OffsetY,state.OffsetZ),Quaterniond.Identity);
        var linear=new Vector3d(invalid ? 101 : state.LinearX,state.LinearY,state.LinearZ);
        var angular=new Vector3d(state.AngularX,state.AngularY,state.AngularZ);
        var template=new EntityId(unchecked((ulong)state.TemplateHigh),unchecked((ulong)state.TemplateLow));
        context.AttemptItemRelease(state.Item,template,linear,angular,bodyInItem);
        if(duplicate)context.TransferItem(state.Item,null);
        if(blocked && state.SaveBlocked!=0)context.RequestSave("engine-blocked",0);
        if(!blocked && !invalid && state.SaveRelease!=0)context.RequestSave("engine-released",0);
    }
}
