// SPDX-License-Identifier: Apache-2.0
// Trusted qualification source. Allocation is intentional, bounded and local.
using System.Numerics;
using System.Runtime.InteropServices;
using Poima;

namespace Poima.Qualification;

[GameplayPersistence(1)]
[StructLayout(LayoutKind.Sequential)]
public struct CallbackProbeState
{
    [GameplayField("d1670000000000000000000000000001")] public int Ticks;
    [GameplayField("d1670000000000000000000000000002")] public int Controls;
    [GameplayField("d1670000000000000000000000000003")] public int Mode;
    [GameplayField("d1670000000000000000000000000004")] public int Witness;
    [GameplayField("d1670000000000000000000000000005")] public EntityId Target;
}

[GameModule("poima.qualification.callback-metrics")]
public sealed class CallbackProbe : Game<CallbackProbeState>
{
    public override void Initialize(ref CallbackProbeState state)
    {
        var held=new byte[32768];held[0]=7;
        state=default;state.Witness=held[0];GC.KeepAlive(held);
    }

    public override void Tick(ref CallbackProbeState state,GameContext context)
    {
        var held=new byte[4096];held[0]=11;
        ++state.Ticks;state.Witness=held[0];
        context.MoveKinematic(new EntityId(0,1671),new Vector3d(state.Ticks,2,0),Quaternion.Identity,60);
        context.SetUi(new UiId(0,1672),text:$"Tick {state.Ticks}");
        GC.KeepAlive(held);
        if(state.Mode==1)
        {
            context.RequestSave("rolled-back-tick",0);
            throw new InvalidOperationException("CallbackProbe actual Tick throw.");
        }
        if(state.Mode==2)state.Target=new EntityId(0,999999); // Returned callback; native candidate must reject.
    }

    public override void Control(ref CallbackProbeState state,ControlContext context)
    {
        var held=new byte[8192];held[0]=13;
        ++state.Controls;state.Witness=held[0];
        context.SetUi(new UiId(0,1672),text:$"Control {state.Controls}");
        GC.KeepAlive(held);
        if(context.Action=="throw")
        {
            context.RequestSave("rolled-back-control",0);
            throw new InvalidOperationException("CallbackProbe actual Control throw.");
        }
        if(context.Action!="allocate")throw new InvalidOperationException("Unknown CallbackProbe action.");
    }
}

[GameModule("poima.qualification.callback-metrics")]
public sealed class ThrowingInitializeProbe : Game<CallbackProbeState>
{
    public override void Initialize(ref CallbackProbeState state)
    {
        var held=new byte[16384];held[0]=17;state.Witness=held[0];GC.KeepAlive(held);
        throw new InvalidOperationException("CallbackProbe actual Initialize throw.");
    }
    public override void Tick(ref CallbackProbeState state,GameContext context) => throw new InvalidOperationException("Unreachable candidate Tick.");
}
