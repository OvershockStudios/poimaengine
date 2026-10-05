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
}
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
[GameModule("poima-test-services-v3")]
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
    }
}
static unsafe class Program
{
    static int gets, sets;
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
    static void Check(bool condition,string message) { if(!condition)throw new Exception(message); }
    static string Output(byte* output) => Marshal.PtrToStringUTF8((nint)output)!;
    static void Reject(Call* call,List<string> checks,string name,Services candidate,bool nullServices=false)
    {
        call->Services=nullServices ? null:&candidate;
        int before=((ProbeState*)call->State)->Count;
        Check(Entry.Invoke((nint)call,sizeof(Call))!=0,$"{name} unexpectedly accepted.");
        Check(Output(call->Output).Contains("Gameplay service"),$"{name}: wrong error {Output(call->Output)}");
        Check(((ProbeState*)call->State)->Count==before && gets==0 && sets==0,$"{name} invoked gameplay or callbacks.");
        checks.Add(name);
    }
    static int Main(string[] args)
    {
        var checks=new List<string>();
        try
        {
            Check(sizeof(Call)==80 && sizeof(Services)==64 && sizeof(Command)==48 && sizeof(Transition)==56 && sizeof(Animation)==120,"Independent ABI sizes.");
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
            Services good=new(){Version=3,Bytes=64,Context=(void*)0x1234,Entity=unused,Raycast=unused,Move=unused,Sound=unused,Get=&Get,Set=&Set};
            call.Operation=3;call.Tick=123;
            Reject(&call,checks,"null services rejected before Tick",good,true);
            foreach(var pair in new (uint Version,uint Bytes)[]{(2,48),(2,64),(3,48),(3,63),(3,65),(4,64),(3,0)})
            { var candidate=good;candidate.Version=pair.Version;candidate.Bytes=pair.Bytes;Reject(&call,checks,$"services {pair.Version}/{pair.Bytes} rejected before Tick",candidate); }
            var missing=good;missing.Get=null;Reject(&call,checks,"null animation get rejected",missing);
            missing=good;missing.Set=null;Reject(&call,checks,"null animation set rejected",missing);
            missing=good;missing.Sound=0;Reject(&call,checks,"null prefix callback rejected",missing);
            call.Services=&good;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            Check(((ProbeState*)state)->Count==8 && gets==3 && sets==1 && !badPayload,"Successful v3 invocation payload/state mismatch.");
            checks.Add("matching v3 transfers state, transition, nullable rest, frozen metadata and command correctly");
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
