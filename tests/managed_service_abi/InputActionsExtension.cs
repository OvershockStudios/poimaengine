// SPDX-License-Identifier: Apache-2.0
// Independent wire declarations, including poisoned unused intermediate tails.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)] struct ActionReply { public uint Version,Bytes,Flags,Reserved; }
[StructLayout(LayoutKind.Sequential)] unsafe struct ActionServices
{
    public PreferenceServices Preferences;
    public delegate* unmanaged[Cdecl]<void*,Id*,byte*,uint,ActionReply*,void*,int> Get;
}
[GameModule("poima-test-actions")]
public sealed class ActionProbeGame : Game<ProbeState>,IInputActionsGame
{
    public override void Initialize(ref ProbeState state){state.Count=1000;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        foreach(string? name in new string?[]{null,"","1fire","Fire","é","a b","a/b","a\0b",new('a',65)})
        {
            bool denied=false;try{_=context.GetInputAction(new(11,40),name!);}catch(ArgumentException){denied=true;}
            if(!denied)throw new Exception("Invalid local action name reached host.");
        }
        bool blocked=false;try{_=context.GetPlayerPreferences();}catch(ArgumentException e){blocked=e.Message.Contains("Declare IPlayerPreferencesGame");}
        if(!blocked)throw new Exception("Action feature granted preferences.");
        blocked=false;try{_=context.ResolveNode(new(11,80),new(12,81));}catch(ArgumentException e){blocked=e.Message.Contains("Declare IHierarchicalInstancesGame");}
        if(!blocked)throw new Exception("Action feature granted instances.");
        blocked=false;try{context.SetCharacterInput(new(11,40),0,0);}catch(ArgumentException e){blocked=e.Message.Contains("Declare ICharacterInputGame");}
        if(!blocked)throw new Exception("Action feature granted character input.");
        Span<NavigationPoint> corners=stackalloc NavigationPoint[2];
        blocked=false;try{_=context.FindNavigationPath(new(11,40),new(1,2,3),corners);}catch(ArgumentException e){blocked=e.Message.Contains("Declare INavigationGame");}
        if(!blocked)throw new Exception("Action feature granted navigation.");
        blocked=false;try{_=context.GetAnimationExtended(new(11,33));}catch(ArgumentException e){blocked=e.Message.Contains("Declare IInertialAnimationGame");}
        if(!blocked)throw new Exception("Action feature granted inertial animation.");
        blocked=false;try{_=context.GetAnimationLayer(new(11,31),1);}catch(ArgumentException e){blocked=e.Message.Contains("Declare IMaskedAnimationGame");}
        if(!blocked)throw new Exception("Action feature granted animation layers.");
        if(context.GetInputAction(new(11,40),"fire")!=new InputActionState(true,true,false,true) ||
            context.GetInputAction(new(11,40),"weapon.ads-1")!=new InputActionState(false,true,true,true) ||
            context.GetInputAction(new(11,40),"idle")!=new InputActionState(false,false,false,true) ||
            context.GetInputAction(new(11,40),new('a',64))!=default ||
            context.GetInputAction(new(11,41),"fire")!=default)
            throw new Exception("Action state lost held/edge/presence or other-entity absence.");
        ++state.Count;
    }
}
[GameModule("poima-test-action-constructor")]
public sealed class ActionConstructorProbe : Game<ProbeState>,IInputActionsGame
{
    public ActionConstructorProbe(){throw new Exception("ACTION CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){throw new Exception("ACTION INITIALIZE REACHED");}
    public override void Tick(ref ProbeState state,GameContext context){throw new Exception("ACTION TICK REACHED");}
}
[GameModule("poima-test-action-combined-constructor")]
public sealed class ActionCombinedConstructorProbe : Game<ProbeState>,IInputActionsGame,IPlayerPreferencesGame
{
    public ActionCombinedConstructorProbe(){throw new Exception("ACTION COMBINED CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){throw new Exception("ACTION INITIALIZE REACHED");}
    public override void Tick(ref ProbeState state,GameContext context){throw new Exception("ACTION TICK REACHED");}
}
[GameModule("poima-test-action-unmarked")]
public sealed class ActionUnmarkedProbe : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state){state.Count=1100;}
    public override void Tick(ref ProbeState state,GameContext context){_=context.GetInputAction(new(11,40),"fire");++state.Count;}
}
static unsafe partial class Program
{
    static int actionCalls,actionReplyMode;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ActionRead(void* context,Id* entity,byte* name,uint bytes,ActionReply* output,void* error)
    {
        ++actionCalls;
        if((nint)context!=0x1234 || entity->High!=11 || entity->Low is not (40 or 41) ||
            output->Version!=1 || output->Bytes!=16 || output->Flags!=0 || output->Reserved!=0 || bytes is <1 or >64)
            badPayload=true;
        string text=Encoding.ASCII.GetString(new ReadOnlySpan<byte>(name,(int)bytes));
        if(text is not ("fire" or "weapon.ads-1" or "idle") && text!=new string('a',64))badPayload=true;
        if(actionReplyMode==1)return LifecycleError(error);
        *output=new(){Version=1,Bytes=16,Flags=entity->Low==41?0u:text=="fire"?11u:text=="weapon.ads-1"?14u:text=="idle"?8u:0u};
        switch(actionReplyMode)
        {
            case 2:output->Version=2;break;
            case 3:output->Bytes=15;break;
            case 4:output->Bytes=24;break;
            case 5:output->Reserved=1;break;
            case 6:output->Flags=16;break;
            case 7:output->Flags=1;break;
        }
        return 0;
    }
    static JsonElement ActionHost(uint bytes=264,bool actions=true,bool preferences=false,uint epoch=7)
    {
        var names=new List<string>{"baseline_v7"};if(actions)names.Add("input_actions_v1");if(preferences)names.Add("player_preferences_v1");
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void InputActionsContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(ActionReply)==16 && sizeof(ActionServices)==264 && Marshal.OffsetOf<ActionServices>(nameof(ActionServices.Get)).ToInt64()==256 &&
            Marshal.OffsetOf<ActionReply>(nameof(ActionReply.Flags)).ToInt64()==8 && Marshal.OffsetOf<ActionReply>(nameof(ActionReply.Reserved)).ToInt64()==12,
            "Independent action layout mismatch.");
        Check(typeof(ControlContext).GetMethod("GetInputAction")==null,"Tick-only action API leaked into Control.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,ActionHost(176),ActionHost(192),ActionHost(208),ActionHost(216),ActionHost(224),ActionHost(232),ActionHost(256),ActionHost(263),ActionHost(actions:false),ActionHost(epoch:8),
            ExtensionJson("{\"call_version\":1,\"call_bytes\":80,\"services_version\":7,\"services_bytes\":264,\"features\":[\"baseline_v7\",\"input_actions_v1\",\"input_actions_v1\"]}")})
        {
            Check(LoadExtension(&call,typeof(ActionConstructorProbe),host)!=0 && !Output(output).Contains("ACTION CONSTRUCTOR REACHED"),"Invalid action host entered constructor.");NoExtensionModules(&call);
        }
        foreach(uint bytes in new[]{264u,272u})
        {Check(LoadExtension(&call,typeof(ActionConstructorProbe),ActionHost(bytes))!=0 && Output(output).Contains("ACTION CONSTRUCTOR REACHED"),"Valid action host failed constructor sentinel.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(ActionCombinedConstructorProbe),ActionHost())!=0 && !Output(output).Contains("ACTION COMBINED CONSTRUCTOR REACHED"),"Actions implicitly granted preference feature.");NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(ActionCombinedConstructorProbe),ActionHost(preferences:true))!=0 && Output(output).Contains("ACTION COMBINED CONSTRUCTOR REACHED"),"Combined declared features failed negotiation.");NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(ActionProbeGame),ActionHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var required=doc.RootElement.GetProperty("requirements");
            Check(required.GetProperty("services_bytes").GetUInt32()==264 && required.GetProperty("features").EnumerateArray().Select(v=>v.GetString()).SequenceEqual(new[]{"baseline_v7","input_actions_v1"}),"Action-only requirements granted unrelated extensions.");
        }
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        ActionServices services=new(){Preferences=new(){Instances=new(){Navigation=new(){Character=new(){Animation=new(){Animation=new(){Baseline=good}}}}}},Get=&ActionRead};
        services.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline.Bytes=264;
        new Span<byte>((byte*)&services+176,80).Fill(0xa5);actionCalls=actionReplyMode=0;call.Operation=3;
        foreach(int bytes in new[]{176,192,208,216,224,232,256,263})
        {
            using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==1000 && actionCalls==0,"Short protected action allocation entered game/read tail.");
        }
        var missing=services;missing.Get=null;call.Services=&missing.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==1000 && actionCalls==0,"Missing action callback entered game.");
        call.Services=&services.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==1001 && actionCalls==5 && !badPayload,Output(output));
        for(int mode=1;mode<=7;++mode)
        {
            actionReplyMode=mode;int calls=actionCalls;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==1001 && actionCalls==calls+1,"Native failure/malformed action reply entered state commit: "+mode);
            if(mode==1)Check(Output(output).Contains("lifecycle rejected"),"Native action error message was lost.");
        }
        actionReplyMode=0;call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(ActionUnmarkedProbe),null)==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(176))
        {
            *(Services*)guard.Header=good;call.Services=(Services*)guard.Header;call.Operation=3;int before=actionCalls;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare IInputActionsGame") && ((ProbeState*)state)->Count==1100 && actionCalls==before,"Unmarked action getter touched protected tail.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("input actions independent named264/larger negotiation precedes construction; protected short/missing tail and undeclared access reject; unrelated poisoned tails stay unread; ASCII bounds, held/quick-tap/presence/other-entity absence and strict reply/error transfer are Tick-only");
    }
}
