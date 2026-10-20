// SPDX-License-Identifier: Apache-2.0
// Independent wire declarations; these stubs prove ABI transfer, not native clock math.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;
[StructLayout(LayoutKind.Sequential)] struct RateCommand
{public uint Version,Bytes;public Id Entity;public uint Layer,Reserved;public double Speed;}
[StructLayout(LayoutKind.Sequential)] unsafe struct RateServices
{public GroundingServices Grounding;public delegate* unmanaged[Cdecl]<void*,RateCommand*,void*,int> Set;}
[GameModule("poima-test-animation-rate")]
public sealed class RateProbe:Game<ProbeState>,IAnimationRateGame
{
    public override void Initialize(ref ProbeState state){state.Count=3000;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        EntityId entity=new(11,40);var before=context.GetAnimationExtended(entity);
        if(before is null || before.Value.State.Speed!=1)throw new Exception("Committed rate state differs.");
        context.SetAnimationSpeed(entity,.5);context.SetAnimationLayerSpeed(entity,2,2);
        if(context.GetAnimationExtended(entity)!=before)throw new Exception("Staged rate leaked into getter.");
        foreach(double rate in new[]{-.1,8.1,double.NaN,double.PositiveInfinity})
        {bool denied=false;try{context.SetAnimationSpeed(entity,rate);}catch(ArgumentException){denied=true;}if(!denied)throw new Exception("Invalid rate entered host.");}
        foreach(uint layer in new[]{0u,5u})
        {bool denied=false;try{context.SetAnimationLayerSpeed(entity,layer,1);}catch(ArgumentException){denied=true;}if(!denied)throw new Exception("Invalid layer entered host.");}
        bool masked=false;try{_=context.GetAnimationLayer(entity,2);}catch(ArgumentException e){masked=e.Message.Contains("Declare IMaskedAnimationGame");}
        if(!masked)throw new Exception("Speed feature granted full layer API.");
        bool grounding=false;try{_=context.GetGroundingControl(entity,1);}catch(ArgumentException e){grounding=e.Message.Contains("Declare IGroundingControlGame");}
        if(!grounding)throw new Exception("Speed feature granted grounding API.");
        ++state.Count;
    }
}
[GameModule("poima-test-rate-constructor")]
public sealed class RateConstructorProbe:Game<ProbeState>,IAnimationRateGame
{
    public RateConstructorProbe(){throw new Exception("RATE CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){}
    public override void Tick(ref ProbeState state,GameContext context){}
}
[GameModule("poima-test-rate-unmarked")]
public sealed class RateUnmarkedProbe:Game<ProbeState>,IInertialAnimationGame
{
    public override void Initialize(ref ProbeState state){state.Count=3100;}
    public override void Tick(ref ProbeState state,GameContext context){context.SetAnimationSpeed(new(11,40),1);++state.Count;}
}
[GameModule("poima-test-rate-all")]
public sealed class RateAllProbe:Game<ProbeState>,IAnimationRateGame,IGroundingControlGame,ICharacterStanceGame,ICameraLensGame,IRootMotionGame,
    IHitRegionsGame,IInputActionsGame,IPlayerPreferencesGame,IHierarchicalInstancesGame,INavigationGame,ICharacterInputGame,IMaskedAnimationGame
{
    public override void Initialize(ref ProbeState state){}
    public override void Tick(ref ProbeState state,GameContext context){}
}
static unsafe partial class Program
{
    static int rateWrites,rateReads;static bool rateError;static readonly List<RateCommand> rateCommands=[];
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int RateGet(void* context,Id* entity,ExtendedAnimation* output,void* error)
    {
        ++rateReads;if((nint)context!=0x1234 || output->Version!=1 || output->Bytes!=136 || output->Reserved!=0)badPayload=true;
        *output=new(){Version=1,Bytes=136,State=new(){Entity=*entity,Present=1,Clip=1,Time=.5,Speed=1,Duration=2,Loop=1,Playing=1,Transition=new(){Clip=-1}}};return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int RateSet(void* context,RateCommand* command,void* error)
    {
        ++rateWrites;rateCommands.Add(*command);
        if((nint)context!=0x1234 || command->Version!=1 || command->Bytes!=40 || command->Reserved!=0)badPayload=true;
        return rateError?LifecycleError(error):0;
    }
    static JsonElement RateHost(uint bytes=376,bool rate=true,bool inertial=true,uint epoch=7,bool all=false)
    {
        var names=new List<string>{"baseline_v7"};if(rate)names.Add("animation_rate_v1");if(inertial)names.Add("animation_inertial_v1");
        if(all)foreach(var item in GroundingHost(all:true).GetProperty("features").EnumerateArray()){var name=item.GetString()!;if(!names.Contains(name))names.Add(name);}
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void AnimationRateContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(RateCommand)==40&&sizeof(RateServices)==376&&Marshal.OffsetOf<RateCommand>(nameof(RateCommand.Entity)).ToInt64()==8&&
            Marshal.OffsetOf<RateCommand>(nameof(RateCommand.Layer)).ToInt64()==24&&Marshal.OffsetOf<RateCommand>(nameof(RateCommand.Reserved)).ToInt64()==28&&
            Marshal.OffsetOf<RateCommand>(nameof(RateCommand.Speed)).ToInt64()==32&&Marshal.OffsetOf<RateServices>(nameof(RateServices.Set)).ToInt64()==368,"Independent rate layout differs.");
        Check(typeof(ControlContext).GetMethod("SetAnimationSpeed")==null&&typeof(ControlContext).GetMethod("SetAnimationLayerSpeed")==null,"Rate entered Control context.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,RateHost(368),RateHost(375),RateHost(rate:false),RateHost(inertial:false),RateHost(epoch:8)})
        {Check(LoadExtension(&call,typeof(RateConstructorProbe),host)!=0&&!Output(output).Contains("RATE CONSTRUCTOR REACHED"),"Invalid rate host reached constructor.");NoExtensionModules(&call);}
        foreach(uint bytes in new[]{376u,384u})
        {Check(LoadExtension(&call,typeof(RateConstructorProbe),RateHost(bytes))!=0&&Output(output).Contains("RATE CONSTRUCTOR REACHED"),"Valid rate host denied constructor.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(RateAllProbe),RateHost(all:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var req=doc.RootElement.GetProperty("requirements");Check(req.GetProperty("features").GetArrayLength()==16&&req.GetProperty("services_bytes").GetUInt32()==376,"All16 rate union differs.");var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;Check(Convert.ToUInt32(parser.Invoke(null,[req.GetRawText()]))==32767,"Rate generated union flags differ.");}
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(RateProbe),RateHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var req=doc.RootElement.GetProperty("requirements");Check(req.GetProperty("services_bytes").GetUInt32()==376&&req.GetProperty("features").EnumerateArray().Select(x=>x.GetString()).SequenceEqual(new[]{"animation_rate_v1","animation_inertial_v1","baseline_v7"}),"Rate declaration grants unrelated features.");}
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        RateServices services=default;var anim=&services.Grounding.Orientation.Stance.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation;
        anim->Baseline=good;anim->Baseline.Bytes=376;anim->Get=&RateGet;anim->Set=&ExtendedSet;services.Set=&RateSet;
        rateReads=rateWrites=0;rateError=false;rateCommands.Clear();call.Operation=3;
        foreach(int bytes in new[]{176,192,208,352,368,375})
        {using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;if(bytes>=192){var prefix=(AnimationServices*)guard.Header;prefix->Get=&RateGet;prefix->Set=&ExtendedSet;}call.Services=(Services*)guard.Header;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0&&rateReads==0&&rateWrites==0,"Protected short rate tail accessed.");}
        var absent=services;absent.Set=null;call.Services=&absent.Grounding.Orientation.Stance.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0&&rateReads==0&&rateWrites==0,"Missing rate callback entered game.");
        call.Services=&anim->Baseline;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0&&((ProbeState*)state)->Count==3001&&rateReads==2&&rateWrites==2&&!badPayload,Output(output));
        Check(rateCommands.Count==2&&rateCommands[0].Entity.High==11&&rateCommands[0].Entity.Low==40&&rateCommands[0].Layer==0&&rateCommands[0].Speed==.5&&rateCommands[1].Layer==2&&rateCommands[1].Speed==2,"Speed-only base/layer payload differs.");
        rateError=true;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0&&((ProbeState*)state)->Count==3001&&Output(output).Contains("lifecycle rejected"),"Rate callback failure did not preserve compiled state/error.");rateError=false;
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(RateUnmarkedProbe),ExtensionHost())==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(192))
        {*(Services*)guard.Header=good;var prefix=(AnimationServices*)guard.Header;prefix->Baseline.Bytes=192;prefix->Get=&RateGet;prefix->Set=&ExtendedSet;call.Services=(Services*)guard.Header;call.Operation=3;int writes=rateWrites;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0&&Output(output).Contains("Declare IAnimationRateGame")&&rateWrites==writes,"Unmarked game accessed rate tail.");}
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("animation_rate376/CMD40 independent POD,16feature union, base/layer-only rights, protected short-tail and pre-construction negotiation");
        checks.Add("speed-only transfer, existing getter staged invisibility, malformed inputs, callback errors and retained192 denial");
    }
}
