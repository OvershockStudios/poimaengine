// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct AttachmentSurfaceState
{
    public EntityId Item, Anchor, Source, Target;
    public double PositionX, PositionY, PositionZ, RotationX, RotationY, RotationZ, RotationW;
    public double PositionLimit, RotationLimit, DistanceLimit;
    public int Probe0, Probe1, Probe2, Probe3, Count, TargetTriangle;
    public int AtTick, RepeatAtTick, Ticks, Attempts, SaveFirst;
}

[GameModule("poima.test.item-attachment-surface")]
public sealed class AttachmentSurfaceGame : Game<AttachmentSurfaceState>, IItemAttachmentSurfaceGame
{
    public override void Initialize(ref AttachmentSurfaceState state) { }

    public override void Tick(ref AttachmentSurfaceState state, GameContext context)
    {
        ++state.Ticks;
        var finalTick=context.Tick+1;
        var first=finalTick==(ulong)state.AtTick;
        var repeat=finalTick==(ulong)state.RepeatAtTick;
        if(!first && !repeat)return;
        ++state.Attempts;

        var itemFrame=new ItemGripFrame(new(state.PositionX,state.PositionY,state.PositionZ),
            new(state.RotationX,state.RotationY,state.RotationZ,state.RotationW));
        var contact=new ItemAttachmentContact(itemFrame,ItemGripFrame.Identity,state.PositionLimit,state.RotationLimit);
        var selected=state.TargetTriangle<0 ? Array.Empty<uint>() : new[]{checked((uint)state.TargetTriangle)};
        var probes=new ItemSurfaceProbe[state.Count];
        ReadOnlySpan<int> vertices=[state.Probe0,state.Probe1,state.Probe2,state.Probe3];
        for(int i=0;i<probes.Length;++i)
        {
            ItemSurfacePoint point=repeat ? new(state.Source,3,new(.3,.6,.1))
                : new(state.Source,checked((uint)vertices[i]));
            probes[i]=new(point,state.Target,state.DistanceLimit,selected);
        }
        context.TransferItem(state.Item,state.Anchor,contact,probes);
        // Selectors are scratch data and can be reused immediately after staging.
        if(selected.Length!=0)selected[0]=uint.MaxValue;
        Array.Clear(probes);
        if(first && state.SaveFirst!=0)context.RequestSave("surface-auto",0);
    }
}
