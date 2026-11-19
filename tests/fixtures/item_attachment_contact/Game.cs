// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct AttachmentContactState
{
    public EntityId Item, First, Second;
    public double OffsetX, OffsetY, OffsetZ;
    public int Command, Ticks;
}

[GameModule("poima.test.item-attachment-contact")]
public sealed class AttachmentContactGame : Game<AttachmentContactState>, IItemAttachmentContactGame
{
    public override void Initialize(ref AttachmentContactState state) { }

    public override void Tick(ref AttachmentContactState state, GameContext context)
    {
        ++state.Ticks;
        if(state.Command==0)return;
        var offset=new ItemGripFrame(new(state.OffsetX,state.OffsetY,state.OffsetZ),Quaterniond.Identity);
        var contact=new ItemAttachmentContact(ItemGripFrame.Identity,offset,.002,.1);
        var anchor=context.Tick==0 ? state.First : state.Second;
        context.TransferItem(state.Item,anchor,contact);
        if(context.Tick==0)context.RequestSave("contact-auto",0);
        if(state.Command==1 || context.Tick!=0)state.Command=0;
    }
}
