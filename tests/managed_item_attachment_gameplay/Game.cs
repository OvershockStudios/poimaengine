// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct AttachmentState
{
    public EntityId Item, First, Second, ObservedAnchor;
    public int Command, Ticks, ObservedBinding;
    public long ObservedTick, ObservedRevision, FailAt;
}

[GameModule("poima.test.item-attachment")]
public sealed class ItemAttachmentGame : Game<AttachmentState>, IItemAttachmentGame
{
    public override void Initialize(ref AttachmentState state) => state.FailAt=-1;

    public override void Tick(ref AttachmentState state, GameContext context)
    {
        var before=context.GetItemAttachment(state.Item);
        state.ObservedAnchor=before?.Anchor??default;
        state.ObservedBinding=before.HasValue ? 1 : 0;
        state.ObservedTick=before.HasValue ? checked((long)before.Value.SampledTick) : -1;
        state.ObservedRevision=before.HasValue ? checked((long)before.Value.StructureRevision) : -1;
        ++state.Ticks;
        switch(state.Command)
        {
            case 0: break;
            case 1: context.TransferItem(state.Item,state.First); break;
            case 2:
                context.TransferItem(state.Item,state.Second);
                context.RequestSave("handoff-auto",0);
                break;
            case 3:
                context.TransferItem(state.Item,null);
                context.RequestSave("pin-auto",0);
                break;
            case 4: context.SnapItem(state.Item,state.Second,new(new(.25,.5,-.25),Quaterniond.Identity,new(1,1,1))); break;
            case 5: context.ResetItemAttachment(state.Item); break;
            case 6: context.TransferItem(state.Item,state.Item); break;
            default: throw new InvalidOperationException("Unknown handoff fixture command.");
        }
        state.Command=0;
        if(context.GetItemAttachment(state.Item)!=before)throw new InvalidOperationException("Staged transfer changed the committed read.");
        if(checked((long)context.Tick)==state.FailAt)throw new InvalidOperationException("Intentional later tick failure.");
    }
}

[GameModule("poima.test.item-attachment")]
public sealed class ItemAttachmentUnmarkedGame : Game<AttachmentState>
{
    public override void Initialize(ref AttachmentState state) { }
    public override void Tick(ref AttachmentState state, GameContext context) => context.TransferItem(state.Item,state.First);
}
