// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima.Gameplay;
using Poima.Lab;
using Poima.ManagedLab;

// Bounded shipping experiment: the same Game is compiled into this executable.
// There is no assembly discovery/loading, reflection serialization, scripting,
// or runtime compilation. NativeKernel loads only our process-lifetime C++ DLL.
internal static class Program
{
    private const int Count = 1000;

    private static int Main(string[] args)
    {
        try
        {
            if (args.Length != 4 || !ulong.TryParse(args[3], NumberStyles.None,
                CultureInfo.InvariantCulture, out ulong ticks) || ticks > 10000)
                throw new ArgumentException("Usage: Poima.ShippingLab KERNEL INPUT_OR_DASH OUTPUT TICKS (0..10000).");

            var kernel = new NativeKernel(args[0]);
            var game = new Game();
            if (game.AbiVersion != 1 || game.EntityBytes is <= 0 or > 256 || game.EntityBytes % 8 != 0)
                throw new InvalidOperationException("Invalid game ABI/layout.");
            ulong tick = 0;
            byte[] checkpoint;
            EntitySnapshot[] entities = new EntitySnapshot[Count];
            double loopMs;
            long allocationDelta;
            int collections;
            using (var state = kernel.Allocate(Count * game.EntityBytes))
            {
                if (args[1] == "-") game.Initialize(state.Span);
                else
                {
                    if (new FileInfo(args[1]).Length != 40 + Count * 40)
                        throw new InvalidDataException("Checkpoint length mismatch.");
                    var saved = StateCodec.Decode(File.ReadAllBytes(args[1]), Count);
                    game.Migrate(saved.Entities, saved.Schema, state.Span);
                    tick = saved.Tick;
                }
                if (tick > ulong.MaxValue - ticks) throw new ArgumentException("Tick overflow.");
                // Observe only the update body. Initialization, snapshots, JSON,
                // native allocation and checkpoint I/O are outside this interval.
                long started = Stopwatch.GetTimestamp();
                int beforeCollections = GC.CollectionCount(0) + GC.CollectionCount(1) + GC.CollectionCount(2);
                long beforeAllocation = GC.GetAllocatedBytesForCurrentThread();
                for (ulong i = 0; i < ticks; ++i) game.Step(state.Span, tick + i, kernel.Services);
                allocationDelta = GC.GetAllocatedBytesForCurrentThread() - beforeAllocation;
                collections = GC.CollectionCount(0) + GC.CollectionCount(1) + GC.CollectionCount(2) - beforeCollections;
                loopMs = Stopwatch.GetElapsedTime(started).TotalMilliseconds;
                tick += ticks;
                game.Snapshot(state.Span, entities);
                GC.KeepAlive(state);
                for (int i = 0; i < entities.Length; ++i)
                    if (entities[i].Id != (ulong)i) throw new InvalidDataException("Entity identity changed.");
                checkpoint = StateCodec.Encode(game.Schema, tick, entities);
            }
            // No output is published until all state validation and disposal pass.
            if (kernel.LiveBytes != 0) throw new InvalidOperationException("Native state was not released.");
            WriteAtomic(args[2], checkpoint);
            using var output = new Utf8JsonWriter(Console.OpenStandardOutput());
            output.WriteStartObject();
            output.WriteString("status", "ok");
            output.WriteString("fixture", "managed_shipping_v1");
            output.WriteString("runtime", RuntimeInformation.FrameworkDescription);
            output.WriteBoolean("dynamic_code_supported", RuntimeFeature.IsDynamicCodeSupported);
            output.WriteBoolean("dynamic_code_compiled", RuntimeFeature.IsDynamicCodeCompiled);
            output.WriteNumber("schema", game.Schema);
            output.WriteNumber("entity_bytes", game.EntityBytes);
            output.WriteNumber("entities", Count);
            output.WriteNumber("tick", tick);
            output.WriteNumber("state_hash", StateCodec.Hash(checkpoint));
            output.WriteNumber("native_calls", kernel.NativeCalls);
            output.WriteNumber("native_live_bytes", kernel.LiveBytes);
            output.WriteNumber("loop_ms", loopMs);
            output.WriteNumber("loop_allocated_bytes", allocationDelta);
            output.WriteNumber("loop_collections", collections);
            output.WriteNumber("managed_heap_bytes", GC.GetTotalMemory(false));
            using var process = Process.GetCurrentProcess();
            output.WriteNumber("working_set_bytes", process.WorkingSet64);
            output.WritePropertyName("first"); WriteEntity(output, entities[0]);
            output.WritePropertyName("last"); WriteEntity(output, entities[^1]);
            output.WriteEndObject();
            output.Flush();
            return 0;
        }
        catch (Exception error)
        {
            using var output = new Utf8JsonWriter(Console.OpenStandardOutput());
            output.WriteStartObject();
            output.WriteString("status", "error");
            output.WriteString("message", error.Message);
            output.WriteEndObject();
            output.Flush();
            return 4;
        }
    }

    private static void WriteAtomic(string path, byte[] bytes)
    {
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

    private static void WriteEntity(Utf8JsonWriter output, EntitySnapshot entity)
    {
        output.WriteStartObject();
        output.WriteNumber("id", entity.Id);
        output.WriteNumber("position_mm", entity.PositionMm);
        output.WriteNumber("velocity_mm_per_tick", entity.VelocityMmPerTick);
        output.WriteNumber("updates", entity.Updates);
        output.WriteNumber("energy", entity.Energy);
        output.WriteEndObject();
    }
}
