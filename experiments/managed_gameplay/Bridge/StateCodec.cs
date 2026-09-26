// SPDX-License-Identifier: Apache-2.0
using System.Buffers.Binary;
using Poima.Lab;

namespace Poima.ManagedLab;

public static class StateCodec
{
    public static ulong Hash(ReadOnlySpan<byte> bytes)
    {
        ulong hash = 14695981039346656037;
        foreach (byte value in bytes) hash = unchecked((hash ^ value) * 1099511628211);
        return hash;
    }
    public static byte[] Encode(ulong schema, ulong tick, ReadOnlySpan<EntitySnapshot> entities)
    {
        var bytes = new byte[40 + entities.Length * 40];
        "PMLAB001"u8.CopyTo(bytes);
        Write(bytes, 8, schema); Write(bytes, 16, (ulong)entities.Length); Write(bytes, 24, tick);
        for (int i = 0; i < entities.Length; ++i)
        {
            int offset = 32 + i * 40;
            Write(bytes, offset, entities[i].Id); Write(bytes, offset + 8, entities[i].PositionMm);
            Write(bytes, offset + 16, entities[i].VelocityMmPerTick); Write(bytes, offset + 24, entities[i].Updates);
            Write(bytes, offset + 32, entities[i].Energy);
        }
        Write(bytes, bytes.Length - 8, Hash(bytes.AsSpan(0, bytes.Length - 8)));
        return bytes;
    }
    public static (ulong Schema, ulong Tick, EntitySnapshot[] Entities) Decode(byte[] bytes, int count)
    {
        if (bytes.Length != 40 + count * 40 || !bytes.AsSpan(0, 8).SequenceEqual("PMLAB001"u8))
            throw new InvalidDataException("Checkpoint format/length mismatch.");
        if (Read(bytes, 16) != (ulong)count || Read(bytes, bytes.Length - 8) != Hash(bytes.AsSpan(0, bytes.Length - 8)))
            throw new InvalidDataException("Checkpoint checksum/count mismatch.");
        var entities = new EntitySnapshot[count];
        for (int i = 0; i < count; ++i)
        {
            int offset = 32 + i * 40;
            entities[i] = new EntitySnapshot { Id = Read(bytes, offset), PositionMm = Read(bytes, offset + 8),
                VelocityMmPerTick = Read(bytes, offset + 16), Updates = Read(bytes, offset + 24), Energy = Read(bytes, offset + 32) };
            if (entities[i].Id != (ulong)i) throw new InvalidDataException("Checkpoint identity mismatch.");
        }
        return (Read(bytes, 8), Read(bytes, 24), entities);
    }
    private static void Write(byte[] bytes, int offset, ulong value) => BinaryPrimitives.WriteUInt64LittleEndian(bytes.AsSpan(offset, 8), value);
    private static ulong Read(byte[] bytes, int offset) => BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(offset, 8));
}
