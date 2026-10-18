// SPDX-License-Identifier: Apache-2.0
using System.Collections;
using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

// Independent actual 80-byte Entry wire; no mocked registry or capacity predicate.
[StructLayout(LayoutKind.Sequential)]
unsafe struct Call
{
    public uint Version,Operation;public ulong Handle;public byte* Text,State;
    public uint StateBytes,InputCount;public void* Services,Inputs;public ulong Tick;
    public byte* Output;public uint OutputCapacity,Reserved;
}

// Independent epoch-7 baseline. Pointer-only payloads are opaque native PODs;
// these callbacks must never be used by the state-only MemoryProbe game.
[StructLayout(LayoutKind.Explicit,Size=72)] struct Ray { }
[StructLayout(LayoutKind.Explicit,Size=88)] struct Hit { }
[StructLayout(LayoutKind.Explicit,Size=80)] struct Motion { }
[StructLayout(LayoutKind.Explicit,Size=32)] struct Sound { }
[StructLayout(LayoutKind.Explicit,Size=120)] struct AnimationState { }
[StructLayout(LayoutKind.Explicit,Size=48)] struct AnimationCommand { }
[StructLayout(LayoutKind.Explicit,Size=104)] struct SaveInfo { }
[StructLayout(LayoutKind.Explicit,Size=32)] struct SaveRequest { }
[StructLayout(LayoutKind.Explicit,Size=32)] struct SaveEnqueue { }
[StructLayout(LayoutKind.Explicit,Size=344)] struct SaveResult { }
[StructLayout(LayoutKind.Explicit,Size=80)] struct Transform { }
[StructLayout(LayoutKind.Explicit,Size=40)] struct UiState { }
[StructLayout(LayoutKind.Explicit,Size=40)] struct UiPatch { }
[StructLayout(LayoutKind.Explicit,Size=24)] struct UiModal { }
[StructLayout(LayoutKind.Explicit,Size=160)] struct UiEvent { }
[StructLayout(LayoutKind.Sequential)] unsafe struct Error { public fixed byte Text[2048]; }
[StructLayout(LayoutKind.Sequential)] unsafe struct Services
{
    public uint Version,Bytes;public void* Context;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,EntitySnapshot*,Error*,int> Entity;
    public delegate* unmanaged[Cdecl]<void*,Ray*,Hit*,Error*,int> Raycast;
    public delegate* unmanaged[Cdecl]<void*,Motion*,Error*,int> Move;
    public delegate* unmanaged[Cdecl]<void*,Sound*,ulong*,Error*,int> Sound;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,AnimationState*,Error*,int> AnimationGet;
    public delegate* unmanaged[Cdecl]<void*,AnimationCommand*,Error*,int> AnimationSet;
    public delegate* unmanaged[Cdecl]<void*,SaveInfo*,Error*,int> SaveInfo;
    public delegate* unmanaged[Cdecl]<void*,SaveRequest*,SaveEnqueue*,Error*,int> SaveRequest;
    public delegate* unmanaged[Cdecl]<void*,SaveTicket*,SaveResult*,Error*,int> SaveResult;
    public delegate* unmanaged[Cdecl]<void*,GameplayComponentDescriptor*,EntityId*,EntityId*,uint,uint*,Error*,int> ComponentQuery;
    public delegate* unmanaged[Cdecl]<void*,GameplayComponentDescriptor*,EntityId*,void*,uint,uint*,Error*,int> ComponentGet;
    public delegate* unmanaged[Cdecl]<void*,GameplayComponentDescriptor*,EntityId*,void*,uint,Error*,int> ComponentSet;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,uint*,Error*,int> EntityAlive;
    public delegate* unmanaged[Cdecl]<void*,TemplateId*,Transform*,EntityId*,Error*,int> Spawn;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,Error*,int> Despawn;
    public delegate* unmanaged[Cdecl]<void*,GameplayComponentDescriptor*,TemplateId*,void*,uint,uint*,Error*,int> TemplateComponentGet;
    public delegate* unmanaged[Cdecl]<void*,UiId*,UiState*,byte*,uint,uint*,Error*,int> UiGet;
    public delegate* unmanaged[Cdecl]<void*,UiPatch*,uint,UiModal*,Error*,int> UiEdit;
    public delegate* unmanaged[Cdecl]<void*,UiEvent*,Error*,int> ControlInfo;
    public delegate* unmanaged[Cdecl]<void*,uint,Error*,int> ControlRequest;
}

static unsafe class Program
{
    const string BoundsError="Managed memory context counts exceed their bounds.";
    const string MissingError="Memory snapshots were not requested for this live module.";
    const string BudgetError="Managed context budget reached; collect diagnostics or restart the session.";
    static string fixture="",mode="",outputDirectory="",fixtureHash="";
    static int checks,sequence,acceptedLoads,releases,collectCalls,unexpectedServiceCalls;
    static long rawBytes;
    static bool decisiveProbeFinished;
    static readonly Stopwatch clock=Stopwatch.StartNew();
    // Entry redirects Console.Out; retain the test's original stdout explicitly.
    static readonly TextWriter standardOutput=Console.Out;
    static readonly Dictionary<string,object> stages=[];
    static readonly Dictionary<string,int> operationCounts=[];
    static JsonElement referenceManifest;
    static void Check(bool value,string message) {++checks;if(!value)throw new InvalidOperationException(message);}
    static object Identity(string path)
    {
        using var stream=File.OpenRead(path);
        return new {path=Path.GetFullPath(path),bytes=stream.Length,sha256=Convert.ToHexStringLower(SHA256.HashData(stream))};
    }
    static object Identities() => new {bridge=Identity(typeof(Entry).Assembly.Location),
        sdk=Identity(typeof(Game<>).Assembly.Location),fixture=Identity(fixture),helper=Identity(Assembly.GetExecutingAssembly().Location)};
    static (int Code,string Text) Invoke(uint operation,ulong handle=0,string? request=null,byte[]? state=null,ulong tick=0,Services* services=null)
    {
        Check(clock.Elapsed.TotalSeconds<90,"Fixture90-second deadline reached.");
        byte[] buffer=new byte[8192],text=Encoding.UTF8.GetBytes((request ?? "")+"\0");
        string? before=state is null ? null : Convert.ToHexStringLower(state);
        fixed(byte* outPtr=buffer,textPtr=text,statePtr=state)
        {
            Call call=new() {Version=1,Operation=operation,Handle=handle,Text=textPtr,State=statePtr,
                StateBytes=(uint)(state?.Length ?? 0),Services=services,Tick=tick,Output=outPtr,OutputCapacity=(uint)buffer.Length};
            Check(sizeof(Call)==80,"Independent Entry call layout changed.");
            int code=Entry.Invoke((nint)(&call),sizeof(Call));
            int end=Array.IndexOf(buffer,(byte)0);
            string result=end<0 ? Convert.ToHexStringLower(buffer) : Encoding.UTF8.GetString(buffer,0,end);
            ++sequence;string key=operation.ToString(CultureInfo.InvariantCulture);
            operationCounts[key]=operationCounts.GetValueOrDefault(key)+1;
            string row="RAW_CALL "+JsonSerializer.Serialize(new {sequence,operation,handle,request,tick,code,
                output=result,terminated=end>=0,services_epoch=services==null ? null : (uint?)services->Version,
                services_bytes=services==null ? null : (uint?)services->Bytes,state_before_hex=before,
                state_after_hex=state is null ? null : Convert.ToHexStringLower(state)});
            rawBytes+=Encoding.UTF8.GetByteCount(row)+1;
            Check(rawBytes<=8*1024*1024,"Raw bridge call stream exceeds8MiB.");
            Console.Error.WriteLine(row);
            Check(end>=0,"Original Entry output lacks NUL terminator.");
            return (code,result);
        }
    }
    static int Unexpected(Error* error)
    {
        ++unexpectedServiceCalls;if(error!=null)error->Text[0]=0;return -1;
    }
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcEntity(void* context,EntityId* id,EntitySnapshot* output,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcRaycast(void* context,Ray* request,Hit* output,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcMove(void* context,Motion* value,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcSound(void* context,Sound* value,ulong* output,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcAnimationGet(void* context,EntityId* id,AnimationState* output,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcAnimationSet(void* context,AnimationCommand* value,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcSaveInfo(void* context,SaveInfo* output,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcSaveRequest(void* context,SaveRequest* request,SaveEnqueue* output,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcSaveResult(void* context,SaveTicket* ticket,SaveResult* output,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcComponentQuery(void* context,GameplayComponentDescriptor* descriptor,EntityId* after,EntityId* output,uint capacity,uint* written,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcComponentGet(void* context,GameplayComponentDescriptor* descriptor,EntityId* id,void* output,uint bytes,uint* present,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcComponentSet(void* context,GameplayComponentDescriptor* descriptor,EntityId* id,void* value,uint bytes,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcEntityAlive(void* context,EntityId* id,uint* alive,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcSpawn(void* context,TemplateId* template,Transform* transform,EntityId* output,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcDespawn(void* context,EntityId* id,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcTemplateComponentGet(void* context,GameplayComponentDescriptor* descriptor,TemplateId* template,void* output,uint bytes,uint* present,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcUiGet(void* context,UiId* id,UiState* output,byte* text,uint capacity,uint* written,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcUiEdit(void* context,UiPatch* patch,uint count,UiModal* modal,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcControlInfo(void* context,UiEvent* output,Error* error) => Unexpected(error);
    [UnmanagedCallersOnly(CallConvs=new[]{typeof(CallConvCdecl)})]
    static int SvcControlRequest(void* context,uint intent,Error* error) => Unexpected(error);
    static Services TickServices()
    {
        Services result=new() {Version=7,Bytes=176,
        Entity=&SvcEntity,
        Raycast=&SvcRaycast,
        Move=&SvcMove,
        Sound=&SvcSound,
        AnimationGet=&SvcAnimationGet,
        AnimationSet=&SvcAnimationSet,
        SaveInfo=&SvcSaveInfo,
        SaveRequest=&SvcSaveRequest,
        SaveResult=&SvcSaveResult,
        ComponentQuery=&SvcComponentQuery,
        ComponentGet=&SvcComponentGet,
        ComponentSet=&SvcComponentSet,
        EntityAlive=&SvcEntityAlive,
        Spawn=&SvcSpawn,
        Despawn=&SvcDespawn,
        TemplateComponentGet=&SvcTemplateComponentGet,
        UiGet=&SvcUiGet,
        UiEdit=&SvcUiEdit,
        ControlInfo=&SvcControlInfo,
        ControlRequest=&SvcControlRequest};
        Check(sizeof(Services)==176 && sizeof(EntitySnapshot)==160 && sizeof(GameplayComponentDescriptor)==56 &&
            sizeof(SaveTicket)==24 && sizeof(Error)==2048,"Independent baseline services layout differs.");
        Check((byte*)&result.Entity-(byte*)&result==16 && (byte*)&result.ControlRequest-(byte*)&result==168,
            "Independent baseline callback offsets differ.");return result;
    }
    static JsonElement JsonResult((int Code,string Text) result)
    {
        Check(result.Code==0,result.Text);
        using var document=JsonDocument.Parse(result.Text);return document.RootElement.Clone();
    }
    static IDictionary Modules() => (IDictionary)typeof(Entry).GetField("modules",BindingFlags.NonPublic|BindingFlags.Static)!.GetValue(null)!;
    static int RetiredCount() => ((ICollection)typeof(Entry).GetField("retired",BindingFlags.NonPublic|BindingFlags.Static)!.GetValue(null)!).Count;
    static ulong NextHandle() => (ulong)typeof(Entry).GetField("next",BindingFlags.NonPublic|BindingFlags.Static)!.GetValue(null)!;
    static (int Active,int Retired,ulong Next) Registry() => (Modules().Count,RetiredCount(),NextHandle());
    static string Request() => JsonSerializer.Serialize(new {assembly=fixture,type="Poima.Qualification.MemoryProbe",memory_snapshot_version=1});
    static string WithoutHandle(JsonElement value) => JsonSerializer.Serialize(value.EnumerateObject()
        .Where(p=>p.Name!="handle").ToDictionary(p=>p.Name,p=>p.Value.Clone()));
    static ulong Load()
    {
        var manifest=JsonResult(Invoke(1,request:Request()));++acceptedLoads;
        ulong handle=manifest.GetProperty("handle").GetUInt64();
        Check(handle>0 && manifest.GetProperty("identity").GetString()=="poima.qualification.memory-snapshot" &&
              manifest.GetProperty("bytes").GetRawText()=="12" && manifest.GetProperty("assembly_sha256").GetString()==fixtureHash &&
              manifest.GetProperty("memory_snapshot_version").GetRawText()=="1","Actual MemoryProbe manifest/identity differs.");
        if(referenceManifest.ValueKind==JsonValueKind.Undefined)referenceManifest=manifest;
        else Check(WithoutHandle(manifest)==WithoutHandle(referenceManifest),"Actual complete load manifest changed beyond handle.");
        return handle;
    }
    static void Release(ulong handle)
    {
        var result=Invoke(4,handle);Check(result.Code==0 && result.Text=="",result.Text);++releases;
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    static AssemblyLoadContext ActualContext(ulong handle)
    {
        object module=Modules()[handle]!;
        var context=(AssemblyLoadContext)module.GetType().GetProperty("Context")!.GetValue(module)!;
        Check(context.IsCollectible,"Loaded context is not collectible.");return context;
    }
    static void Fields(JsonElement value,params string[] fields) =>
        Check(value.ValueKind==JsonValueKind.Object && value.EnumerateObject().Select(p=>p.Name).Order().SequenceEqual(fields.Order()),"Closed observer field membership differs.");
    static int Integer(JsonElement value,int minimum,int maximum)
    {
        Check(value.ValueKind==JsonValueKind.Number && value.TryGetInt32(out _),"Exact JSON integer required.");
        int result=value.GetInt32();
        Check(result>=minimum && result<=maximum && value.GetRawText()==result.ToString(CultureInfo.InvariantCulture),"Integer bounds/lexeme differ.");return result;
    }
    static ulong Decimal(JsonElement value,ulong maximum=long.MaxValue)
    {
        Check(value.ValueKind==JsonValueKind.String,"Counter must remain a string.");string text=value.GetString()!;
        Check(ulong.TryParse(text,NumberStyles.None,CultureInfo.InvariantCulture,out ulong result) && result<=maximum &&
            result.ToString(CultureInfo.InvariantCulture)==text,"Canonical unsigned source-bounded counter required.");return result;
    }
    static void ValidateSnapshot(JsonElement value,int active,int retired,long start,long end)
    {
        Fields(value,"format","version","process_id","timestamp_start","timestamp_end","timestamp_frequency",
            "active_modules","retired_contexts_tracked","retired_contexts_alive","gc","last_gc");
        Check(value.GetProperty("format").GetString()=="poima.managed-memory-snapshot.v1" &&
            Integer(value.GetProperty("version"),1,1)==1 && Integer(value.GetProperty("process_id"),1,int.MaxValue)==Environment.ProcessId,"Observer identity differs.");
        Check(Integer(value.GetProperty("active_modules"),0,32)==active &&
            Integer(value.GetProperty("retired_contexts_tracked"),0,mode=="legacy" ? 1024 : 1055)==retired &&
            Integer(value.GetProperty("retired_contexts_alive"),0,retired)==retired,"Exact actual active/tracked/alive counts differ.");
        ulong first=Decimal(value.GetProperty("timestamp_start")),last=Decimal(value.GetProperty("timestamp_end"));
        Check(first>=(ulong)start && last>=first && last<=(ulong)end &&
            Decimal(value.GetProperty("timestamp_frequency"))==(ulong)Stopwatch.Frequency,"Observer clock bracket differs.");
        var gc=value.GetProperty("gc");Fields(gc,"total_memory_bytes","total_allocated_bytes","gen0_collections","gen1_collections","gen2_collections");
        _=Decimal(gc.GetProperty("total_memory_bytes"));_=Decimal(gc.GetProperty("total_allocated_bytes"));
        foreach(string key in new[]{"gen0_collections","gen1_collections","gen2_collections"})_=Decimal(gc.GetProperty(key),int.MaxValue);
        var history=value.GetProperty("last_gc");Fields(history,"index","generation","concurrent","compacted","heap_size_bytes","fragmented_bytes",
            "total_committed_bytes","memory_load_bytes","high_memory_load_threshold_bytes","total_available_memory_bytes");
        _=Decimal(history.GetProperty("index"));_=Integer(history.GetProperty("generation"),0,2);
        foreach(string key in new[]{"concurrent","compacted"})Check(history.GetProperty(key).ValueKind is JsonValueKind.True or JsonValueKind.False,"GC flag type differs.");
        foreach(string key in new[]{"heap_size_bytes","fragmented_bytes","total_committed_bytes","memory_load_bytes","high_memory_load_threshold_bytes","total_available_memory_bytes"})_=Decimal(history.GetProperty(key));
        // GC history and current limits are raw/non-atomic; no invented cross-counter or all-zero assumptions.
    }
    static void Probe(ulong observer,string label,int active,int retired,byte[] state)
    {
        var registry=Registry();string before=Convert.ToHexStringLower(state);long start=Stopwatch.GetTimestamp();
        var result=Invoke(8,observer);long end=Stopwatch.GetTimestamp();
        Check(Registry()==registry && registry.Active==active && registry.Retired==retired &&
            Convert.ToHexStringLower(state)==before,"Passive probe changed actual registry or complete gameplay state.");
        bool refused=mode=="legacy" && retired>1024;
        if(refused)Check(result.Code==-1 && result.Text==BoundsError,"Expected exact original observer bounds refusal.");
        else
        {
            var snapshot=JsonResult(result);ValidateSnapshot(snapshot,active,retired,start,end);
            byte[] literal=Encoding.UTF8.GetBytes(result.Text);
            using var file=new FileStream(Path.Combine(outputDirectory,label+".json"),FileMode.CreateNew,FileAccess.Write,FileShare.Read);
            file.Write(literal);file.Flush(true);
        }
        stages[label]=new {active,tracked=retired,alive=retired,code=result.Code,text=result.Text,
            raw_file=refused ? null : label+".json",state_hex=before,registry_next=registry.Next};
    }
    static byte[] Initialized(ulong observer)
    {
        byte[] state=new byte[12];var result=Invoke(2,observer,state:state);
        byte[] expected=new byte[12];BitConverter.GetBytes(17).CopyTo(expected,8);
        Check(result.Code==0 && result.Text=="" && state.SequenceEqual(expected),"Complete Initialize state differs.");
        stages["initialize"]=new {code=result.Code,text=result.Text,state_hex=Convert.ToHexStringLower(state)};return state;
    }
    [MethodImpl(MethodImplOptions.NoInlining)]
    static void RealBoundary()
    {
        var roots=new List<AssemblyLoadContext>(1055);var distinct=new HashSet<AssemblyLoadContext>(ReferenceEqualityComparer.Instance);
        var handles=new HashSet<ulong>();var peers=new List<ulong>();
        ulong Add()
        {
            ulong handle=Load();Check(handles.Add(handle),"Actual handle reused.");var context=ActualContext(handle);
            Check(distinct.Add(context),"Actual collectible context reused.");roots.Add(context);return handle;
        }
        ulong observer=Add();byte[] state=Initialized(observer);
        for(int i=0;i<30;++i)peers.Add(Add());
        Check(Modules().Count==31 && RetiredCount()==0,"Initial31 actual modules differ.");
        for(int i=0;i<1023;++i)
        {
            ulong temporary=Add();Check(Modules().Count==32,"Temporary real load did not fill32nd slot.");Release(temporary);
            Check(Modules().Count==31 && RetiredCount()==i+1,"Actual retirement sequence differs.");
        }
        peers.Add(Add());Check(Modules().Count==32 && RetiredCount()==1023,"Final accepted32nd peer differs.");
        var beforeReject=Registry();var rejected=Invoke(1,request:Request());
        Check(rejected.Code==-1 && rejected.Text==BudgetError && Registry()==beforeReject,"Active33 guard or rejected-load atomicity changed.");
        stages["active33-rejected"]=new {code=rejected.Code,text=rejected.Text,active=32,tracked=1023};
        Probe(observer,"retired-1023",32,1023,state);
        for(int i=0;i<peers.Count;++i)
        {
            Release(peers[i]);int retired=1024+i,active=31-i;
            Check(Modules().Count==active && RetiredCount()==retired,"Peer release did not transfer exact lifetime membership.");
            if(retired is 1024 or 1025 or 1054)
            {
                Probe(observer,"retired-"+retired,active,retired,state);
                Probe(observer,"retired-"+retired+"-repeat",active,retired,state);
            }
        }
        Check(acceptedLoads==1055 && releases==1054 && roots.Count==1055 && distinct.Count==1055 && handles.Count==1055,"Real context cohort counts differ.");
        Services services=TickServices();
        string beforeTick=Convert.ToHexStringLower(state);var tick=Invoke(3,observer,state:state,tick:1,services:&services);
        byte[] expected=new byte[12];BitConverter.GetBytes(1).CopyTo(expected,0);BitConverter.GetBytes(23).CopyTo(expected,8);
        Check(tick.Code==0 && tick.Text=="" && state.SequenceEqual(expected) && unexpectedServiceCalls==0,"Surviving module complete Tick state differs.");
        stages["tick-after-boundary"]=new {code=tick.Code,text=tick.Text,before_hex=beforeTick,after_hex=Convert.ToHexStringLower(state),
            services_epoch=services.Version,services_bytes=services.Bytes,unexpected_service_calls=unexpectedServiceCalls};
        Release(observer);Check(Modules().Count==0 && RetiredCount()==1055,"Global final retirement1055 differs.");
        var expiredBefore=Registry();var expired=Invoke(8,observer);
        Check(expired.Code==-1 && expired.Text==MissingError && Registry()==expiredBefore,"Released observer did not preserve exact live-handle refusal.");
        Check(collectCalls==0 && !operationCounts.ContainsKey("5"),"Active collection entered decisive lifetime probes.");
        stages["global1055-expired-handle"]=new {active=0,tracked=1055,code=expired.Code,text=expired.Text};
        stages["real-contexts"]=new {accepted_loads=acceptedLoads,releases,distinct_contexts=distinct.Count,
            actual_handles=handles.Count,controlled_root_count=roots.Count,operation5_during_decisive=collectCalls};
        GC.KeepAlive(roots);GC.KeepAlive(distinct);decisiveProbeFinished=true;
    }
    static int Main(string[] args)
    {
        string? error=null,inputIdentity=null;object? identities=null;bool passed=false;var cleanupErrors=new List<string>();
        try
        {
            Check(args.Length==3 && args[1] is "legacy" or "candidate","Usage: MemoryProbe.dll legacy|candidate FRESH_OUTPUT_DIRECTORY");
            fixture=Path.GetFullPath(args[0]);mode=args[1];outputDirectory=Path.GetFullPath(args[2]);
            Check(File.Exists(fixture) && !File.Exists(outputDirectory) && !Directory.Exists(outputDirectory) &&
                Directory.Exists(Path.GetDirectoryName(outputDirectory)),"Existing input and fresh output with existing parent required.");
            identities=Identities();inputIdentity=JsonSerializer.Serialize(identities);
            using(var file=File.OpenRead(fixture))fixtureHash=Convert.ToHexStringLower(SHA256.HashData(file));
            Console.Error.WriteLine("IDENTITIES "+JsonSerializer.Serialize(new {format="poima.managed-retirement-bounds-inputs.v1",mode,identities}));
            Check(Modules().Count==0 && RetiredCount()==0 && NextHandle()==1,"Fresh actual bridge process required.");
            Directory.CreateDirectory(outputDirectory);RealBoundary();passed=true;
        }
        catch(Exception failure) {error=failure.ToString();}
        finally
        {
            // The decisive no-inline root frame has returned/unwound. This is
            // explicitly active teardown, never part of passive observation.
            try
            {
                foreach(ulong handle in Modules().Keys.Cast<ulong>().ToArray())Release(handle);
                ++collectCalls;var final=JsonResult(Invoke(5));
                stages["explicit-teardown-after-root-frame"]=final;
                int remainingAlive=final.GetProperty("retired_alive").GetInt32();
                Check(final.GetProperty("active_modules").GetInt32()==0 && remainingAlive is >=0 and <=1055,
                    "Explicit teardown returned invalid module/context accounting.");
                // Raw retained-alive results are not a requirement to force
                // unloading; the decisive observer-bound proof is separate.
                if(inputIdentity is not null)Check(JsonSerializer.Serialize(Identities())==inputIdentity,"Selected input bytes changed.");
            }
            catch(Exception failure) {cleanupErrors.Add(failure.ToString());}
        }
        passed=passed && decisiveProbeFinished && error is null && cleanupErrors.Count==0 && clock.Elapsed.TotalSeconds<90;
        string receipt=JsonSerializer.Serialize(new {format="poima.managed-retirement-bounds.v1",mode,passed,checks,identities,error,
            cleanup_errors=cleanupErrors,rawcall_count=sequence,raw_call_bytes=rawBytes,accepted_loads=acceptedLoads,releases,
            operation_counts=operationCounts,decisive_probe_finished=decisiveProbeFinished,collect_calls_total=collectCalls,
            stages,seconds=clock.Elapsed.TotalSeconds,
            scope="Real compiled Entry lifetime-bound fixture with controlled strong roots; not native world hosting, a leak diagnosis or production memory/performance"});
        if(Encoding.UTF8.GetByteCount(receipt)>64*1024)throw new InvalidOperationException("Terminal receipt exceeds64KiB.");
        standardOutput.WriteLine(receipt);standardOutput.Flush();
        return passed && clock.Elapsed.TotalSeconds<90 ? 0 : 1;
    }
}
