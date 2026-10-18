// SPDX-License-Identifier: Apache-2.0
// Trusted test source: bounded allocation/throw witnesses, no world dependencies.
using System.Runtime.InteropServices;
using Poima;

namespace Poima.Qualification;

[StructLayout(LayoutKind.Sequential)]
public struct MemoryProbeState
{
    public int Ticks,Mode,Witness;
}

[GameModule("poima.qualification.memory-snapshot")]
public sealed class MemoryProbe : Game<MemoryProbeState>
{
    public override void Initialize(ref MemoryProbeState state) { state=default;state.Witness=17; }
    public override void Tick(ref MemoryProbeState state,GameContext context)
    {
        var held=new byte[8192];held[0]=23;
        ++state.Ticks;state.Witness=held[0];GC.KeepAlive(held);
        if(state.Mode==1)throw new InvalidOperationException("MemoryProbe intentional Tick failure.");
    }
}

[GameModule("poima.qualification.memory-snapshot")]
public sealed class ThrowingInitializeMemoryProbe : Game<MemoryProbeState>
{
    public override void Initialize(ref MemoryProbeState state)
    {
        state.Witness=29;throw new InvalidOperationException("MemoryProbe intentional Initialize failure.");
    }
    public override void Tick(ref MemoryProbeState state,GameContext context) =>
        throw new InvalidOperationException("Unreachable MemoryProbe Tick.");
}
