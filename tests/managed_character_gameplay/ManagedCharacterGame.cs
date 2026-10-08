// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Tests;

public struct ManagedCharacterState
{
#if INCOMPATIBLE_STATE
    public int Mode;
    public double Ticks;
    public int StagedReads, PhysicalInputs;
#else
    public int Mode, Ticks, StagedReads, PhysicalInputs;
#endif
    public EntityId Actor, Player;
    public long FailAt;
    public double BeforeX, BeforeY, BeforeZ;
}

[GameModule("poima.test.managed-character")]
public sealed class ManagedCharacterGame : Game<ManagedCharacterState>, ICharacterInputGame
{
    public override void Initialize(ref ManagedCharacterState state)
    {
        state = default;
        state.Actor = new(0, 300);
        state.Player = new(0, 100);
        state.FailAt = -1;
    }

    public override void Tick(ref ManagedCharacterState state, GameContext context)
    {
        ++state.Ticks;
        state.PhysicalInputs = context.Inputs.Length;
        var before = context.Get(state.Actor);
        var position = before.Transform.Position;
        state.BeforeX = position.X; state.BeforeY = position.Y; state.BeforeZ = position.Z;
        switch (state.Mode)
        {
            case 0: break;
            case 1: context.SetCharacterInput(state.Actor, 0, 1); break;
            case 2: context.SetCharacterInput(state.Actor, 0, 1, 90, 30, true); break;
            case 3:
                context.SetCharacterInput(state.Actor, 0, 1);
                context.SetCharacterInput(state.Actor, 1, 0);
                break;
            case 4: context.SetCharacterInput(new(0, 1), 0, 1); break;
            case 5: context.SetCharacterInput(new(0, 999999), 0, 1); break;
            case 6:
                context.SetCharacterInput(state.Actor, 0, 1);
                if ((long)context.Tick == state.FailAt)
                    throw new InvalidOperationException("Character later-tick rollback fixture");
                break;
            case 7:
                context.SetCharacterInput((long)context.Tick == state.FailAt ? state.Player : state.Actor, 0, 1);
                break;
            case 8: context.SetCharacterInput(state.Actor, float.NaN, 0); break;
            case 9: context.SetCharacterInput(state.Actor, 0, 2); break;
            case 10: context.SetCharacterInput(state.Actor, 0, 1, 181); break;
            case 11: context.SetCharacterInput(state.Actor, 0, 0); break;
            default: throw new InvalidOperationException("Unknown character fixture mode");
        }
        var after = context.Get(state.Actor);
        if (after.Transform.Position != position || after.Velocity != before.Velocity)
            throw new InvalidOperationException("Staged character input leaked into same-tick reads");
        ++state.StagedReads;
    }
}
