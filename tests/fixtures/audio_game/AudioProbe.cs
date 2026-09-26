// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;
public struct AudioState
{
    public EntityId Emitter;
    public long Voice;
    public int MoveDoor;
    public int PlayAt,StopAt,FailAt;
}
[GameModule("poima.test.sound-transaction")]
public sealed class AudioProbe : Game<AudioState>
{
    public override void Initialize(ref AudioState state)
    { state.Emitter=new(0,101);state.PlayAt=0;state.StopAt=-1;state.FailAt=-1; }
    public override void Tick(ref AudioState state,GameContext context)
    {
        if((long)context.Tick==state.PlayAt)
        {
            state.Voice=context.PlaySound(state.Emitter);
            if(state.MoveDoor!=0)context.MoveKinematic(new(0,2),new(3,1.5,-3),System.Numerics.Quaternion.Identity,120);
        }
        if((long)context.Tick==state.StopAt)context.StopSound(state.Voice);
        if((long)context.Tick==state.FailAt)throw new InvalidOperationException("Intentional failure after sound events.");
    }
}
