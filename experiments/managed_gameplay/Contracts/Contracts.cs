// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;

namespace Poima.Lab;

[StructLayout(LayoutKind.Sequential)]
public struct EntitySnapshot
{
    public ulong Id;
    public ulong PositionMm;
    public ulong VelocityMmPerTick;
    public ulong Updates;
    public ulong Energy;
}

public readonly unsafe struct NativeServices(delegate* unmanaged[Cdecl]<ulong, ulong, ulong> add)
{
    public ulong Add(ulong a, ulong b) => add(a, b);
}

// Experimental low-level contract, not the intended ergonomic gameplay SDK.
// Spans are borrowed only for the duration of each call. Never retain pointers,
// start background jobs or register external callbacks in this bounded fixture.
public interface IGame
{
    uint AbiVersion { get; }
    ulong Schema { get; }
    int EntityBytes { get; }
    void Initialize(Span<byte> storage);
    void Step(Span<byte> storage, ulong tick, NativeServices native);
    void Snapshot(ReadOnlySpan<byte> storage, Span<EntitySnapshot> output);
    void Migrate(ReadOnlySpan<EntitySnapshot> old, ulong oldSchema, Span<byte> storage);
}

// Test-only deliberate root: proves the unload diagnostic catches retained
// game references. This is not a proposed gameplay API or global registry.
public static class LifetimeProbe
{
    private static object? root;
    public static void Hold(object value) => root = value;
    public static void Release() => root = null;
}
