// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using Poima.Lab;

namespace Poima.ManagedLab;

internal sealed class GameContext(string path) : AssemblyLoadContext(isCollectible: true)
{
    private readonly AssemblyDependencyResolver resolver = new(path);
    protected override Assembly? Load(AssemblyName name)
    {
        // The contract must have one identity in the stable bridge context.
        if (name.Name == typeof(IGame).Assembly.GetName().Name) return typeof(IGame).Assembly;
        string? resolved = resolver.ResolveAssemblyToPath(name);
        return resolved is null ? null : LoadFromAssemblyPath(resolved);
    }
}

internal sealed class GameLease(GameContext context, IGame game)
{
    private GameContext? context = context;
    private IGame? game = game;
    public IGame Game => game ?? throw new ObjectDisposedException(nameof(GameLease));
    [MethodImpl(MethodImplOptions.NoInlining)]
    public WeakReference Retire()
    {
        var old = context ?? throw new ObjectDisposedException(nameof(GameLease));
        var reference = new WeakReference(old);
        game = null;
        context = null;
        old.Unload();
        return reference;
    }
}

public sealed record LoopStats(ulong Ticks, double ElapsedMs, long AllocatedBytes, int Gen0, int Gen1, int Gen2);

internal sealed class Host(NativeKernel kernel) : IDisposable
{
    private const int Count = 1000;
    private GameLease? active;
    private NativeBuffer? state;
    private readonly List<WeakReference> retired = [];
    private ulong tick, revision;
    private int contextsCreated, contextsCollected;
    public LoopStats? LastLoop { get; private set; }

    private static void Require(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    private GameLease Create(string path)
    {
        path = Path.GetFullPath(path);
        var context = new GameContext(path);
        ++contextsCreated;
        try
        {
            using var bytes = new MemoryStream(File.ReadAllBytes(path));
            var assembly = context.LoadFromStream(bytes);
            var type = assembly.GetType("Poima.Gameplay.Game", throwOnError: true)!;
            var game = Activator.CreateInstance(type) as IGame ?? throw new InvalidOperationException("Game does not implement the lab contract.");
            return new GameLease(context, game);
        }
        catch
        {
            retired.Add(new WeakReference(context));
            context.Unload();
            throw;
        }
    }
    private static EntitySnapshot[] Snapshot(IGame game, NativeBuffer storage)
    {
        var result = new EntitySnapshot[Count];
        game.Snapshot(storage.Span, result);
        GC.KeepAlive(storage);
        for (int i = 0; i < result.Length; ++i) Require(result[i].Id == (ulong)i, "Entity identity changed.");
        return result;
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    public void Load(string path)
    {
        var candidate = Create(path);
        NativeBuffer? staged = null;
        try
        {
            var game = candidate.Game;
            Require(game.AbiVersion == 1, "Game ABI mismatch.");
            Require(game.Schema != 0 && game.EntityBytes is > 0 and <= 256 && game.EntityBytes % 8 == 0,
                "Invalid entity layout.");
            staged = kernel.Allocate(Count * game.EntityBytes);
            if (active is null) game.Initialize(staged.Span);
            else game.Migrate(Snapshot(active.Game, state!), active.Game.Schema, staged.Span);
            Snapshot(game, staged);
            retired.EnsureCapacity(retired.Count + 1);
        }
        catch
        {
            staged?.Dispose();
            retired.Add(candidate.Retire());
            throw;
        }
        // Preparation failures above cannot touch live state. Retirement is
        // outside that catch: never discard the published candidate if a
        // runtime error occurs while retiring an older context.
        var oldGame = active;
        var oldState = state;
        active = candidate;
        state = staged;
        ++revision;
        oldState?.Dispose();
        if (oldGame is not null) retired.Add(oldGame.Retire());
    }
    public void Step(ulong ticks)
    {
        Require(active is not null && state is not null, "Load a game assembly first.");
        Require(ticks is > 0 and <= 10000 && tick <= ulong.MaxValue - ticks, "Tick count out of range.");
        var staged = state!.Copy();
        try
        {
            int gen0 = GC.CollectionCount(0), gen1 = GC.CollectionCount(1), gen2 = GC.CollectionCount(2);
            long allocated = GC.GetAllocatedBytesForCurrentThread();
            long started = Stopwatch.GetTimestamp();
            for (ulong i = 0; i < ticks; ++i) active!.Game.Step(staged.Span, tick + i, kernel.Services);
            double elapsed = Stopwatch.GetElapsedTime(started).TotalMilliseconds;
            long allocationDelta = GC.GetAllocatedBytesForCurrentThread() - allocated;
            LastLoop = new LoopStats(ticks, elapsed, allocationDelta,
                GC.CollectionCount(0) - gen0, GC.CollectionCount(1) - gen1, GC.CollectionCount(2) - gen2);
            Snapshot(active!.Game, staged);
            var old = state;
            state = staged;
            staged = null!;
            tick += ticks;
            ++revision;
            old.Dispose();
        }
        finally { staged?.Dispose(); }
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    public void CollectRetired()
    {
        // Forced collection is a diagnostic command, not the proposed gameplay
        // scheduling policy. Measure its cost separately from reload/update.
        for (int attempt = 0; attempt < 6 && retired.Any(item => item.IsAlive); ++attempt)
        {
            GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
        }
        contextsCollected += retired.RemoveAll(item => !item.IsAlive);
    }
    private byte[] Checkpoint()
    {
        Require(active is not null && state is not null, "Load a game assembly first.");
        return StateCodec.Encode(active!.Game.Schema, tick, Snapshot(active.Game, state!));
    }
    public void Save(string path)
    {
        var bytes = Checkpoint();
        path = Path.GetFullPath(path);
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            using (var file = new FileStream(temporary, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            { file.Write(bytes); file.Flush(flushToDisk: true); }
            File.Move(temporary, path, overwrite: true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
    public void Restore(string path)
    {
        Require(active is not null && state is not null, "Load a game assembly first.");
        Require(new FileInfo(path).Length == 40 + Count * 40, "Checkpoint length mismatch.");
        var saved = StateCodec.Decode(File.ReadAllBytes(path), Count);
        var staged = kernel.Allocate(Count * active!.Game.EntityBytes);
        try
        {
            active.Game.Migrate(saved.Entities, saved.Schema, staged.Span);
            Snapshot(active.Game, staged);
            var old = state;
            state = staged;
            staged = null!;
            tick = saved.Tick;
            ++revision;
            old!.Dispose();
        }
        finally { staged?.Dispose(); }
    }
    private static object EntityJson(EntitySnapshot entity) => new
    {
        id = entity.Id, position_mm = entity.PositionMm, velocity_mm_per_tick = entity.VelocityMmPerTick,
        updates = entity.Updates, energy = entity.Energy
    };
    public object Inspect()
    {
        var entities = active is null ? null : Snapshot(active.Game, state!);
        var bytes = active is null ? null : Checkpoint();
        return new
        {
            entities = Count, tick, revision, loaded = active is not null,
            schema = active?.Game.Schema, entity_bytes = active?.Game.EntityBytes,
            state_hash = bytes is null ? null : StateCodec.Hash(bytes.AsSpan(0, bytes.Length - 8)).ToString("x16"),
            first = entities is null ? null : EntityJson(entities[0]), last = entities is null ? null : EntityJson(entities[^1]),
            native_calls = kernel.NativeCalls, native_live_bytes = kernel.LiveBytes,
            contexts_created = contextsCreated, contexts_collected = contextsCollected,
            retired_contexts_alive = retired.Count(item => item.IsAlive),
            managed_heap_bytes = GC.GetTotalMemory(forceFullCollection: false),
            resident_bytes = Environment.WorkingSet, runtime = Environment.Version.ToString(),
            last_hot_loop = LastLoop
        };
    }
    public void Dispose()
    {
        if (active is not null) { retired.Add(active.Retire()); active = null; }
        state?.Dispose(); state = null;
    }
}
