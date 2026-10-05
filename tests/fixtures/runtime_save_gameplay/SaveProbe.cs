// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct RuntimeSaveState
{
    public int Ticks;
    public long Minimum, Accumulator;
    public float Fraction;
    public double Precise, LastX;
    public EntityId Target, OptionalTarget;
}

[GameModule("poima.test.runtime-save")]
public sealed class RuntimeSaveProbe : Game<RuntimeSaveState>
{
    public override void Initialize(ref RuntimeSaveState state)
    {
        if(File.Exists(Environment.GetEnvironmentVariable("POIMA_SAVE_FIXTURE_INITIALIZE_MARKER")))
            throw new InvalidOperationException("Initialize must not execute during snapshot restore.");
        state.Minimum=long.MinValue;
        state.Target=new(0,1);
        state.Fraction=.25f;
        state.Precise=.125;
    }
    public override void Tick(ref RuntimeSaveState state,GameContext context)
    {
        state.Ticks=checked(state.Ticks+1);
        state.Accumulator=checked(state.Accumulator+(long)context.Tick);
        state.Fraction+=.25f;
        state.Precise+=.125;
        state.LastX=context.Get(state.Target).Transform.Position.X;
        if(state.OptionalTarget!=default)state.LastX+=context.Get(state.OptionalTarget).Transform.Position.X;
    }
}

// Same state layout and declared module identity, deliberately another type.
// The save binding must distinguish it from RuntimeSaveProbe.
[GameModule("poima.test.runtime-save")]
public sealed class AlternateSaveProbe : Game<RuntimeSaveState>
{
    public override void Initialize(ref RuntimeSaveState state) => state.Target=new(0,1);
    public override void Tick(ref RuntimeSaveState state,GameContext context) => state.Ticks+=100;
}
