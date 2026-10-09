// SPDX-License-Identifier: Apache-2.0
// Independent ABI declarations/adversarial callbacks. Actual runtime instance
// publication and save behavior are qualified by the separate compiled consumer.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)] unsafe struct InstanceServices
{
    public NavServices Navigation;
    public delegate* unmanaged[Cdecl]<void*,Id*,Id*,Id*,void*,int> Resolve;
}
[GameModule("poima-test-instance-extension")]
public class InstanceProbeGame : Game<ProbeState>,IHierarchicalInstancesGame
{
    public override void Initialize(ref ProbeState state){state.Count=500;}
    internal static void One(GameContext context)
    {
        if(context.ResolveNode(new(11,80),new(12,81))!=new EntityId(13,82))throw new Exception("Instance mapping lost ID words.");
    }
    public override void Tick(ref ProbeState state,GameContext context)
    {
        foreach(bool root in new[]{false,true}) {
            bool rejected=false;try {_=context.ResolveNode(root?default:new EntityId(11,80),root?new TemplateNodeId(12,81):default);}
            catch(ArgumentException){rejected=true;}
            if(!rejected)throw new Exception("Zero instance root or local ID accepted.");
        }
        bool denied=false;try{context.SetCharacterInput(new(11,40),0,0);}catch(ArgumentException e){denied=e.Message.Contains("Declare ICharacterInputGame");}
        if(!denied)throw new Exception("Instance prefix granted character input.");
        Span<NavigationPoint> corners=stackalloc NavigationPoint[2];
        denied=false;try{_=context.FindNavigationPath(new(11,40),new(1,2,3),corners);}catch(ArgumentException e){denied=e.Message.Contains("Declare INavigationGame");}
        if(!denied)throw new Exception("Instance prefix granted navigation.");
        denied=false;try{_=context.GetAnimationExtended(new(11,33));}catch(ArgumentException e){denied=e.Message.Contains("Declare IInertialAnimationGame");}
        if(!denied)throw new Exception("Instance prefix granted inertial animation.");
        denied=false;try{_=context.GetAnimationLayer(new(11,31),1);}catch(ArgumentException e){denied=e.Message.Contains("Declare IMaskedAnimationGame");}
        if(!denied)throw new Exception("Instance prefix granted layers.");
        One(context);++state.Count;
    }
    public override void Control(ref ProbeState state,ControlContext context)
    {
        if(context.ResolveNode(new(11,80),new(12,81))!=new EntityId(13,82))throw new Exception("Control instance resolution lost negotiated feature.");
        ++state.Count;
    }
}
[GameModule("poima-test-instance-constructor")]
public sealed class InstanceConstructorProbe : Game<ProbeState>,IHierarchicalInstancesGame
{
    public InstanceConstructorProbe(){throw new Exception("INSTANCE CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){throw new Exception("INSTANCE INITIALIZE REACHED");}
    public override void Tick(ref ProbeState state,GameContext context){throw new Exception("INSTANCE TICK REACHED");}
}
[GameModule("poima-test-instance-unmarked")]
public sealed class InstanceUnmarkedProbe : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state){state.Count=600;}
    public override void Tick(ref ProbeState state,GameContext context){InstanceProbeGame.One(context);++state.Count;}
    public override void Control(ref ProbeState state,ControlContext context){_=context.ResolveNode(new(11,80),new(12,81));++state.Count;}
}
[StructLayout(LayoutKind.Sequential)] public struct UnsupportedNodeState {public TemplateNodeId Node;}
[GameModule("poima-test-template-node-state-rejected")]
public sealed class UnsupportedNodeGame : Game<UnsupportedNodeState>,IHierarchicalInstancesGame
{
    public override void Initialize(ref UnsupportedNodeState state){ }
    public override void Tick(ref UnsupportedNodeState state,GameContext context){ }
}
[GameModule("poima-test-instance-character")]
public sealed class InstanceCharacterProbe : Game<ProbeState>,IHierarchicalInstancesGame,ICharacterInputGame
{
    public override void Initialize(ref ProbeState state){state.Count=700;}
    public override void Tick(ref ProbeState state,GameContext context){InstanceProbeGame.One(context);CharacterProbeGame.Intent(context);++state.Count;}
}
static unsafe partial class Program
{
    static int instanceCalls,instanceReplyMode;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int InstanceResolve(void* context,Id* root,Id* local,Id* result,void* error)
    {
        ++instanceCalls;
        if((nint)context!=0x1234 || root->High!=11 || root->Low!=80 || local->High!=12 || local->Low!=81 || result->High!=0 || result->Low!=0)badPayload=true;
        if(instanceReplyMode==1)return LifecycleError(error);
        if(instanceReplyMode==2){*result=default;return 0;}
        *result=new(){High=13,Low=82};return 0;
    }
    static JsonElement InstanceHost(uint bytes=232,bool instances=true,bool character=false)
        =>JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=7,services_bytes=bytes,
            features=new[]{"baseline_v7"}.Concat(character?new[]{"character_input_v1"}:Array.Empty<string>()).Concat(instances?new[]{"hierarchical_instances_v1"}:Array.Empty<string>()).ToArray()});
    static void InstanceContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(InstanceServices)==232 && Marshal.OffsetOf<InstanceServices>(nameof(InstanceServices.Resolve)).ToInt64()==224 && sizeof(TemplateNodeId)==16,"Independent instance ABI layout mismatch.");
        Check(TemplateNodeId.Parse("fedcba98765432100000000000000016")==new TemplateNodeId(0xfedcba9876543210,22),"Template node parsing differs.");
        foreach(string? invalid in new string?[]{null,"node",new string('0',32),new string('A',32),new string('g',32)}) {
            bool rejected=false;try{_=TemplateNodeId.Parse(invalid!);}catch(ArgumentException){rejected=true;}
            Check(rejected,"Malformed template-local identity accepted.");
        }
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,InstanceHost(176),InstanceHost(192),InstanceHost(208),InstanceHost(216),InstanceHost(224),InstanceHost(231),InstanceHost(instances:false),
            ExtensionJson("{\"call_version\":1,\"call_bytes\":80,\"services_version\":8,\"services_bytes\":232,\"features\":[\"baseline_v7\",\"hierarchical_instances_v1\"]}")}) {
            Check(LoadExtension(&call,typeof(InstanceConstructorProbe),host)!=0 && !Output(output).Contains("INSTANCE CONSTRUCTOR REACHED"),"Invalid instance host entered constructor.");NoExtensionModules(&call);
        }
        foreach(uint bytes in new[]{232u,256u}) {
            Check(LoadExtension(&call,typeof(InstanceConstructorProbe),InstanceHost(bytes))!=0 && Output(output).Contains("INSTANCE CONSTRUCTOR REACHED"),"Valid named instance host failed positive constructor sentinel.");NoExtensionModules(&call);
        }
        checks.Add("instance-only named232 and larger hosts admit before constructor; absent feature/old extents/wrong epoch reject before construction; local IDs parse distinctly");
        Check(LoadExtension(&call,typeof(InstanceProbeGame),InstanceHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output))) {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var r=doc.RootElement.GetProperty("requirements");
            Check(r.GetProperty("services_bytes").GetUInt32()==232 && r.GetProperty("features").EnumerateArray().Select(v=>v.GetString()).SequenceEqual(new[]{"baseline_v7","hierarchical_instances_v1"}),"Instance-only requirements granted unrelated features.");
        }
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        InstanceServices services=new(){Navigation=new(){Character=new(){Animation=new(){Animation=new(){Baseline=good}}}},Resolve=&InstanceResolve};
        services.Navigation.Character.Animation.Animation.Baseline.Bytes=232;
        new Span<byte>((byte*)&services+176,48).Fill(0xa5);
        instanceCalls=instanceReplyMode=0;
        foreach(uint operation in new[]{3u,6u}) {
            call.Operation=operation;
            foreach(int bytes in new[]{176,192,208,216,224}) {
                using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;
                int before=((ProbeState*)state)->Count,calls=instanceCalls;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==before && instanceCalls==calls,"Short guarded allocation read resolver tail or entered game.");
            }
            var missing=services;missing.Resolve=null;call.Services=&missing.Navigation.Character.Animation.Animation.Baseline;
            int prior=((ProbeState*)state)->Count;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==prior,"Missing own resolver entered game.");
            call.Services=&services.Navigation.Character.Animation.Animation.Baseline;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==prior+1 && !badPayload,Output(output));
            foreach(int mode in new[]{1,2}) {
                instanceReplyMode=mode;prior=((ProbeState*)state)->Count;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==prior,"Native error or zero success handle changed game state.");
            }
            instanceReplyMode=0;
        }
        Check(instanceCalls==6,"Zero arguments or missing tables unexpectedly reached resolver.");
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("Tick/Control guard actual176..224 allocations; poison all unrequested extension callbacks; exact ID transfer, local argument denial, native failure and malformed zero success reject safely");
        Check(LoadExtension(&call,typeof(InstanceCharacterProbe),InstanceHost(character:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output))){call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();Check(doc.RootElement.GetProperty("requirements").GetProperty("features").EnumerateArray().Select(v=>v.GetString()).SequenceEqual(new[]{"baseline_v7","character_input_v1","hierarchical_instances_v1"}),"Combined instance character requirements differ.");}
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        services=new(){Navigation=new(){Character=new(){Animation=new(){Animation=new(){Baseline=good}},Set=&CharacterSet}},Resolve=&InstanceResolve};services.Navigation.Character.Animation.Animation.Baseline.Bytes=232;
        call.Services=&services.Navigation.Character.Animation.Animation.Baseline;call.Operation=3;
        var noCharacter=services;noCharacter.Navigation.Character.Set=null;call.Services=&noCharacter.Navigation.Character.Animation.Animation.Baseline;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==700,"Combined instance profile omitted required character callback.");
        call.Services=&services.Navigation.Character.Animation.Animation.Baseline;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==701 && !badPayload,Output(output));
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("independent instance plus character profile retains exact names and validates each opted-in callback only");
        Check(LoadExtension(&call,typeof(InstanceUnmarkedProbe),null)==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(176)) {
            *(Services*)guard.Header=good;call.Services=(Services*)guard.Header;
            foreach(uint operation in new[]{3u,6u}) {call.Operation=operation;int calls=instanceCalls;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare IHierarchicalInstancesGame") && ((ProbeState*)state)->Count==600 && instanceCalls==calls,"Unmarked Tick/Control read protected resolver tail.");}
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(UnsupportedNodeGame),InstanceHost())!=0 && Output(output).Contains("Unsupported state field"),"Template-local identity accepted as live entity state kind.");NoExtensionModules(&call);
        checks.Add("unmarked Tick/Control deny resolver before protected tail access; template-local identity is not silently serialized as a live entity");
    }
}
