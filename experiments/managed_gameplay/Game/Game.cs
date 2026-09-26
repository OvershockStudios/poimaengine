// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using Poima.Lab;

namespace Poima.Gameplay;

public sealed class Game : IGame
{
    private const ulong Scale = 1;
    private static readonly bool RejectMigration = false;
    private static readonly bool FailStep = false;
    private static readonly bool RetainProbe = false;
    public Game() { if (RetainProbe) LifetimeProbe.Hold(this); }
    public uint AbiVersion => 1;
#if LAYOUT_V2
    public ulong Schema => 2;
#else
    public ulong Schema => 1;
#endif
    public int EntityBytes => Marshal.SizeOf<Entity>();

    [StructLayout(LayoutKind.Sequential)]
    private struct Entity
    {
        public ulong Id, Position, Velocity, Updates;
#if LAYOUT_V2
        public ulong Energy;
#endif
    }

    public void Initialize(Span<byte> storage)
    {
        var entities = MemoryMarshal.Cast<byte, Entity>(storage);
        for (int i = 0; i < entities.Length; ++i)
        {
            entities[i] = new Entity { Id = (ulong)i, Position = (ulong)i * 3, Velocity = (ulong)(i % 7 + 1) };
#if LAYOUT_V2
            entities[i].Energy = 100;
#endif
        }
    }

    public void Step(Span<byte> storage, ulong tick, NativeServices native)
    {
        var entities = MemoryMarshal.Cast<byte, Entity>(storage);
        for (int i = 0; i < entities.Length; ++i)
        {
            entities[i].Position = native.Add(entities[i].Position, unchecked(entities[i].Velocity * Scale));
            entities[i].Updates = unchecked(entities[i].Updates + 1);
#if LAYOUT_V2
            entities[i].Energy = unchecked(entities[i].Energy + 1);
#endif
        }
        if (FailStep) throw new InvalidOperationException("Intentional update failure after writing staging.");
    }

    public void Snapshot(ReadOnlySpan<byte> storage, Span<EntitySnapshot> output)
    {
        var entities = MemoryMarshal.Cast<byte, Entity>(storage);
        if (entities.Length != output.Length) throw new ArgumentException("Snapshot length mismatch.");
        for (int i = 0; i < entities.Length; ++i)
        {
            output[i] = new EntitySnapshot { Id = entities[i].Id, PositionMm = entities[i].Position,
                VelocityMmPerTick = entities[i].Velocity, Updates = entities[i].Updates };
#if LAYOUT_V2
            output[i].Energy = entities[i].Energy;
#endif
        }
    }

    public void Migrate(ReadOnlySpan<EntitySnapshot> old, ulong oldSchema, Span<byte> storage)
    {
        if (oldSchema != 1 && oldSchema != Schema) throw new InvalidOperationException("Unsupported schema migration.");
        var entities = MemoryMarshal.Cast<byte, Entity>(storage);
        if (entities.Length != old.Length) throw new ArgumentException("Migration length mismatch.");
        for (int i = 0; i < entities.Length; ++i)
        {
            entities[i] = new Entity { Id = old[i].Id, Position = old[i].PositionMm,
                Velocity = old[i].VelocityMmPerTick, Updates = old[i].Updates };
#if LAYOUT_V2
            entities[i].Energy = oldSchema == 1 ? 100 : old[i].Energy;
#endif
        }
        if (RejectMigration) throw new InvalidOperationException("Intentional migration failure after writing staging.");
    }
}
