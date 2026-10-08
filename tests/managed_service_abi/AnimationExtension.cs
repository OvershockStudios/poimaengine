// SPDX-License-Identifier: Apache-2.0
// Independent declarations and mock callbacks qualify SDK/bridge transfer.
// They do not exercise Runtime::Impl callbacks or NativeAOT publication.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)] struct ExtendedCommand
{ public uint Version,Bytes;public Command Command;public uint Mode,Reserved; }
[StructLayout(LayoutKind.Sequential)] struct ExtendedAnimation
{ public uint Version,Bytes;public Animation State;public uint Mode,Reserved; }
[StructLayout(LayoutKind.Sequential)] unsafe struct AnimationServices
{
    public Services Baseline;
    public delegate* unmanaged[Cdecl]<void*,Id*,ExtendedAnimation*,void*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,ExtendedCommand*,void*,int> Set;
}

[GameModule("poima-test-inertial-services")]
public sealed class InertialProbeGame : Game<ProbeState>,IInertialAnimationGame
{
    // Compilation alone checks zero/default/named legacy overload resolution.
    // These calls are not executed against the narrowly prescribed mock data.
    private static void CompileLegacyCalls(GameContext context)
    {
        context.SetAnimation(new(11,22),null,0);
        context.SetAnimation(new(11,22),null,default);
        context.SetAnimation(new(11,22),null,time:0);
        context.SetAnimation(new(11,22),null,transitionMode:AnimationTransitionMode.Inertial);
    }
    public override void Initialize(ref ProbeState state) {state.Count=20;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        var inertial=context.GetAnimationExtended(new(11,31)) ?? throw new Exception("Missing extended rig.");
        if(inertial.Mode!=AnimationTransitionMode.Inertial || inertial.Progress!=.75 ||
            inertial.State.Clip!=3 || inertial.State.Time!=.125 || inertial.State.Speed!=2 ||
            inertial.State.Duration!=4 || !inertial.State.Loop || inertial.State.Playing ||
            inertial.State.Transition is not {} transition || transition.StartTick!=99 ||
            transition.DurationTicks!=32 || transition.ElapsedTicks!=24 || !transition.SourceFrozen ||
            transition.SourceClip!=null || transition.SourceTime!=null)
            throw new Exception("Inertial extension payload mismatch.");
        var crossfade=context.GetAnimationExtended(new(11,32)) ?? throw new Exception("Missing crossfade rig.");
        if(crossfade.Mode!=AnimationTransitionMode.Crossfade || crossfade.Progress!=.75 ||
            crossfade.State.Transition is not {} source || source.SourceFrozen || source.SourceClip!=2 ||
            source.SourceTime!=.5 || source.SourceSpeed!=.25 || source.SourceLoop!=false || source.SourcePlaying!=true)
            throw new Exception("Crossfade extension payload mismatch.");
        var steady=context.GetAnimationExtended(new(11,33)) ?? throw new Exception("Missing steady rig.");
        if(steady.Mode!=null || steady.Progress!=null || steady.State.Transition!=null)
            throw new Exception("Inactive transition exposed a mode.");
        if(context.GetAnimationExtended(new(11,34))!=null)throw new Exception("Missing rig must remain null.");
        var legacy=context.GetAnimation(new(11,22)) ?? throw new Exception("Legacy getter missing.");
        if(legacy.Clip!=3 || legacy.Time!=.125)throw new Exception("Legacy getter changed.");
        // Exact old seven-argument call and new enum-third overload both compile.
        context.SetAnimation(new(11,22),null,.375,1.5,false,true,17);
        context.SetAnimation(new(11,22),3,AnimationTransitionMode.Crossfade,.5,.75,false,true,12);
        context.SetAnimation(new(11,23),null,AnimationTransitionMode.Inertial,.125,1.5,true,false,9);
        bool invalid=false;
        try {context.SetAnimation(new(11,23),null,(AnimationTransitionMode)2);}
        catch(ArgumentOutOfRangeException) {invalid=true;}
        if(!invalid)throw new Exception("Invalid public transition mode accepted.");
        ++state.Count;
    }
    public override void Control(ref ProbeState state,ControlContext context) {++state.Count;}
}

[GameModule("poima-test-inertial-constructor-gate")]
public sealed class InertialConstructorProbe : Game<ProbeState>,IInertialAnimationGame
{
    public InertialConstructorProbe() {throw new Exception("MARKED CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state) {throw new Exception("MARKED INITIALIZE REACHED");}
    public override void Tick(ref ProbeState state,GameContext context) {throw new Exception("MARKED TICK REACHED");}
}

[GameModule("poima-test-unmarked-extension-helper")]
public sealed class UnmarkedExtensionProbe : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state) {state.Count=30;}
    public override void Tick(ref ProbeState state,GameContext context)
    {_=context.GetAnimationExtended(new(11,31));++state.Count;}
}

static unsafe partial class Program
{
    static int extendedGets,extendedSets,malformedExtendedState;
    static bool extendedError;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ExtendedGet(void* context,Id* id,ExtendedAnimation* output,void* error)
    {
        ++extendedGets;
        if((nint)context!=0x1234 || id->High!=11 || output->Version!=1 || output->Bytes!=136 || output->Reserved!=0)badPayload=true;
        if(extendedError) {
            Encoding.UTF8.GetBytes("extended rejected\0",new Span<byte>(error,2048));return -1;
        }
        *output=new(){Version=1,Bytes=136,Mode=id->Low==31 ? 1u:0u,
            State=new(){Entity=*id,Present=id->Low==34 ? 0u:1u,Clip=3,Time=.125,Speed=2,Duration=4,Loop=1,
                TransitionPresent=id->Low is 31 or 32 ? 1u:0u,
                Transition=new(){Start=99,Duration=32,Elapsed=24,Weight=.75,Clip=2,Time=.5,Speed=.25,
                    Frozen=id->Low==31 ? 1u:0u,Loop=0,Playing=1}}};
        switch(malformedExtendedState) {
            case 1:output->Version=2;break;
            case 2:output->Bytes=135;break;
            case 3:output->Reserved=1;break;
            case 4:output->Mode=2;break;
            case 5:output->State.Reserved=1;break;
            case 6:output->State.Present=2;break;
            case 7:output->State.TransitionPresent=2;break;
            case 8:output->State.TransitionPresent=0;break; // Mode 1 with no active transition.
            case 9:output->State.Present=0;break; // Absent rig with active transition.
            case 10:output->State.Transition.Duration=0;break;
            case 11:output->State.Transition.Elapsed=32;break;
            case 12:output->State.Transition.Weight=double.NaN;break;
            case 13:output->State.Transition.Weight=.5;break;
            case 14:output->State.Transition.Frozen=0;break;
        }
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ExtendedSet(void* context,ExtendedCommand* value,void* error)
    {
        ++extendedSets;
        if((nint)context!=0x1234 || value->Version!=1 || value->Bytes!=64 || value->Reserved!=0 || value->Command.Entity.High!=11)badPayload=true;
        var command=value->Command;
        if(value->Mode==0) {
            if(command.Entity.Low!=22 || command.Clip!=3 || command.Time!=.5 || command.Speed!=.75 ||
                command.Loop!=0 || command.Playing!=1 || command.BlendTicks!=12)badPayload=true;
        }else if(value->Mode==1) {
            if(command.Entity.Low!=23 || command.Clip!=-1 || command.Time!=.125 || command.Speed!=1.5 ||
                command.Loop!=1 || command.Playing!=0 || command.BlendTicks!=9)badPayload=true;
        }else badPayload=true;
        return 0;
    }
    static JsonElement ExtensionHost(uint bytes=192,bool animation=true,uint epoch=7,uint callBytes=80)
        =>JsonSerializer.SerializeToElement(new {call_version=1,call_bytes=callBytes,services_version=epoch,services_bytes=bytes,
            features=animation ? new[]{"baseline_v7","animation_inertial_v1"}:new[]{"baseline_v7"}});
    static JsonElement ExtensionJson(string json)
    {using var document=JsonDocument.Parse(json);return document.RootElement.Clone();}
    static int LoadExtension(Call* call,Type type,JsonElement? host)
    {
        var data=new Dictionary<string,object?>{{"assembly",Assembly.GetExecutingAssembly().Location},{"type",type.FullName}};
        if(host.HasValue)data.Add("host_contract",host.Value);
        byte[] encoded=Encoding.UTF8.GetBytes(JsonSerializer.Serialize(data)+"\0");
        call->Operation=1;call->Handle=0;
        fixed(byte* text=encoded) {call->Text=text;return Entry.Invoke((nint)call,sizeof(Call));}
    }
    static void NoExtensionModules(Call* call)
    {
        call->Operation=5;Check(Entry.Invoke((nint)call,sizeof(Call))==0,Output(call->Output));
        using var collected=JsonDocument.Parse(Output(call->Output));
        Check(collected.RootElement.GetProperty("active_modules").GetInt32()==0 &&
            collected.RootElement.GetProperty("retired_alive").GetInt32()==0,"Extension fixture retained a context.");
    }
    static void AnimationExtensionContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(AnimationServices)==192 && sizeof(ExtendedCommand)==64 && sizeof(ExtendedAnimation)==136 &&
            Marshal.OffsetOf<AnimationServices>(nameof(AnimationServices.Get)).ToInt64()==176 &&
            Marshal.OffsetOf<AnimationServices>(nameof(AnimationServices.Set)).ToInt64()==184 &&
            Marshal.OffsetOf<ExtendedCommand>(nameof(ExtendedCommand.Command)).ToInt64()==8 &&
            Marshal.OffsetOf<ExtendedCommand>(nameof(ExtendedCommand.Mode)).ToInt64()==56 &&
            Marshal.OffsetOf<ExtendedAnimation>(nameof(ExtendedAnimation.State)).ToInt64()==8 &&
            Marshal.OffsetOf<ExtendedAnimation>(nameof(ExtendedAnimation.Mode)).ToInt64()==128,"Independent extension layout mismatch.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        var invalidHosts=new JsonElement?[]{null,ExtensionHost(176),ExtensionHost(192,false),ExtensionHost(epoch:8),ExtensionHost(callBytes:79),
            JsonSerializer.SerializeToElement((string?)null),JsonSerializer.SerializeToElement(new[]{1}),
            ExtensionJson("{\"call_version\":1,\"call_bytes\":80,\"services_version\":7,\"services_bytes\":192,\"features\":[\"baseline_v7\",\"animation_inertial_v1\",\"animation_inertial_v1\"]}"),
            ExtensionJson("{\"call_version\":1,\"call_bytes\":80,\"services_version\":7,\"services_bytes\":192,\"services_bytes\":192,\"features\":[\"baseline_v7\",\"animation_inertial_v1\"]}")};
        foreach(var host in invalidHosts) {
            Check(LoadExtension(&call,typeof(InertialConstructorProbe),host)!=0,"Invalid host accepted marked constructor.");
            Check(!Output(output).Contains("MARKED CONSTRUCTOR REACHED") && Output(output).Contains("host",StringComparison.OrdinalIgnoreCase),
                "Invalid host did not reject before marked constructor.");
            NoExtensionModules(&call);
        }
        Check(LoadExtension(&call,typeof(InertialConstructorProbe),ExtensionHost())!=0 && Output(output).Contains("MARKED CONSTRUCTOR REACHED"),
            "Valid host did not reach the positive constructor sentinel.");
        NoExtensionModules(&call);
        checks.Add("marked CoreCLR loads reject missing/short/wrong/duplicate host contracts before constructor; valid192 negotiation reaches constructor sentinel; failed contexts collected");

        Check(LoadExtension(&call,typeof(InertialProbeGame),ExtensionHost())==0,Output(output));
        using(var declaration=JsonDocument.Parse(Output(output))) {
            var manifest=declaration.RootElement;call.Handle=manifest.GetProperty("handle").GetUInt64();
            var required=manifest.GetProperty("requirements");
            Check(required.GetProperty("services_bytes").GetUInt32()==192 && required.GetProperty("services_version").GetUInt32()==7 &&
                required.GetProperty("features").EnumerateArray().Select(v=>v.GetString()).Order().SequenceEqual(new[]{"animation_inertial_v1","baseline_v7"}),
                "Marked manifest omitted exact extension requirement.");
            Check(manifest.GetProperty("fields").GetArrayLength()==1 && manifest.GetProperty("bytes").GetInt32()==4,"Requirements changed state schema.");
        }
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==20,Output(output));
        AnimationServices extended=new(){Baseline=good,Get=&ExtendedGet,Set=&ExtendedSet};extended.Baseline.Bytes=192;
        foreach(uint operation in new uint[]{3,6}) {
            call.Operation=operation;call.InputCount=0;call.Inputs=null;
            foreach(int size in new[]{176,184}) {
                using var guarded=new GuardedHeader(size);*(Services*)guarded.Header=good;((Services*)guarded.Header)->Bytes=(uint)size;
                call.Services=(Services*)guarded.Header;int before=((ProbeState*)state)->Count,reads=extendedGets,writes=extendedSets;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("192 bytes"),"Marked short table was accepted or touched its protected tail.");
                Check(((ProbeState*)state)->Count==before && extendedGets==reads && extendedSets==writes,"Marked prefix rejection entered game.");
            }
            for(int slot=0;slot<2;++slot) {
                var missing=extended;if(slot==0)missing.Get=null;else missing.Set=null;call.Services=&missing.Baseline;
                int before=((ProbeState*)state)->Count,reads=extendedGets,writes=extendedSets;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("callback is absent"),"Marked null extension callback accepted.");
                Check(((ProbeState*)state)->Count==before && extendedGets==reads && extendedSets==writes,"Missing tail callback entered game.");
            }
        }
        checks.Add("marked Tick/Control reject actual176/184 guarded allocations before extension reads and reject either null tail callback before game execution");
        call.Services=&extended.Baseline;call.Operation=3;extendedGets=0;extendedSets=0;int oldGets=gets,oldSets=sets;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        Check(((ProbeState*)state)->Count==21 && extendedGets==4 && extendedSets==2 && gets==oldGets+1 && sets==oldSets+1 && !badPayload,
            "Marked192 SDK transfer or old overload mismatch.");
        call.Operation=6;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==22,Output(output));
        checks.Add("real bridge marked192 Tick/Control transfers independent64/136 PODs, both transition modes, nullable active mode/progress, rest clip, old getter and seven-argument setter; invalid enum never reaches callback");
        call.Operation=3;
        for(int malformed=1;malformed<=14;++malformed) {
            malformedExtendedState=malformed;int before=((ProbeState*)state)->Count,reads=extendedGets,writes=extendedSets;
            string expected=malformed==9 ? "Missing animation cannot have an active transition" : "versioned native animation";
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains(expected),"Malformed extended SDK response accepted.");
            Check(((ProbeState*)state)->Count==before && extendedGets==reads+1 && extendedSets==writes,"Malformed response continued gameplay.");
        }
        malformedExtendedState=0;extendedError=true;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output)=="extended rejected","Extended native error text was not transferred.");
        extendedError=false;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==23 && !badPayload,"Marked channel did not recover after mock errors.");
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        NoExtensionModules(&call);
        checks.Add("SDK rejects14 malformed mock extended-state responses, transfers mock native error, and later executes valid Tick; extension contexts retired");

        Check(LoadExtension(&call,typeof(UnmarkedExtensionProbe),null)==0,Output(output));
        using(var manifest=JsonDocument.Parse(Output(output))) {
            call.Handle=manifest.RootElement.GetProperty("handle").GetUInt64();
            Check(!manifest.RootElement.TryGetProperty("requirements",out _),"Legacy load received a requirements sibling.");
        }
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));call.Operation=3;call.Services=&good;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare IInertialAnimationGame") && ((ProbeState*)state)->Count==30,
            "Undeclared helper did not safely reject the baseline-only view.");
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("unmarked game remains legacy-compatible at load and safely rejects extended helper on176 baseline; no load-time IL capability claim");
    }
}
