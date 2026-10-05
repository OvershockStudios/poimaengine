// SPDX-License-Identifier: Apache-2.0
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

// Independent ABI declarations: do not link production internal POD types.
[StructLayout(LayoutKind.Sequential)] unsafe struct Call
{
    public uint Version, Operation; public ulong Handle; public byte* Text, State;
    public uint StateBytes, InputCount; public Services* Services; public void* Inputs;
    public ulong Tick; public byte* Output; public uint OutputCapacity, Reserved;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct Services
{
    public uint Version, Bytes; public void* Context;
    public nint Entity, Raycast, Move, Sound;
    public delegate* unmanaged[Cdecl]<void*, Id*, Animation*, void*, int> Get;
    public delegate* unmanaged[Cdecl]<void*, Command*, void*, int> Set;
    public delegate* unmanaged[Cdecl]<void*, SaveInfo*, void*, int> SaveInfo;
    public delegate* unmanaged[Cdecl]<void*, SaveRequest*, SaveEnqueue*, void*, int> SaveRequest;
    public delegate* unmanaged[Cdecl]<void*, Ticket*, SaveResult*, void*, int> SaveResult;
}
[StructLayout(LayoutKind.Sequential)] struct Ticket { public ulong High,Low,Sequence; }
[StructLayout(LayoutKind.Sequential)] unsafe struct SaveRequest { public uint Kind,SlotBytes;public byte* Slot;public ulong Expected;public uint HasExpected,Recovery; }
[StructLayout(LayoutKind.Sequential)] struct SaveEnqueue { public Ticket Ticket;public uint Rejection,Reserved; }
[StructLayout(LayoutKind.Sequential)] unsafe struct SaveResult
{ public Ticket Ticket;public uint Kind,State;public ulong Requested,Committed,Generation;public uint Recovered;public int Error;public ulong High,Low,Tick;public fixed byte Diagnostic[256]; }
[StructLayout(LayoutKind.Sequential)] struct SaveInfo
{ public ulong High,Low,Configuration;public uint Enabled,RestorePresent;public Ticket Ticket;public ulong DestinationHigh,DestinationLow,SourceTick,RestoredTick,Generation;public uint Recovered,Reserved; }
[StructLayout(LayoutKind.Sequential)] struct Id { public ulong High, Low; }
[StructLayout(LayoutKind.Sequential)] struct Command
{ public Id Entity; public double Time, Speed; public int Clip; public uint Loop, Playing, BlendTicks; }
[StructLayout(LayoutKind.Sequential)] struct Transition
{
    public ulong Start; public double Weight, Time, Speed; public uint Duration, Elapsed;
    public int Clip; public uint Frozen, Loop, Playing;
}
[StructLayout(LayoutKind.Sequential)] struct Animation
{
    public Id Entity; public double Time, Speed, Duration; public int Clip;
    public uint Present, Loop, Playing, TransitionPresent, Reserved; public Transition Transition;
}
[StructLayout(LayoutKind.Sequential)] public struct ProbeState { public int Count; }
[GameModule("poima-test-services-v4")]
public sealed class ProbeGame : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state) { state.Count=7; }
    public override void Tick(ref ProbeState state, GameContext context)
    {
        ++state.Count;
        if(context.Tick!=123)throw new Exception("Tick ABI mismatch.");
        var a=context.GetAnimation(new(11,22)) ?? throw new Exception("Missing rig.");
        if(a.Entity!=new EntityId(11,22) || a.Clip!=3 || a.Time!=.125 || a.Speed!=2 || a.Duration!=4 || !a.Loop || a.Playing)
            throw new Exception("Animation state ABI mismatch.");
        var t=a.Transition ?? throw new Exception("Missing transition.");
        if(t.StartTick!=99 || t.DurationTicks!=32 || t.ElapsedTicks!=24 || t.Weight!=.75 || t.SourceFrozen ||
            t.SourceClip!=2 || t.SourceTime!=.5 || t.SourceSpeed!=.25 || t.SourceLoop!=false || t.SourcePlaying!=true)
            throw new Exception("Transition ABI mismatch.");
        var frozen=context.GetAnimation(new(11,23))!.Value.Transition!.Value;
        if(!frozen.SourceFrozen || frozen.SourceClip!=null || frozen.SourceTime!=null || frozen.SourceSpeed!=null || frozen.SourceLoop!=null || frozen.SourcePlaying!=null)
            throw new Exception("Frozen source must hide clock metadata.");
        if(context.GetAnimation(new(11,24))!=null)throw new Exception("Non-rig must be null.");
        context.SetAnimation(new(11,22),null,.375,1.5,false,true,17);
        var saves=context.Saves;
        if(!saves.Enabled || saves.Epoch!=new SaveEpoch(-1,22) || saves.ConfigurationGeneration!=19 || saves.LastRestore is not {} restore ||
            restore.InitiatingTicket!=new SaveTicket(long.MinValue,21,6) || restore.DestinationEpoch!=new SaveEpoch(-1,22) ||
            restore.CommittedSourceTick!=80 || restore.RestoredTick!=14 || restore.Generation!=3 || !restore.Recovered)
            throw new Exception("Save capability/restore ABI mismatch.");
        var complete=context.GetSaveResult(new(-1,22,7));
        if(complete.Ticket!=new SaveTicket(-1,22,7) || complete.Kind!=SaveKind.Load || complete.State!=SaveOperationState.Succeeded ||
            complete.RequestedTick!=22 || complete.CommittedTick!=27 || complete.Generation!=5 || !complete.Recovered || complete.ErrorCode!=0 ||
            complete.RestoredEpoch!=new SaveEpoch(long.MinValue,33) || complete.RestoredTick!=12 || complete.Diagnostic!="restored" || !complete.IsTerminal)
            throw new Exception("Save result ABI mismatch.");
        var uncertain=context.GetSaveResult(new(-1,22,8));
        if(uncertain.State!=SaveOperationState.Resolving || uncertain.ErrorCode!=-32070 || uncertain.Diagnostic!="uncertain" || uncertain.IsTerminal || uncertain.RestoredEpoch!=null || uncertain.RestoredTick!=null)
            throw new Exception("Uncertain save became terminal.");
        if(context.GetSaveResult(default).State!=SaveOperationState.Expired)throw new Exception("Unknown save token did not expire.");
        if(context.RequestSave("quick",42)!=new SaveTicket(-1,22,101))throw new Exception("Save enqueue mismatch.");
        var queued=context.TryRequestLoad("chapter-1",null,true);
        if(!queued.Accepted || queued.Ticket!=new SaveTicket(-1,22,102))throw new Exception("Load enqueue mismatch.");
        if(context.TryRequestSave("bad/path").Rejection!=SaveRequestRejection.Invalid || context.TryRequestLoad("quick",-1).Accepted)
            throw new Exception("Invalid request escaped SDK bounds.");
        bool rejected=false;
        try { context.RequestLoad("busy"); }catch(SaveRequestException error) { rejected=error.Rejection==SaveRequestRejection.Busy; }
        if(!rejected)throw new Exception("Rejected request lost its typed reason.");
    }
}
static unsafe class Program
{
    static int gets, sets,infos,requests,results;
    static bool badPayload;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Unused() => -1;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Get(void* context, Id* id, Animation* output, void* error)
    {
        ++gets; if((nint)context!=0x1234 || id->High!=11)badPayload=true;
        *output=new Animation {Entity=*id,Present=id->Low==24 ? 0u:1u,Clip=3,Time=.125,Speed=2,Duration=4,Loop=1,Playing=0,TransitionPresent=1,
            Transition=new Transition {Start=99,Duration=32,Elapsed=24,Weight=.75,Clip=2,Time=.5,Speed=.25,Frozen=id->Low==23 ? 1u:0u,Loop=0,Playing=1}};
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Set(void* context, Command* value, void* error)
    {
        ++sets;
        if((nint)context!=0x1234 || value->Entity.High!=11 || value->Entity.Low!=22 || value->Clip!=-1 ||
            value->Time!=.375 || value->Speed!=1.5 || value->Loop!=0 || value->Playing!=1 || value->BlendTicks!=17)badPayload=true;
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Info(void* context,SaveInfo* output,void* error)
    {
        ++infos;if((nint)context!=0x1234)badPayload=true;
        *output=new(){High=ulong.MaxValue,Low=22,Configuration=19,Enabled=1,RestorePresent=1,
            Ticket=new(){High=1UL<<63,Low=21,Sequence=6},DestinationHigh=ulong.MaxValue,DestinationLow=22,SourceTick=80,RestoredTick=14,Generation=3,Recovered=1};
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Request(void* context,SaveRequest* request,SaveEnqueue* output,void* error)
    {
        ++requests;if((nint)context!=0x1234 || request->SlotBytes>64) {badPayload=true;return -1;}
        string slot=Encoding.UTF8.GetString(new ReadOnlySpan<byte>(request->Slot,(int)request->SlotBytes));
        if(slot=="quick") {
            if(request->Kind!=1 || request->Expected!=42 || request->HasExpected!=1 || request->Recovery!=0)badPayload=true;
            *output=new(){Ticket=new(){High=ulong.MaxValue,Low=22,Sequence=101}};
        } else if(slot=="chapter-1") {
            if(request->Kind!=2 || request->Expected!=0 || request->HasExpected!=0 || request->Recovery!=1)badPayload=true;
            *output=new(){Ticket=new(){High=ulong.MaxValue,Low=22,Sequence=102}};
        } else if(slot=="busy")*output=new(){Rejection=2};
        else {badPayload=true;return -1;}
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Result(void* context,Ticket* ticket,SaveResult* output,void* error)
    {
        ++results;if((nint)context!=0x1234)badPayload=true;
        *output=new(){Ticket=*ticket};
        if(ticket->Sequence==0)return 0;
        if(ticket->High!=ulong.MaxValue || ticket->Low!=22)badPayload=true;
        if(ticket->Sequence==7) {
            output->Kind=2;output->State=3;output->Requested=22;output->Committed=27;output->Generation=5;output->Recovered=1;output->High=1UL<<63;output->Low=33;output->Tick=12;
            Encoding.UTF8.GetBytes("restored",new Span<byte>(output->Diagnostic,256));
        }else if(ticket->Sequence==8) {
            output->Kind=1;output->State=2;output->Error=-32070;
            Encoding.UTF8.GetBytes("uncertain",new Span<byte>(output->Diagnostic,256));
        }else badPayload=true;
        return 0;
    }
    static void Check(bool condition,string message) { if(!condition)throw new Exception(message); }
    static string Output(byte* output) => Marshal.PtrToStringUTF8((nint)output)!;
    static void Reject(Call* call,List<string> checks,string name,Services candidate,bool nullServices=false)
    {
        call->Services=nullServices ? null:&candidate;
        int before=((ProbeState*)call->State)->Count;
        Check(Entry.Invoke((nint)call,sizeof(Call))!=0,$"{name} unexpectedly accepted.");
        Check(Output(call->Output).Contains("Gameplay service"),$"{name}: wrong error {Output(call->Output)}");
        Check(((ProbeState*)call->State)->Count==before && gets==0 && sets==0 && infos==0 && requests==0 && results==0,$"{name} invoked gameplay or callbacks.");
        checks.Add(name);
    }
    static int Main(string[] args)
    {
        var checks=new List<string>();
        try
        {
            Check(sizeof(Call)==80 && sizeof(Services)==88 && sizeof(Command)==48 && sizeof(Transition)==56 && sizeof(Animation)==120 &&
                sizeof(Ticket)==24 && sizeof(SaveRequest)==32 && sizeof(SaveEnqueue)==32 && sizeof(SaveResult)==344 && sizeof(SaveInfo)==104,"Independent ABI sizes.");
            byte* output=stackalloc byte[65536]; byte* state=stackalloc byte[sizeof(ProbeState)];
            var request=Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new {assembly=Assembly.GetExecutingAssembly().Location,type=typeof(ProbeGame).FullName})+"\0");
            Call call=new(){Version=1,Operation=1,Output=output,OutputCapacity=65536};
            fixed(byte* text=request)
            {
                call.Text=text; Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            }
            using var manifest=JsonDocument.Parse(Output(output)); call.Handle=manifest.RootElement.GetProperty("handle").GetUInt64();
            call.State=state;call.StateBytes=(uint)sizeof(ProbeState);call.Operation=2;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            Check(((ProbeState*)state)->Count==7,"Initialize state.");
            nint unused=(nint)(delegate* unmanaged[Cdecl]<int>)&Unused;
            Services good=new(){Version=4,Bytes=88,Context=(void*)0x1234,Entity=unused,Raycast=unused,Move=unused,Sound=unused,Get=&Get,Set=&Set,SaveInfo=&Info,SaveRequest=&Request,SaveResult=&Result};
            call.Operation=3;call.Tick=123;
            Reject(&call,checks,"null services rejected before Tick",good,true);
            foreach(var pair in new (uint Version,uint Bytes)[]{(2,48),(3,64),(3,88),(4,64),(4,87),(4,89),(5,88),(4,0)})
            { var candidate=good;candidate.Version=pair.Version;candidate.Bytes=pair.Bytes;Reject(&call,checks,$"services {pair.Version}/{pair.Bytes} rejected before Tick",candidate); }
            var missing=good;missing.Get=null;Reject(&call,checks,"null animation get rejected",missing);
            missing=good;missing.Set=null;Reject(&call,checks,"null animation set rejected",missing);
            missing=good;missing.Sound=0;Reject(&call,checks,"null prefix callback rejected",missing);
            missing=good;missing.SaveInfo=null;Reject(&call,checks,"null save info rejected",missing);
            missing=good;missing.SaveRequest=null;Reject(&call,checks,"null save request rejected",missing);
            missing=good;missing.SaveResult=null;Reject(&call,checks,"null save result rejected",missing);
            call.Services=&good;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            Check(((ProbeState*)state)->Count==8 && gets==3 && sets==1 && infos==1 && requests==3 && results==3 && !badPayload,"Successful v4 invocation payload/state mismatch.");
            checks.Add("matching v4 transfers animation, typed save requests/results, uncertainty, restore metadata and rejection correctly");
            call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            call.Operation=5;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            using var collected=JsonDocument.Parse(Output(output));
            Check(collected.RootElement.GetProperty("active_modules").GetInt32()==0 && collected.RootElement.GetProperty("retired_alive").GetInt32()==0,"Fixture context retained after release.");
            checks.Add("animation fixture collectible context released");
            var evidence=JsonSerializer.Serialize(new {passed=true,checks,platform=RuntimeInformation.OSDescription,
                bridge_sha256=Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(typeof(Entry).Assembly.Location))),
                sdk_sha256=Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(typeof(Game<>).Assembly.Location)))},new JsonSerializerOptions{WriteIndented=true});
            if(args.Length==1)File.WriteAllText(args[0],evidence+"\n");Console.WriteLine(evidence);return 0;
        }
        catch(Exception error) { Console.Error.WriteLine(error);return 1; }
    }
}
