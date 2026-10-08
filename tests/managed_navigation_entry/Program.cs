// SPDX-License-Identifier: Apache-2.0
// Production NativeGame.Entry with an independent typed navigation-only Binding
// runs under CoreCLR here; published NativeAOT qualification is separate.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Poima.NativeGame;
[StructLayout(LayoutKind.Sequential)] unsafe struct EntryCall
{
    public uint Version,Operation;public ulong Handle;public byte* Text,State;public uint StateBytes,InputCount;
    public void* Services,Inputs;public ulong Tick;public byte* Output;public uint Capacity,Reserved;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct EntryServices
{
    public uint Version,Bytes;public void* Context;public fixed ulong Baseline[20];public fixed ulong Unrequested[5];
    public delegate* unmanaged[Cdecl]<void*,EntryRequest*,EntryPoint*,uint,EntryReply*,void*,int> Query;
}
[StructLayout(LayoutKind.Sequential)] struct EntryId { public ulong High,Low; }
[StructLayout(LayoutKind.Sequential)] struct EntryPoint { public float X,Y,Z; }
[StructLayout(LayoutKind.Sequential)] struct EntryRequest
{
    public uint Version,Bytes;public EntryId Agent;public EntryPoint Goal,Extents;public uint Polygons,Corners,Nodes,Reserved;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct EntryControlEvent
{
    public EntryId Element;public ulong Sequence;public uint ActionBytes,Reserved;public fixed byte Action[128];
}
[StructLayout(LayoutKind.Sequential)] unsafe struct EntryReply
{
    public uint Version,Bytes;public fixed ulong Asset[4];public uint Status,Flags,Count,Polygons;
    public EntryPoint Requested,Start,End,Reachable;public double StartDistance,EndDistance;public uint Reserved0,Reserved1;
}
static unsafe class NavigationEntryProbe
{
    internal static int Constructors,Initializes;
    static int queries;static bool bad;
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static string Text(byte* output)=>Marshal.PtrToStringUTF8((nint)output)!;
    static int Invoke(EntryCall* call){delegate* unmanaged[Cdecl]<void*,int,int> entry=&Entry.Invoke;return entry(call,sizeof(EntryCall));}
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Query(void* context,EntryRequest* request,EntryPoint* points,uint capacity,EntryReply* result,void* error)
    {
        ++queries;
        if(context!=(void*)0x1234 || request->Version!=1 || request->Bytes!=64 || request->Agent.High!=1 || request->Agent.Low!=2 ||
            request->Goal.X!=5 || request->Goal.Y!=0 || request->Goal.Z!=6 || request->Extents.X!=2 || request->Extents.Y!=4 || request->Extents.Z!=2 ||
            request->Polygons!=256 || request->Corners!=3 || request->Nodes!=4096 || request->Reserved!=0 || capacity!=3 || result->Version!=1 || result->Bytes!=128)bad=true;
        *result=new(){Version=1,Bytes=128,Flags=7,Count=1,Polygons=1,Requested=request->Goal,Start=request->Goal,End=request->Goal,Reachable=request->Goal};
        result->Asset[3]=123;points[0]=request->Goal;return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ControlInfo(void* context,EntryControlEvent* result,void* error)
    {
        if(context!=(void*)0x1234)bad=true;
        *result=new(){Element=new(){High=1,Low=3},Sequence=42,ActionBytes=4};
        result->Action[0]=(byte)'s';result->Action[1]=(byte)'a';result->Action[2]=(byte)'v';result->Action[3]=(byte)'e';return 0;
    }
    static JsonElement Host(uint bytes,bool feature=true)=>JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=7,services_bytes=bytes,
        features=feature?new[]{"baseline_v7","navigation_query_v1"}:new[]{"baseline_v7"}});
    static int Load(EntryCall* call,JsonElement? host)
    {
        var request=new Dictionary<string,object?>{{"type","StaticNavigationProbe"}};if(host.HasValue)request["host_contract"]=host.Value;
        var text=Encoding.UTF8.GetBytes(JsonSerializer.Serialize(request)+"\0");call->Operation=1;
        fixed(byte* ptr=text){call->Text=ptr;return Invoke(call);}
    }
    static void Empty(EntryCall* call){call->Operation=5;Check(Invoke(call)==0,Text(call->Output));using var doc=JsonDocument.Parse(Text(call->Output));Check(doc.RootElement.GetProperty("active_modules").GetInt32()==0,"Static native binding leaked a module.");}
    static int Main(string[] args)
    {
        var checks=new List<string>();byte* output=(byte*)NativeMemory.Alloc(65536);byte* state=(byte*)NativeMemory.Alloc(4);
        try{
            Check(sizeof(EntryCall)==80 && sizeof(EntryServices)==224 && sizeof(EntryRequest)==64 && sizeof(EntryReply)==128 && sizeof(EntryPoint)==12 &&
                Marshal.OffsetOf<EntryServices>(nameof(EntryServices.Query)).ToInt64()==216,"Independent static navigation ABI differs.");
            EntryCall call=new(){Version=1,Output=output,Capacity=65536,State=state,StateBytes=4};
            foreach(var host in new JsonElement?[]{null,Host(176),Host(192),Host(208),Host(216),Host(223),Host(224,false)}){
                Check(Load(&call,host)!=0 && Constructors==0 && Initializes==0,"Static binding constructed before host requirement validation.");Empty(&call);
            }
            checks.Add("static224 navigation Binding rejects absent/short/unnamed hosts before Create/Initialize and retains no module");
            Check(Load(&call,Host(224))==0,Text(output));using(var doc=JsonDocument.Parse(Text(output))){call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
                Check(doc.RootElement.GetProperty("requirements").GetProperty("services_bytes").GetUInt32()==224,"Static Binding manifest lost prefix.");}
            Check(Constructors==1 && Initializes==0,"Valid static binding did not construct exactly once.");call.Operation=2;Check(Invoke(&call)==0 && *(int*)state==10 && Initializes==1,Text(output));
            EntryServices services=new(){Version=7,Bytes=224,Context=(void*)0x1234,Query=&Query};for(int i=0;i<20;++i)services.Baseline[i]=1;
            services.Baseline[18]=(ulong)(nuint)(delegate* unmanaged[Cdecl]<void*,EntryControlEvent*,void*,int>)&ControlInfo;
            for(int i=0;i<5;++i)services.Unrequested[i]=0xa5a5a5a5a5a5a5a5;
            foreach(uint operation in new uint[]{3,6}){call.Operation=operation;
                foreach(int size in new[]{176,192,208,216}){using var guard=new GuardedHeader(size);new Span<byte>(guard.Header,size).Clear();
                    new ReadOnlySpan<byte>(&services,176).CopyTo(new Span<byte>(guard.Header,176));((EntryServices*)guard.Header)->Bytes=(uint)size;call.Services=guard.Header;
                    Check(Invoke(&call)!=0 && *(int*)state==10 && queries==0,"Static Binding read guarded navigation tail or executed game.");}
                var absent=services;absent.Query=null;call.Services=&absent;Check(Invoke(&call)!=0 && *(int*)state==10 && queries==0,"Static Binding accepted absent navigation callback.");
            }
            checks.Add("static Binding Tick/Control reject actual short guarded allocations and absent own callback before execution");
            call.Services=&services;call.Operation=3;Check(Invoke(&call)==0 && *(int*)state==11 && queries==1 && !bad,Text(output));
            call.Operation=6;Check(Invoke(&call)==0 && *(int*)state==12 && queries==1,Text(output));
            checks.Add("static navigation-only Binding transfers one-corner route with poisoned unrelated tails and preserves unused destination corners");
            call.Operation=4;Check(Invoke(&call)==0,Text(output));Empty(&call);
            checks.Add("static navigation-only Binding owner cleanup reports zero active modules");
            string report=JsonSerializer.Serialize(new{passed=true,checks=checks.ToArray(),constructors=Constructors,initializes=Initializes,queries});
            if(args.Length==1)File.WriteAllText(args[0],report);Console.Error.WriteLine(report);return 0;
        }finally{NativeMemory.Free(output);NativeMemory.Free(state);}
    }
}
