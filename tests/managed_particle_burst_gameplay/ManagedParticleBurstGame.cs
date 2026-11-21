// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using Poima;

namespace Poima.Tests;

public struct ManagedParticleBurstState
{
    public EntityId TemporarySource;
    public int Mode;
    public int Uses;
    public int Ticks;
    public long FailAt;
}

[GameModule("poima.test.managed-particle-burst")]
public sealed class ManagedParticleBurstGame : Game<ManagedParticleBurstState>, IParticleBurstGame
{
    private static readonly EntityId Player = new(0, 2);
    private static readonly EntityId Press = new(0, 100);
    private static readonly TemplateId TemporarySource = new(0, 110);

    public override void Initialize(ref ManagedParticleBurstState state)
    {
        state = default;
        state.FailAt = -1;
    }

    public override void Tick(ref ManagedParticleBurstState state, GameContext context)
    {
        ++state.Ticks;
        if ((long)context.Tick == state.FailAt)
        {
            context.BurstParticles(Press, 1);
            throw new InvalidOperationException("Particle fixture later-tick failure.");
        }
        if (!context.Pressed(Player, GameAction.Use))
            return;

        ++state.Uses;
        switch (state.Mode)
        {
            case 0:
                OperatePress(ref state, context);
                break;
            case 1:
                context.BurstParticles(Press, 1022);
                break;
            case 2:
                context.BurstParticles(Press, 1023);
                break;
            case 3:
            case 4:
                for (int request = 0; request < (state.Mode == 3 ? 32 : 33); ++request)
                    context.BurstParticles(Press, 1);
                break;
            case 5:
                context.BurstParticles(Player, 1);
                break;
            case 6:
                EntityId canceled = context.Spawn(TemporarySource);
                context.BurstParticles(canceled, 1);
                context.Despawn(canceled);
                break;
            case 7:
                context.BurstParticles(Press, 0);
                break;
            case 8:
                context.BurstParticles(Press, 1025);
                break;
            default:
                throw new InvalidOperationException("Unknown particle fixture mode.");
        }
    }

    private static void OperatePress(ref ManagedParticleBurstState state, GameContext context)
    {
        bool firstUse = state.Uses == 1;
        Vector3d stop = firstUse ? new(-2, 1, -4) : new(-3, 1, -4);
        Quaternion rotation = firstUse ? new(0, 0, .1f, MathF.Sqrt(.99f)) : Quaternion.Identity;
        context.MoveKinematic(Press, stop, rotation, 1);
        context.BurstParticles(Press, 2);

        if (firstUse)
        {
            state.TemporarySource = context.Spawn(TemporarySource,
                new SpawnTransform(new(2, 1, -4), Quaternion.Identity, new(1, 1, 1)));
            context.MoveKinematic(state.TemporarySource, new(1, 1, -4), Quaternion.Identity, 1);
            context.BurstParticles(state.TemporarySource, 1);
        }
    }
}
