// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Text.Json;

namespace Poima.ManagedBridge;

internal enum CallbackKind { Initialize,Tick,Control }
internal enum CallbackIssue
{
    None,ThreadMismatch,NegativeAllocationCounter,NegativeCollectionCounter,
    AllocationCounterRegressed,CollectionCounterRegressed,CounterSaturated
}

// Value-only raw observations; no callback Types, delegates or collectible
// context objects are retained by diagnostics.
internal readonly record struct CallbackBracket(int Thread,long Allocated,int Gen0,int Gen1,int Gen2)
{
    internal static CallbackBracket Capture() => new(Environment.CurrentManagedThreadId,
        GC.GetAllocatedBytesForCurrentThread(),GC.CollectionCount(0),GC.CollectionCount(1),GC.CollectionCount(2));
}

internal struct CallbackCounters
{
    internal ulong Calls,Returned,Threw,Samples,InvalidSamples,AllocatedBytes,MaxAllocatedBytes;
    internal ulong Gen0Collections,Gen1Collections,Gen2Collections;
}

internal sealed record CallbackBucketDocument(string calls,string returned,string threw,string samples,
    string invalid_samples,string allocated_bytes,string max_allocated_bytes,
    string gen0_collections,string gen1_collections,string gen2_collections);
internal sealed record CallbackMetricsDocument(string format,int version,string module_handle,int owner_thread_id,
    bool saturated,string? last_issue,CallbackBucketDocument initialize,CallbackBucketDocument tick,
    CallbackBucketDocument control);

// A module's owner invokes this on its one managed callback thread. Counters
// describe managed callback attempts, not successful native commits. Native
// transaction rollback intentionally does not roll these diagnostics back.
internal sealed class CallbackMetrics
{
    private readonly ulong handle;
    private readonly int ownerThread;
    private CallbackCounters initialize,tick,control;
    private bool saturated;
    private CallbackIssue lastIssue;

    internal CallbackMetrics(ulong moduleHandle,int thread)
    {
        if(moduleHandle==0 || thread<=0)throw new ArgumentException("Metrics require a positive module/thread identity.");
        handle=moduleHandle;ownerThread=thread;
    }

    internal CallbackBracket Begin() => CallbackBracket.Capture();

    internal void Complete(CallbackKind kind,in CallbackBracket begin,bool returned)
    {
        var end=CallbackBracket.Capture();CompleteObserved(kind,in begin,in end,returned);
    }

    // Shared value-level implementation permits independent synthetic edge
    // tests without altering CLR counters or forcing collection in a game.
    internal void CompleteObserved(CallbackKind kind,in CallbackBracket begin,in CallbackBracket end,bool returned)
    {
        ref var bucket=ref Counters(kind);
        Add(ref bucket.Calls,1);
        if(returned)Add(ref bucket.Returned,1);else Add(ref bucket.Threw,1);
        var issue=begin.Thread!=ownerThread || end.Thread!=ownerThread ? CallbackIssue.ThreadMismatch :
            begin.Allocated<0 || end.Allocated<0 ? CallbackIssue.NegativeAllocationCounter :
            begin.Gen0<0 || begin.Gen1<0 || begin.Gen2<0 || end.Gen0<0 || end.Gen1<0 || end.Gen2<0 ? CallbackIssue.NegativeCollectionCounter :
            end.Allocated<begin.Allocated ? CallbackIssue.AllocationCounterRegressed :
            end.Gen0<begin.Gen0 || end.Gen1<begin.Gen1 || end.Gen2<begin.Gen2 ? CallbackIssue.CollectionCounterRegressed : CallbackIssue.None;
        if(issue!=CallbackIssue.None)
        {
            lastIssue=issue;Add(ref bucket.InvalidSamples,1);return;
        }
        Add(ref bucket.Samples,1);
        var allocated=(ulong)(end.Allocated-begin.Allocated);
        Add(ref bucket.AllocatedBytes,allocated);
        if(allocated>bucket.MaxAllocatedBytes)bucket.MaxAllocatedBytes=allocated;
        Add(ref bucket.Gen0Collections,(ulong)(end.Gen0-begin.Gen0));
        Add(ref bucket.Gen1Collections,(ulong)(end.Gen1-begin.Gen1));
        Add(ref bucket.Gen2Collections,(ulong)(end.Gen2-begin.Gen2));
    }

    internal ref CallbackCounters Counters(CallbackKind kind)
    {
        switch(kind)
        {
            case CallbackKind.Initialize:return ref initialize;
            case CallbackKind.Tick:return ref tick;
            case CallbackKind.Control:return ref control;
            default:throw new ArgumentOutOfRangeException(nameof(kind));
        }
    }

    private void Add(ref ulong destination,ulong value)
    {
        if(value>ulong.MaxValue-destination)
        {
            destination=ulong.MaxValue;saturated=true;lastIssue=CallbackIssue.CounterSaturated;
        }
        else destination+=value;
    }

    private static string Decimal(ulong value) => value.ToString(CultureInfo.InvariantCulture);
    private static CallbackBucketDocument Document(in CallbackCounters value) => new(
        Decimal(value.Calls),Decimal(value.Returned),Decimal(value.Threw),Decimal(value.Samples),
        Decimal(value.InvalidSamples),Decimal(value.AllocatedBytes),Decimal(value.MaxAllocatedBytes),
        Decimal(value.Gen0Collections),Decimal(value.Gen1Collections),Decimal(value.Gen2Collections));
    private static string? IssueText(CallbackIssue issue) => issue switch
    {
        CallbackIssue.None=>null,
        CallbackIssue.ThreadMismatch=>"thread_mismatch",
        CallbackIssue.NegativeAllocationCounter=>"negative_allocation_counter",
        CallbackIssue.NegativeCollectionCounter=>"negative_collection_counter",
        CallbackIssue.AllocationCounterRegressed=>"allocation_counter_regressed",
        CallbackIssue.CollectionCounterRegressed=>"collection_counter_regressed",
        CallbackIssue.CounterSaturated=>"counter_saturated",
        _=>throw new InvalidOperationException("Unknown internal metrics issue.")
    };

    internal string Serialize() => JsonSerializer.Serialize(new CallbackMetricsDocument(
        "poima.managed-callback-metrics.v1",1,Decimal(handle),ownerThread,saturated,IssueText(lastIssue),
        Document(in initialize),Document(in tick),Document(in control)));
}
