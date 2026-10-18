// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Globalization;
using System.Text.Json;

namespace Poima.ManagedBridge;

internal sealed record MemoryGcDocument(string total_memory_bytes,string total_allocated_bytes,
    string gen0_collections,string gen1_collections,string gen2_collections);
internal sealed record MemoryLastGcDocument(string index,int generation,bool concurrent,bool compacted,
    string heap_size_bytes,string fragmented_bytes,string total_committed_bytes,string memory_load_bytes,
    string high_memory_load_threshold_bytes,string total_available_memory_bytes);
internal sealed record MemorySnapshotDocument(string format,int version,int process_id,
    string timestamp_start,string timestamp_end,string timestamp_frequency,int active_modules,
    int retired_contexts_tracked,int retired_contexts_alive,MemoryGcDocument gc,MemoryLastGcDocument last_gc);

// Owner-thread, process-global observations, not a per-game heap measurement.
// Capture holds no collectible context, game Type or delegate. WeakReference
// liveness is sampled without Target access, collection or list pruning.
internal static class MemorySnapshot
{
    internal static bool Requested(JsonElement request)
    {
        if(!request.TryGetProperty("memory_snapshot_version",out var version))return false;
        if(request.EnumerateObject().Count(p=>p.Name=="memory_snapshot_version")!=1 ||
           version.ValueKind!=JsonValueKind.Number || version.GetRawText()!="1")
            throw new ArgumentException("memory_snapshot_version must be the integer 1, once.");
        return true;
    }

    private static string Decimal(long value)
    {
        // Preserve unavailable/invalid observations as an error, never as a
        // fabricated zero or a wrapped unsigned counter.
        if(value<0)throw new InvalidOperationException("Managed memory observation is negative.");
        return value.ToString(CultureInfo.InvariantCulture);
    }

    internal static string Serialize(int activeModules,IReadOnlyList<WeakReference> retired)
    {
        long start=Stopwatch.GetTimestamp();
        if(activeModules is <0 or >32 || retired.Count>1024)
            throw new InvalidOperationException("Managed memory context counts exceed their bounds.");
        int tracked=retired.Count,alive=0;
        foreach(var reference in retired)if(reference.IsAlive)++alive;
        long memory=GC.GetTotalMemory(false),allocated=GC.GetTotalAllocatedBytes(false);
        int gen0=GC.CollectionCount(0),gen1=GC.CollectionCount(1),gen2=GC.CollectionCount(2);
        var last=GC.GetGCMemoryInfo();
        int processId=Environment.ProcessId;
        long end=Stopwatch.GetTimestamp(),frequency=Stopwatch.Frequency;
        if(start<0 || end<start || frequency<=0 || processId<=0 ||
           last.Generation is <0 or >2)
            throw new InvalidOperationException("Managed memory observation metadata is invalid.");
        // Index 0 means no completed GC history, with raw generation 0.
        // Threshold/available bytes are computed independently of that history
        // and may already be nonzero. Recorded GC data is historical; its index
        // need not be monotonic across background/foreground collections.
        // Bracket ends before formatting/serialization. The diagnostic itself
        // allocates and can trigger ordinary GC; this is not an atomic sample.
        return JsonSerializer.Serialize(new MemorySnapshotDocument(
            "poima.managed-memory-snapshot.v1",1,processId,
            Decimal(start),Decimal(end),Decimal(frequency),activeModules,tracked,alive,
            new(Decimal(memory),Decimal(allocated),Decimal(gen0),Decimal(gen1),Decimal(gen2)),
            new(Decimal(last.Index),last.Generation,last.Concurrent,last.Compacted,
                Decimal(last.HeapSizeBytes),Decimal(last.FragmentedBytes),Decimal(last.TotalCommittedBytes),
                Decimal(last.MemoryLoadBytes),Decimal(last.HighMemoryLoadThresholdBytes),
                Decimal(last.TotalAvailableMemoryBytes))));
    }
}
