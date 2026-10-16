// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text.Json;
using Poima.ManagedBridge;

static class Program
{
    static int checks;
    static void Check(bool value,string reason)
    {
        ++checks;if(!value)throw new InvalidOperationException(reason);
    }
    static JsonElement Snapshot(CallbackMetrics metrics)
    {
        using var document=JsonDocument.Parse(metrics.Serialize());return document.RootElement.Clone();
    }
    static ulong Counter(JsonElement root,string bucket,string name)
    {
        var field=root.GetProperty(bucket).GetProperty(name);
        Check(field.ValueKind==JsonValueKind.String,"Counter must retain unsigned decimal string type.");
        string text=field.GetString()!;
        Check(ulong.TryParse(text,NumberStyles.None,CultureInfo.InvariantCulture,out ulong result) &&
              result.ToString(CultureInfo.InvariantCulture)==text,"Counter must be canonical full uint64.");
        return result;
    }
    static void EdgeCases()
    {
        var metrics=new CallbackMetrics(ulong.MaxValue,7);
        var first=new CallbackBracket(7,100,10,20,30);
        var last=new CallbackBracket(7,240,12,23,34);
        metrics.CompleteObserved(CallbackKind.Initialize,in first,in last,true);
        var zero=new CallbackBracket(7,300,50,60,70);
        metrics.CompleteObserved(CallbackKind.Tick,in zero,in zero,false);
        var data=Snapshot(metrics);
        Check(data.GetProperty("module_handle").GetString()=="18446744073709551615" &&
              data.GetProperty("owner_thread_id").GetInt32()==7,"Exact module/thread identity lost.");
        Check(Counter(data,"initialize","allocated_bytes")==140 &&
              Counter(data,"initialize","max_allocated_bytes")==140 &&
              Counter(data,"initialize","gen0_collections")==2 &&
              Counter(data,"initialize","gen1_collections")==3 &&
              Counter(data,"initialize","gen2_collections")==4,"Valid raw deltas differ.");
        Check(Counter(data,"tick","calls")==1 && Counter(data,"tick","threw")==1 &&
              Counter(data,"tick","samples")==1 && Counter(data,"tick","allocated_bytes")==0,
              "A throwing zero-allocation callback is still a valid measured attempt.");
        var smaller=new CallbackBracket(7,140,10,20,30);
        metrics.CompleteObserved(CallbackKind.Initialize,in first,in smaller,true);
        data=Snapshot(metrics);
        Check(Counter(data,"initialize","allocated_bytes")==180 &&
              Counter(data,"initialize","max_allocated_bytes")==140,
              "Maximum allocation was summed or the running total was replaced.");
        (CallbackBracket End,string Issue)[] invalid=[
            (new(8,240,12,23,34),"thread_mismatch"),
            (new(7,-1,12,23,34),"negative_allocation_counter"),
            (new(7,240,12,-1,34),"negative_collection_counter"),
            (new(7,99,12,23,34),"allocation_counter_regressed"),
            (new(7,240,9,23,34),"collection_counter_regressed")];
        foreach(var (end,issue) in invalid)
        {
            metrics.CompleteObserved(CallbackKind.Control,in first,in end,true);
            var observed=Snapshot(metrics);
            Check(observed.GetProperty("last_issue").GetString()==issue,"Invalid bracket diagnostic differs.");
            Check(Counter(observed,"control","allocated_bytes")==0 &&
                  Counter(observed,"control","samples")==0 &&
                  Counter(observed,"control","gen0_collections")==0,
                  "Invalid raw observation was coerced into a measured zero/delta.");
        }
        var badStart=new CallbackBracket(7,100,10,-1,30);
        metrics.CompleteObserved(CallbackKind.Control,in badStart,in last,false);
        var badThread=new CallbackBracket(8,100,10,20,30);
        metrics.CompleteObserved(CallbackKind.Control,in badThread,in last,false);
        var control=Snapshot(metrics);
        Check(Counter(control,"control","calls")==7 && Counter(control,"control","invalid_samples")==7 &&
              Counter(control,"control","returned")==5 && Counter(control,"control","threw")==2,
              "Invalid attempts/return/throw conservation differs.");
        Check(!control.GetProperty("saturated").GetBoolean(),"Normal counters unexpectedly saturated.");
        string before=metrics.Serialize();_ = metrics.Serialize();
        Check(before==metrics.Serialize(),"Serialization changed cached measurements.");
        string[] fields=["calls","returned","threw","samples","invalid_samples","allocated_bytes",
                         "max_allocated_bytes","gen0_collections","gen1_collections","gen2_collections"];
        foreach(string name in new[]{"initialize","tick","control"})
        {
            Check(control.GetProperty(name).EnumerateObject().Select(p=>p.Name).Order().SequenceEqual(fields.Order()),
                  "Bucket field membership changed.");
            Check(Counter(control,name,"calls")==Counter(control,name,"returned")+Counter(control,name,"threw") &&
                  Counter(control,name,"calls")==Counter(control,name,"samples")+Counter(control,name,"invalid_samples"),
                  "Unsaturated attempt/sample/return conservation differs.");
        }
        Check(control.EnumerateObject().Count()==9 && control.GetProperty("version").GetInt32()==1 &&
              control.GetProperty("format").GetString()=="poima.managed-callback-metrics.v1",
              "Exact diagnostic data envelope differs.");
    }
    static void Saturation()
    {
        var metrics=new CallbackMetrics(9,7);
        ref var counters=ref metrics.Counters(CallbackKind.Tick);
        counters.Calls=counters.Returned=counters.Samples=ulong.MaxValue;
        counters.AllocatedBytes=ulong.MaxValue-2;counters.Gen0Collections=ulong.MaxValue-1;
        var begin=new CallbackBracket(7,0,1,1,1);var end=new CallbackBracket(7,100,4,1,1);
        metrics.CompleteObserved(CallbackKind.Tick,in begin,in end,true);
        var data=Snapshot(metrics);
        Check(data.GetProperty("saturated").GetBoolean() &&
              data.GetProperty("last_issue").GetString()=="counter_saturated","Saturation is not explicit.");
        Check(Counter(data,"tick","calls")==ulong.MaxValue && Counter(data,"tick","returned")==ulong.MaxValue &&
              Counter(data,"tick","samples")==ulong.MaxValue && Counter(data,"tick","allocated_bytes")==ulong.MaxValue &&
              Counter(data,"tick","gen0_collections")==ulong.MaxValue && Counter(data,"tick","max_allocated_bytes")==100,
              "A saturated counter rolled over or invented the maximum allocation.");
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void Allocate(byte[][] held,int index)
    {
        held[index]=new byte[4096+index];held[index][0]=123;
    }
    static void ActualAllocationsAndThrows()
    {
        var metrics=new CallbackMetrics(3,Environment.CurrentManagedThreadId);
        var held=new byte[2][];
        var begin=metrics.Begin();bool returned=false;
        try { Allocate(held,0);returned=true; }
        finally { metrics.Complete(CallbackKind.Tick,in begin,returned); }
        begin=metrics.Begin();returned=false;
        try
        {
            try { Allocate(held,1);throw new InvalidOperationException("Intentional measured callback throw."); }
            finally { metrics.Complete(CallbackKind.Control,in begin,returned); }
        }
        catch(InvalidOperationException error) { Check(error.Message.StartsWith("Intentional"),"Unexpected measured throw."); }
        var data=Snapshot(metrics);
        Check(Counter(data,"tick","calls")==1 && Counter(data,"tick","returned")==1 &&
              Counter(data,"tick","threw")==0 && Counter(data,"tick","allocated_bytes")>=4096,
              "Actual allocating returned callback was not measured.");
        Check(Counter(data,"control","calls")==1 && Counter(data,"control","returned")==0 &&
              Counter(data,"control","threw")==1 && Counter(data,"control","allocated_bytes")>=4097,
              "Actual allocating thrown callback was not measured in finally.");
        Check(held[0][0]==123 && held[1][0]==123,"Actual allocation witnesses were not retained.");
        GC.KeepAlive(held);
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void EmptyAttempt(CallbackMetrics metrics)
    {
        var begin=metrics.Begin();metrics.Complete(CallbackKind.Tick,in begin,true);
    }
    static void NoPerCallbackHeap()
    {
        var metrics=new CallbackMetrics(4,Environment.CurrentManagedThreadId);
        for(int i=0;i<4096;++i)EmptyAttempt(metrics);
        long before=GC.GetAllocatedBytesForCurrentThread();
        for(int i=0;i<8192;++i)EmptyAttempt(metrics);
        long after=GC.GetAllocatedBytesForCurrentThread();
        Check(after==before,"Warmed metrics callback path allocated on the owner thread.");
        var data=Snapshot(metrics);
        Check(Counter(data,"tick","calls")==12288 && Counter(data,"tick","samples")==12288 &&
              Counter(data,"tick","allocated_bytes")==0 && Counter(data,"tick","invalid_samples")==0,
              "Warmed value-only path measured hidden allocation or missed attempts.");
        foreach(var field in typeof(CallbackMetrics).GetFields(BindingFlags.Instance|BindingFlags.NonPublic))
            Check(field.FieldType.IsValueType,"Metrics retain an object/delegate/Type reference.");
    }
    static int Main()
    {
        EdgeCases();Saturation();ActualAllocationsAndThrows();NoPerCallbackHeap();
        Console.WriteLine(JsonSerializer.Serialize(new {passed=true,checks,
            scope="Helper arithmetic and actual CLR allocation/throw brackets; no native commit, game rollback, hosted ABI or performance-overhead qualification"}));
        return 0;
    }
}
