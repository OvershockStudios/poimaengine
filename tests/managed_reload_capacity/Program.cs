// SPDX-License-Identifier: Apache-2.0
using System.Collections;
using System.Diagnostics;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

// Independent declaration: calls the actual compiled Entry, not its internal
// NativeCall or a replacement capacity predicate.
[StructLayout(LayoutKind.Sequential)]
unsafe struct Call
{
    public uint Version,Operation;public ulong Handle;public byte* Text,State;
    public uint StateBytes,InputCount;public void* Services,Inputs;public ulong Tick;
    public byte* Output;public uint OutputCapacity,Reserved;
}

static unsafe class Program
{
    const string BudgetError="Managed context budget reached; collect diagnostics or restart the session.";
    static string fixture="",mode="";
    static int checks,sequence,acceptedLoads,releases,collectCalls,testHostGcRounds;
    static bool decisiveProbeFinished;
    static readonly Stopwatch clock=Stopwatch.StartNew();
    // Entry redirects Console.Out to stderr for native protocol safety. Keep
    // the test's original stdout for its one terminal machine-readable receipt.
    static readonly TextWriter standardOutput=Console.Out;
    static readonly Dictionary<string,object> stages=[];
    static readonly Dictionary<string,int> operationCounts=[];
    static JsonElement referenceManifest;
    static void Check(bool condition,string message)
    {
        ++checks;if(!condition)throw new InvalidOperationException(message);
    }
    static object Identity(string path)
    {
        using var input=File.OpenRead(path);
        return new {path=Path.GetFullPath(path),bytes=input.Length,
            sha256=Convert.ToHexStringLower(SHA256.HashData(input))};
    }
    static (int Code,string Text) Invoke(uint operation,ulong handle=0,string? request=null,byte[]? state=null)
    {
        Check(clock.Elapsed.TotalSeconds<90,"Bounded capacity fixture deadline reached.");
        byte[] output=new byte[8192],text=Encoding.UTF8.GetBytes((request ?? "")+"\0");
        fixed(byte* outPtr=output,textPtr=text,statePtr=state)
        {
            Call call=new() {Version=1,Operation=operation,Handle=handle,Text=textPtr,State=statePtr,
                StateBytes=(uint)(state?.Length ?? 0),Output=outPtr,OutputCapacity=(uint)output.Length};
            Check(sizeof(Call)==80,"Independent call layout changed.");
            int code=Entry.Invoke((nint)(&call),sizeof(Call));
            int end=Array.IndexOf(output,(byte)0);
            string result=end<0 ? Convert.ToHexString(output) : Encoding.UTF8.GetString(output,0,end);
            ++sequence;string key=operation.ToString(System.Globalization.CultureInfo.InvariantCulture);
            operationCounts[key]=operationCounts.GetValueOrDefault(key)+1;
            Console.Error.WriteLine("RAW_CALL "+JsonSerializer.Serialize(new {
                sequence,operation,handle,request,code,output=result,terminated=end>=0,
                state_hex=state is null ? null : Convert.ToHexStringLower(state)}));
            Check(end>=0,"Bridge output lacks NUL terminator.");
            return (code,result);
        }
    }
    static JsonElement JsonResult((int Code,string Text) result)
    {
        Check(result.Code==0,result.Text);
        using var document=JsonDocument.Parse(result.Text);return document.RootElement.Clone();
    }
    static string Request() => JsonSerializer.Serialize(new {
        assembly=fixture,type="Poima.Qualification.MemoryProbe",memory_snapshot_version=1});
    static string ManifestWithoutHandle(JsonElement manifest) => JsonSerializer.Serialize(
        manifest.EnumerateObject().Where(p=>p.Name!="handle")
            .ToDictionary(p=>p.Name,p=>p.Value.Clone()));
    static JsonElement Load()
    {
        var result=JsonResult(Invoke(1,request:Request()));++acceptedLoads;
        Check(result.GetProperty("handle").GetUInt64()>0 &&
              result.GetProperty("identity").GetString()=="poima.qualification.memory-snapshot" &&
              result.GetProperty("bytes").GetInt32()==12 &&
              result.GetProperty("memory_snapshot_version").GetRawText()=="1",
              "Actual fixture manifest or negotiated observer differs.");
        using(var input=File.OpenRead(fixture))
            Check(result.GetProperty("assembly_sha256").GetString()==
                  Convert.ToHexStringLower(SHA256.HashData(input)),"Loaded fixture hash differs.");
        if(referenceManifest.ValueKind==JsonValueKind.Undefined)referenceManifest=result;
        else Check(ManifestWithoutHandle(result)==ManifestWithoutHandle(referenceManifest),
                   "Reload changed the complete fixture manifest/schema.");
        return result;
    }
    static void Release(ulong handle)
    {
        var result=Invoke(4,handle);Check(result.Code==0 && result.Text=="",result.Text);++releases;
    }
    static JsonElement Snapshot(ulong observer,string label)
    {
        var result=JsonResult(Invoke(8,observer));
        stages[label]=result;
        Check(result.GetProperty("format").GetString()=="poima.managed-memory-snapshot.v1" &&
              result.GetProperty("version").GetRawText()=="1" &&
              result.GetProperty("process_id").GetInt32()==Environment.ProcessId,
              "Snapshot identity/version differs.");
        int active=result.GetProperty("active_modules").GetInt32();
        int tracked=result.GetProperty("retired_contexts_tracked").GetInt32();
        int alive=result.GetProperty("retired_contexts_alive").GetInt32();
        Check(active is >=0 and <=32 && tracked is >=0 and <=1024 && alive>=0 && alive<=tracked,
              "Original snapshot count bounds differ.");
        return result;
    }
    static void Counts(JsonElement snapshot,int active,int tracked,int alive)
    {
        Check(snapshot.GetProperty("active_modules").GetInt32()==active &&
              snapshot.GetProperty("retired_contexts_tracked").GetInt32()==tracked &&
              snapshot.GetProperty("retired_contexts_alive").GetInt32()==alive,
              "Exact actual module/tracked/alive counts differ.");
    }
    static IDictionary Modules() => (IDictionary)typeof(Entry)
        .GetField("modules",BindingFlags.NonPublic|BindingFlags.Static)!.GetValue(null)!;
    static int RetiredCount() => ((ICollection)typeof(Entry)
        .GetField("retired",BindingFlags.NonPublic|BindingFlags.Static)!.GetValue(null)!).Count;
    static ulong NextHandle() => (ulong)typeof(Entry)
        .GetField("next",BindingFlags.NonPublic|BindingFlags.Static)!.GetValue(null)!;
    static void RejectBudget(string label)
    {
        int active=Modules().Count,tracked=RetiredCount();ulong next=NextHandle();
        var result=Invoke(1,request:Request());
        stages[label]=new {code=result.Code,text=result.Text,active_before=active,
            tracked_before=tracked,next_before=next,active_after=Modules().Count,
            tracked_after=RetiredCount(),next_after=NextHandle()};
        Check(result.Code==-1 && result.Text==BudgetError,"Expected exact original budget rejection.");
        Check(Modules().Count==active && RetiredCount()==tracked && NextHandle()==next,
              "Rejected admission created/published/retired a candidate.");
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void HoldRealRetirements(ulong observer)
    {
        // No list seeding or synthetic WeakReference. Every entry below comes
        // from an actual accepted load followed by actual Entry.Release.
        var roots=new List<AssemblyLoadContext>(1024);
        var distinct=new HashSet<AssemblyLoadContext>(ReferenceEqualityComparer.Instance);
        var handles=new HashSet<ulong>();
        for(int i=0;i<1024;++i)
        {
            ulong handle=Load().GetProperty("handle").GetUInt64();
            Check(handles.Add(handle),"Actual load handle was reused.");
            object module=Modules()[handle]!;
            var context=(AssemblyLoadContext)module.GetType().GetProperty("Context")!.GetValue(module)!;
            Check(context.IsCollectible && distinct.Add(context),"Expected distinct real collectible context.");
            roots.Add(context);Release(handle);
            Check(RetiredCount()==i+1 && Modules().Count==1,"Actual retirement history differs.");
        }
        Counts(Snapshot(observer,"alive-capacity"),1,1024,1024);
        RejectBudget("alive-capacity-rejection");
        Counts(Snapshot(observer,"alive-capacity-after-rejection"),1,1024,1024);
        stages["real-retirements"]=new {loads=handles.Count,releases=roots.Count,
            distinct_collectible_contexts=distinct.Count,strong_roots_scope="NoInlining test frame only"};
        GC.KeepAlive(roots);GC.KeepAlive(distinct);
    }
    static void EstablishDeadHistory(ulong observer)
    {
        // The no-inline frame has fully returned. Collection here is a TEST
        // precondition, outside the bridge's load/query implementation. Never
        // call operation5: that would remove the capacity precondition.
        for(int round=1;round<=8;++round)
        {
            GC.Collect();GC.WaitForPendingFinalizers();GC.Collect();++testHostGcRounds;
            var result=Snapshot(observer,"test-host-gc-"+round);
            Check(result.GetProperty("active_modules").GetInt32()==1 &&
                  result.GetProperty("retired_contexts_tracked").GetInt32()==1024,
                  "Test-host collection or passive query pruned retirement history.");
            if(result.GetProperty("retired_contexts_alive").GetInt32()==0)return;
        }
        throw new InvalidOperationException("Eight bounded test-host GC rounds did not establish dead-history precondition.");
    }
    static void Initialize(ulong handle)
    {
        byte[] state=new byte[12];var result=Invoke(2,handle,state:state);
        stages["candidate-initialize"]=new {code=result.Code,text=result.Text,
            state_hex=Convert.ToHexStringLower(state),witness=BitConverter.ToInt32(state,8)};
        Check(result.Code==0 && result.Text=="" && BitConverter.ToInt32(state,0)==0 &&
              BitConverter.ToInt32(state,4)==0 && BitConverter.ToInt32(state,8)==17,
              "Recovered module did not execute real MemoryProbe.Initialize.");
    }
    static void ActiveCapacity(ulong observer)
    {
        // This runs only AFTER the decisive retirement-capacity probe, so its
        // releases cannot contaminate the exact1024 precondition.
        var handles=new List<ulong>();
        int initial=Modules().Count;
        for(int count=initial;count<32;++count)
        {
            handles.Add(Load().GetProperty("handle").GetUInt64());
            Check(Modules().Count==count+1,"Accepted load did not publish its actual active module.");
        }
        int tracked=RetiredCount();
        var before=Snapshot(observer,"active-capacity");
        Check(before.GetProperty("active_modules").GetInt32()==32,"Expected actual32 active modules.");
        RejectBudget("active-capacity-rejection");
        var after=Snapshot(observer,"active-capacity-after-rejection");
        Check(after.GetProperty("active_modules").GetInt32()==32 &&
              after.GetProperty("retired_contexts_tracked").GetInt32()==tracked,
              "Active-cap rejection changed module/retired membership.");
        foreach(ulong handle in handles)Release(handle);
    }
    static IEnumerable<MethodBase> References(MethodInfo method)
    {
        byte[] code=method.GetMethodBody()?.GetILAsByteArray() ?? [];
        var codes=typeof(OpCodes).GetFields(BindingFlags.Public|BindingFlags.Static)
            .Where(f=>f.FieldType==typeof(OpCode)).Select(f=>(OpCode)f.GetValue(null)!)
            .ToDictionary(op=>unchecked((ushort)op.Value));
        for(int offset=0;offset<code.Length;)
        {
            ushort key=code[offset++];if(key==0xfe)key=(ushort)(0xfe00|code[offset++]);
            OpCode op=codes[key];int length=op.OperandType switch {
                OperandType.InlineNone=>0,
                OperandType.ShortInlineBrTarget or OperandType.ShortInlineI or OperandType.ShortInlineVar=>1,
                OperandType.InlineVar=>2,
                OperandType.InlineI8 or OperandType.InlineR=>8,
                OperandType.InlineSwitch=>4+4*BitConverter.ToInt32(code,offset),_=>4};
            if(op.OperandType==OperandType.InlineMethod)
                yield return method.Module.ResolveMethod(BitConverter.ToInt32(code,offset))!;
            offset+=length;
        }
    }
    static void LoadCodeBoundary()
    {
        var load=typeof(Entry).GetMethod("Load",BindingFlags.NonPublic|BindingFlags.Static)!;
        var work=new Queue<MethodInfo>();work.Enqueue(load);var visited=new HashSet<MethodInfo>();
        bool removeAll=false,isAlive=false;
        while(work.TryDequeue(out var method))
        {
            if(!visited.Add(method))continue;
            foreach(var target in References(method))
            {
                string name=target.DeclaringType?.FullName+"."+target.Name;
                Check(name is not ("System.GC.Collect" or "System.GC.WaitForPendingFinalizers" or
                    "System.WeakReference.get_Target" or "System.WeakReference.set_Target"),
                    "Load/prune IL references active collection or a strong weak target: "+name);
                if(target.Name=="RemoveAll" && target.DeclaringType==typeof(List<WeakReference>))removeAll=true;
                if(name=="System.WeakReference.get_IsAlive")isAlive=true;
                if(target is MethodInfo next && next.Module==load.Module &&
                   next.Name.StartsWith("<Load>",StringComparison.Ordinal))work.Enqueue(next);
            }
        }
        Check(removeAll==(mode=="candidate"),"Selected mode does not match actual compiled Load capacity path.");
        if(mode=="candidate")Check(isAlive,"Candidate prune does not inspect WeakReference.IsAlive.");
        var passive=typeof(Entry).Assembly.GetType("Poima.ManagedBridge.MemorySnapshot",true)!;
        foreach(var method in passive.GetMethods(BindingFlags.NonPublic|BindingFlags.Public|
                                               BindingFlags.Static|BindingFlags.DeclaredOnly))
        foreach(var target in References(method))
        {
            string name=target.DeclaringType?.FullName+"."+target.Name;
            Check(name is not ("System.GC.Collect" or "System.GC.WaitForPendingFinalizers" or
                "System.WeakReference.get_Target" or "System.WeakReference.set_Target" or
                "System.Runtime.Loader.AssemblyLoadContext.Unload") &&
                !(target.Name=="RemoveAll" && target.DeclaringType==typeof(List<WeakReference>)),
                "Passive observer IL references active collection/pruning: "+name);
        }
        stages["compiled-code-boundary"]=new {remove_all_in_load=removeAll,weak_is_alive=isAlive,
            visited_load_methods=visited.Count,scope="Direct Load/generated Load lambdas and passive snapshot methods; not arbitrary game constructors or all transitive framework code"};
    }
    static int Main(string[] args)
    {
        object? identities=null;string? error=null;var cleanupErrors=new List<string>();bool passed=false;
        try
        {
            Check(args.Length==2 && args[1] is "legacy" or "candidate",
                  "Supply the actual MemoryProbe.dll path and legacy|candidate.");
            mode=args[1];fixture=Path.GetFullPath(args[0]);Check(File.Exists(fixture),"Missing actual fixture.");
            identities=new {bridge=Identity(typeof(Entry).Assembly.Location),
                sdk=Identity(typeof(Game<>).Assembly.Location),fixture=Identity(fixture)};
            Console.Error.WriteLine("IDENTITIES "+JsonSerializer.Serialize(new {
                format="poima.managed-reload-capacity-inputs.v1",mode,identities}));
            LoadCodeBoundary();
            Check(Modules().Count==0 && RetiredCount()==0,"Fresh process registry required.");
            ulong observer=Load().GetProperty("handle").GetUInt64();
            Counts(Snapshot(observer,"initial"),1,0,0);
            HoldRealRetirements(observer);
            EstablishDeadHistory(observer);
            Counts(Snapshot(observer,"dead-capacity-before"),1,1024,0);
            Counts(Snapshot(observer,"dead-capacity-repeat"),1,1024,0);
            Check(collectCalls==0,"Operation5 destroyed decisive dead-history precondition.");
            if(mode=="legacy")RejectBudget("dead-capacity-rejection");
            else
            {
                var loaded=Load();ulong handle=loaded.GetProperty("handle").GetUInt64();
                stages["dead-capacity-admission"]=loaded;
                Counts(Snapshot(observer,"dead-capacity-recovered"),2,0,0);
                Initialize(handle);Release(handle);
            }
            decisiveProbeFinished=true;
            stages["decisive-probe"]=new {completed=true,operation5_before_dead_probe=collectCalls,
                accepted_loads=acceptedLoads,releases,test_host_gc_rounds=testHostGcRounds};
            if(mode=="candidate")ActiveCapacity(observer);
            passed=true;
        }
        catch(Exception failure) { error=failure.ToString(); }
        finally
        {
            // Teardown is explicitly active and occurs only after an original
            // outcome/failure is retained. It is not part of capacity recovery.
            try
            {
                if(Modules().Count>0)
                {
                    ++collectCalls;stages["teardown-before-release"]=JsonResult(Invoke(5));
                    foreach(ulong handle in Modules().Keys.Cast<ulong>().ToArray())Release(handle);
                    ++collectCalls;var final=JsonResult(Invoke(5));stages["teardown-final"]=final;
                    Check(final.GetProperty("active_modules").GetInt32()==0 &&
                          final.GetProperty("retired_alive").GetInt32()==0,"Fixture teardown retained contexts.");
                }
            }
            catch(Exception failure) { cleanupErrors.Add(failure.ToString()); }
        }
        passed=passed && error is null && cleanupErrors.Count==0;
        standardOutput.WriteLine(JsonSerializer.Serialize(new {format="poima.managed-reload-capacity.v1",mode,
            passed,checks,identities,error,cleanup_errors=cleanupErrors,rawcall_count=sequence,
            accepted_loads=acceptedLoads,releases,operation_counts=operationCounts,
            test_host_gc_rounds=testHostGcRounds,decisive_probe_finished=decisiveProbeFinished,
            stages,seconds=clock.Elapsed.TotalSeconds,
            scope="Actual compiled bridge bookkeeping-capacity fixture; controlled strong roots and test-host GC; not native hosting, production memory/performance or a general unloadability guarantee"}));
        standardOutput.Flush();
        return passed ? 0 : 1;
    }
}
