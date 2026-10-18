// SPDX-License-Identifier: Apache-2.0
using System.Collections;
using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json;
using Poima.ManagedBridge;

// Independent 80-byte wire declaration: tests call the actual public Entry,
// rather than linking its internal NativeCall or a synthetic snapshot helper.
[StructLayout(LayoutKind.Sequential)]
unsafe struct Call
{
    public uint Version,Operation;public ulong Handle;public byte* Text,State;
    public uint StateBytes,InputCount;public void* Services,Inputs;public ulong Tick;
    public byte* Output;public uint OutputCapacity,Reserved;
}

static unsafe class Program
{
    static int checks;
    static string fixture="";
    static void Check(bool condition,string message)
    {
        ++checks;if(!condition)throw new InvalidOperationException(message);
    }
    static (int Code,string Text) Invoke(uint operation,ulong handle=0,string? request=null,byte[]? state=null,int capacity=8192)
    {
        byte[] output=new byte[capacity],text=Encoding.UTF8.GetBytes((request ?? "")+"\0");
        fixed(byte* outPtr=output,textPtr=text,statePtr=state)
        {
            Call call=new() {Version=1,Operation=operation,Handle=handle,Text=textPtr,State=statePtr,
                StateBytes=(uint)(state?.Length ?? 0),Output=outPtr,OutputCapacity=(uint)capacity};
            Check(sizeof(Call)==80,"Independent bridge call layout changed.");
            int code=Entry.Invoke((nint)(&call),sizeof(Call));
            int end=Array.IndexOf(output,(byte)0);Check(end>=0,"Output is not NUL-terminated.");
            return (code,Encoding.UTF8.GetString(output,0,end));
        }
    }
    static JsonElement Success(uint operation,ulong handle=0,string? request=null)
    {
        var result=Invoke(operation,handle,request);Check(result.Code==0,result.Text);
        using var document=JsonDocument.Parse(result.Text);return document.RootElement.Clone();
    }
    static void Reject(uint operation,ulong handle,string? request,string expected)
    {
        var result=Invoke(operation,handle,request);
        Check(result.Code==-1 && result.Text.Contains(expected,StringComparison.Ordinal),"Expected rejection: "+result.Text);
    }
    static string LoadRequest(string? option=null,string type="Poima.Qualification.MemoryProbe") =>
        "{\"assembly\":"+JsonSerializer.Serialize(fixture)+",\"type\":"+JsonSerializer.Serialize(type)+
        (option is null ? "" : ",\"memory_snapshot_version\":"+option)+"}";
    static JsonElement Load(string? option=null) => Success(1,request:LoadRequest(option));
    static void Release(ulong handle)
    {
        var result=Invoke(4,handle);Check(result.Code==0 && result.Text=="",result.Text);
    }
    static ulong Decimal(JsonElement value)
    {
        Check(value.ValueKind==JsonValueKind.String,"Unsigned counters must remain JSON strings.");
        string text=value.GetString()!;
        Check(ulong.TryParse(text,NumberStyles.None,CultureInfo.InvariantCulture,out ulong parsed) &&
              parsed.ToString(CultureInfo.InvariantCulture)==text,"Counter is not canonical unsigned decimal.");
        return parsed;
    }
    static void Fields(JsonElement value,params string[] names) =>
        Check(value.EnumerateObject().Select(p=>p.Name).Order().SequenceEqual(names.Order()),"Closed field membership differs.");
    static JsonElement Snapshot(ulong handle)
    {
        long before=Stopwatch.GetTimestamp();var value=Success(8,handle);long after=Stopwatch.GetTimestamp();
        // Retain the bounded raw DTO before semantic assertions, including on
        // failure. Logging allocations are outside the captured bracket.
        Console.Error.WriteLine("RAW_SNAPSHOT "+value.GetRawText());
        Fields(value,"format","version","process_id","timestamp_start","timestamp_end","timestamp_frequency",
            "active_modules","retired_contexts_tracked","retired_contexts_alive","gc","last_gc");
        Check(value.GetProperty("format").GetString()=="poima.managed-memory-snapshot.v1" &&
              value.GetProperty("version").GetRawText()=="1","Snapshot version differs.");
        Check(value.GetProperty("process_id").GetInt32()==Environment.ProcessId,"Process identity differs.");
        ulong start=Decimal(value.GetProperty("timestamp_start")),end=Decimal(value.GetProperty("timestamp_end"));
        Check(start>=(ulong)before && end>=start && end<=(ulong)after &&
              Decimal(value.GetProperty("timestamp_frequency"))==(ulong)Stopwatch.Frequency,"Raw capture bracket differs.");
        int active=value.GetProperty("active_modules").GetInt32(),tracked=value.GetProperty("retired_contexts_tracked").GetInt32(),alive=value.GetProperty("retired_contexts_alive").GetInt32();
        Check(active is >=0 and <=32 && tracked is >=0 and <=1024 && alive>=0 && alive<=tracked,"Context bounds differ.");
        var gc=value.GetProperty("gc");
        Fields(gc,"total_memory_bytes","total_allocated_bytes","gen0_collections","gen1_collections","gen2_collections");
        foreach(var property in gc.EnumerateObject())_ = Decimal(property.Value);
        var last=value.GetProperty("last_gc");
        Fields(last,"index","generation","concurrent","compacted","heap_size_bytes","fragmented_bytes",
            "total_committed_bytes","memory_load_bytes","high_memory_load_threshold_bytes","total_available_memory_bytes");
        ulong index=Decimal(last.GetProperty("index"));int generation=last.GetProperty("generation").GetInt32();
        Check(generation is >=0 and <=2,"GC generation outside CLR bounds.");
        foreach(string name in new[]{"concurrent","compacted"})Check(last.GetProperty(name).ValueKind is JsonValueKind.True or JsonValueKind.False,"GC flag lost Boolean type.");
        foreach(var property in last.EnumerateObject())
            if(property.Name.EndsWith("_bytes",StringComparison.Ordinal))_ = Decimal(property.Value);
        if(index==0)
        {
            Check(generation==0 && !last.GetProperty("concurrent").GetBoolean() && !last.GetProperty("compacted").GetBoolean(),"No-GC raw zero flags differ.");
            foreach(string name in new[]{"heap_size_bytes","fragmented_bytes","total_committed_bytes","memory_load_bytes"})
                Check(Decimal(last.GetProperty(name))==0,"No-GC history was fabricated.");
        }
        Check(Encoding.UTF8.GetByteCount(value.GetRawText())<2048,"Snapshot exceeds minimum bridge buffer.");
        return value;
    }
    static void SameCounts(JsonElement a,JsonElement b)
    {
        foreach(string name in new[]{"active_modules","retired_contexts_tracked"})
            Check(a.GetProperty(name).GetInt32()==b.GetProperty(name).GetInt32(),"Passive query altered "+name);
    }
    static void Negotiation(ulong observer)
    {
        var baseline=Snapshot(observer);
        foreach(string bad in new[]{"false","true","null","\"1\"","0","2","-1","1.0","1e0","{}","[]","\"\"","18446744073709551616"})
        {
            Reject(1,0,LoadRequest(bad),"memory_snapshot_version");SameCounts(baseline,Snapshot(observer));
        }
        string duplicate=LoadRequest("1")[..^1]+",\"memory_snapshot_version\":1}";
        Reject(1,0,duplicate,"memory_snapshot_version");SameCounts(baseline,Snapshot(observer));
        var legacy=Load();ulong old=legacy.GetProperty("handle").GetUInt64();
        Check(!legacy.TryGetProperty("memory_snapshot_version",out _) && !legacy.TryGetProperty("callback_metrics_version",out _),"Legacy load gained opt-in fields.");
        Reject(8,old,null,"not requested");
        var enabled=Load("1");ulong current=enabled.GetProperty("handle").GetUInt64();
        Check(enabled.GetProperty("memory_snapshot_version").GetRawText()=="1","Negotiated load did not echo integer version.");
        string Without(JsonElement document,params string[] excluded) => JsonSerializer.Serialize(
            document.EnumerateObject().Where(p=>!excluded.Contains(p.Name)).ToDictionary(p=>p.Name,p=>p.Value.Clone()));
        Check(Without(legacy,"handle")==Without(enabled,"handle","memory_snapshot_version"),"Opt-in changed old gameplay manifest/schema.");
        var snapshot=Snapshot(current);Check(snapshot.GetProperty("active_modules").GetInt32()==3,"Process-global module count was filtered by requested handle.");
        Release(old);Release(current);Reject(8,current,null,"not requested");Reject(8,ulong.MaxValue,null,"not requested");
        var small=Invoke(8,observer,capacity:2048);Check(small.Code==0,small.Text);
    }
    // Test-only strong retention fixture. Reflection stays outside production
    // diagnostics. Returning only from a no-inline helper ends all strong locals.
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void RetainedContext(ulong observer)
    {
        var loaded=Load("1");ulong handle=loaded.GetProperty("handle").GetUInt64();
        var table=(IDictionary)typeof(Entry).GetField("modules",BindingFlags.NonPublic|BindingFlags.Static)!.GetValue(null)!;
        object module=table[handle]!;
        object context=module.GetType().GetProperty("Context")!.GetValue(module)!;
        Check(context is AssemblyLoadContext {IsCollectible:true},"Fixture did not retain real collectible context.");
        Release(handle);
        var first=Snapshot(observer);int tracked=first.GetProperty("retired_contexts_tracked").GetInt32();
        Check(first.GetProperty("retired_contexts_alive").GetInt32()>=1,"Retained context not visible.");
        // This operation is intentionally explicit collection, separate from the
        // passive path. The retained context must survive even this diagnostic.
        var collected=Success(5);Check(collected.GetProperty("retired_alive").GetInt32()>=1,"Strongly held fixture collected.");
        var after=Snapshot(observer);Check(after.GetProperty("retired_contexts_alive").GetInt32()>=1 &&
            after.GetProperty("retired_contexts_tracked").GetInt32()<=tracked,"Explicit collector did not expose retained root.");
        for(int i=0;i<4;++i)SameCounts(after,Snapshot(observer));
        GC.KeepAlive(module);GC.KeepAlive(context);
    }
    static void Lifetimes(ulong observer)
    {
        RetainedContext(observer);
        var collected=Success(5);Check(collected.GetProperty("retired_alive").GetInt32()==0,"Released test fixture still rooted.");
        var clean=Snapshot(observer);Check(clean.GetProperty("retired_contexts_tracked").GetInt32()==0,"Existing explicit collector did not prune dead contexts.");
        for(int i=0;i<4;++i)
        {
            var loaded=Load("1");ulong handle=loaded.GetProperty("handle").GetUInt64();
            byte[] state=new byte[12];var initialized=Invoke(2,handle,state:state);
            Check(initialized.Code==0 && BitConverter.ToInt32(state,8)==17,"Actual loaded module Initialize failed.");
            Release(handle);
            var snapshot=Snapshot(observer);
            Check(snapshot.GetProperty("active_modules").GetInt32()==1 &&
                  snapshot.GetProperty("retired_contexts_tracked").GetInt32()==i+1,"Passive lifetime query pruned/reset retired entries.");
        }
        var before=Snapshot(observer);
        Reject(1,0,LoadRequest("1","Missing.MemoryType"),"Missing.MemoryType");
        var failed=Snapshot(observer);
        Check(failed.GetProperty("active_modules").GetInt32()==1 &&
              failed.GetProperty("retired_contexts_tracked").GetInt32()==before.GetProperty("retired_contexts_tracked").GetInt32()+1,
              "Failed load context not retired faithfully.");
        SameCounts(failed,Snapshot(observer));
    }
    static List<(MethodBase Target,OpCode Previous)> Calls(MethodInfo method)
    {
        var calls=new List<(MethodBase Target,OpCode Previous)>();
        byte[] code=method.GetMethodBody()?.GetILAsByteArray() ?? [];
        var opcodes=typeof(OpCodes).GetFields(BindingFlags.Public|BindingFlags.Static).Where(f=>f.FieldType==typeof(OpCode))
            .Select(f=>(OpCode)f.GetValue(null)!).ToDictionary(o=>unchecked((ushort)o.Value));
        OpCode previous=default;
        for(int offset=0;offset<code.Length;)
        {
            ushort key=code[offset++];if(key==0xfe)key=(ushort)(0xfe00|code[offset++]);
            OpCode op=opcodes[key];int length=op.OperandType switch
            {
                OperandType.InlineNone=>0,
                OperandType.ShortInlineBrTarget or OperandType.ShortInlineI or OperandType.ShortInlineVar=>1,
                OperandType.InlineVar=>2,
                OperandType.InlineI8 or OperandType.InlineR=>8,
                OperandType.InlineSwitch=>4+4*BitConverter.ToInt32(code,offset),
                _=>4
            };
            if(op.OperandType==OperandType.InlineMethod)
                calls.Add((method.Module.ResolveMethod(BitConverter.ToInt32(code,offset))!,previous));
            offset+=length;previous=op;
        }
        return calls;
    }
    static void PassiveCodeBoundary()
    {
        Type helper=typeof(Entry).Assembly.GetType("Poima.ManagedBridge.MemorySnapshot",true)!;
        foreach(var field in helper.GetFields(BindingFlags.Static|BindingFlags.Instance|BindingFlags.Public|BindingFlags.NonPublic))
            Check(field.FieldType.IsValueType,"Snapshot helper caches an object/Type/delegate.");
        var seen=new HashSet<string>();
        foreach(var method in helper.GetMethods(BindingFlags.Static|BindingFlags.NonPublic|BindingFlags.Public|BindingFlags.DeclaredOnly))
        foreach(var (call,previous) in Calls(method))
        {
            string name=call.DeclaringType!.FullName+"."+call.Name;seen.Add(name);
            Check(name is not ("System.GC.Collect" or "System.GC.WaitForPendingFinalizers" or "System.WeakReference.get_Target" or "System.Runtime.Loader.AssemblyLoadContext.Unload"),"Passive path calls active collection/strong target/unload: "+name);
            if(name is "System.GC.GetTotalMemory" or "System.GC.GetTotalAllocatedBytes")
                Check(previous==OpCodes.Ldc_I4_0,"Passive sampling did not pass literal false: "+name);
        }
        Check(seen.Contains("System.GC.GetTotalMemory") && seen.Contains("System.GC.GetTotalAllocatedBytes") &&
            seen.Contains("System.GC.GetGCMemoryInfo") && seen.Contains("System.WeakReference.get_IsAlive"),"Production passive observations are not inspected.");
        // IL inspection proves forbidden direct calls absent; raw counters alone
        // cannot distinguish natural collection from explicit collection.
    }
    static int Main(string[] args)
    {
        Check(args.Length==1,"Supply the freshly built MemoryProbe.dll path.");fixture=Path.GetFullPath(args[0]);
        Check(File.Exists(fixture),"Missing fixture assembly.");
        PassiveCodeBoundary();var manifest=Load("1");ulong observer=manifest.GetProperty("handle").GetUInt64();
        try
        {
            Negotiation(observer);Lifetimes(observer);
            var a=Snapshot(observer);var held=new byte[1024*1024];held[0]=31;var b=Snapshot(observer);
            Check(Decimal(b.GetProperty("gc").GetProperty("total_allocated_bytes"))>=
                  Decimal(a.GetProperty("gc").GetProperty("total_allocated_bytes")),"Process allocation observation regressed.");
            SameCounts(a,b);GC.KeepAlive(held);
        }
        finally { Release(observer); }
        var final=Success(5);Check(final.GetProperty("active_modules").GetInt32()==0 && final.GetProperty("retired_alive").GetInt32()==0,"Final explicit lifetime cleanup failed.");
        Console.WriteLine(JsonSerializer.Serialize(new {passed=true,checks,
            scope="Actual managed Entry.Invoke/80-byte call negotiation, passive CLR data and collectible lifetimes; explicit collection tests separate; not native hosting, per-game heap attribution or representative memory growth"}));
        return 0;
    }
}
