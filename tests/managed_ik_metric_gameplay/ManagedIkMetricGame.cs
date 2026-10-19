// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct ManagedIkMetricState
{
    public EntityId Rig;
    public int Mode,Ticks,Reads,Writes;
}

// Uses the existing 352-byte orientation extension. Metric policy is the
// native authored rig definition, not a new managed service or state decoder.
[GameModule("poima.test.managed-ik-metric")]
public sealed class ManagedIkMetricGame:Game<ManagedIkMetricState>,ITwoBoneIkOrientationGame
{
    public override void Initialize(ref ManagedIkMetricState state)
    {state=default;state.Rig=new(0,400);}

    public override void Tick(ref ManagedIkMetricState state,GameContext context)
    {
        ++state.Ticks;
        for(uint slot=1;slot<=2;++slot)
        {
            var current=context.GetTwoBoneIkOrientation(state.Rig,slot)
                ?? throw new InvalidOperationException("Original Knight leg chain absent");
            ++state.Reads;
            if(state.Mode is 1 or 2)
            {
                var point=current.Point;
                context.SetTwoBoneIkOrientation(new(state.Rig,slot,point.Target,point.Pole,
                    point.Enabled,point.Space,point.Weight,current.EndOrientation?.Goal));
                ++state.Writes;
            }
        }
        if(state.Mode==2)throw new InvalidOperationException("Original Knight metric rollback fixture");
        state.Mode=0;
    }
}
