// SPDX-License-Identifier: Apache-2.0
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;
[StructLayout(LayoutKind.Sequential)] struct StanceCommand
{ public uint Version,Bytes;public Id Entity;public uint Crouch,Reserved; }
[StructLayout(LayoutKind.Sequential)] struct StanceState
{
    public uint Version,Bytes;public Id Entity;public uint Present,Flags,Ground;
    public float Radius,StandingHeight,CrouchedHeight,Height,Speed,Drop;public uint Reserved;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct StanceServices
{
    public LensServices Lens;
    public delegate* unmanaged[Cdecl]<void*,Id*,StanceState*,void*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,StanceCommand*,void*,int> Set;
}
[GameModule("poima-test-stance")]
public sealed class StanceProbeGame:Game<ProbeState>,ICharacterStanceGame
{
    public override void Initialize(ref ProbeState state){state.Count=2100;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        bool denied=false;try{_=context.GetCameraLens(new(11,50));}catch(ArgumentException e){denied=e.Message.Contains("Declare ICameraLensGame");}
        if(!denied)throw new Exception("Stance marker granted camera lenses.");
        denied=false;try{context.SetCharacterInput(new(11,50),0,0);}catch(ArgumentException e){denied=e.Message.Contains("Declare ICharacterInputGame");}
        if(!denied)throw new Exception("Stance marker granted character input.");
        EntityId id=new(11,50);var before=context.GetCharacterStance(id) ?? throw new Exception("Missing stance.");
        if(before.Entity!=id || !before.Configured || !before.Crouched || !before.RequestedCrouch || before.StandBlocked ||
           before.GroundState!=CharacterGroundState.OnGround || before.Radius!=.25f || before.StandingHeight!=2 ||
           before.CrouchedHeight!=1 || before.Height!=1 || before.Speed!=2 || before.CameraDrop!=.5f)throw new Exception("Committed stance transfer differs.");
        context.SetCharacterStance(id,false);
        if(context.GetCharacterStance(id)!=before)throw new Exception("Staged stance exposed to getter.");
        if(context.GetCharacterStance(new(11,51))!=null)throw new Exception("Live noncontroller was not null.");
        var legacy=context.GetCharacterStance(new(11,52)) ?? throw new Exception("Legacy controller absent.");
        if(legacy.Configured || legacy.Crouched || legacy.RequestedCrouch || legacy.StandBlocked ||
           legacy.Height!=2 || legacy.CrouchedHeight!=legacy.StandingHeight || legacy.Speed!=4 || legacy.CameraDrop!=0)throw new Exception("Legacy neutral state differs.");
        bool invalid=false;try{context.SetCharacterStance(default,true);}catch(ArgumentException){invalid=true;}
        if(!invalid)throw new Exception("Empty SDK stance command reached host.");
        ++state.Count;
    }
}
[GameModule("poima-test-stance-constructor")]
public sealed class StanceConstructorProbe:Game<ProbeState>,ICharacterStanceGame
{
    public StanceConstructorProbe(){throw new Exception("STANCE CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){ }
    public override void Tick(ref ProbeState state,GameContext context){ }
}
[GameModule("poima-test-stance-all")]
public sealed class StanceAllFeaturesProbe:Game<ProbeState>,ICharacterStanceGame,ICameraLensGame,IRootMotionGame,ITwoBoneIkGame,IHitRegionsGame,
    IInputActionsGame,IPlayerPreferencesGame,IHierarchicalInstancesGame,INavigationGame,ICharacterInputGame,IMaskedAnimationGame
{
    public override void Initialize(ref ProbeState state){ }
    public override void Tick(ref ProbeState state,GameContext context){ }
}
[GameModule("poima-test-stance-unmarked")]
public sealed class StanceUnmarkedProbe:Game<ProbeState>
{
    public override void Initialize(ref ProbeState state){state.Count=2200;}
    public override void Tick(ref ProbeState state,GameContext context){_=context.GetCharacterStance(new(11,50));++state.Count;}
}
[GameModule("poima-test-stance-ground")]
public sealed class StanceGroundProbe:Game<ProbeState>,ICharacterStanceGame
{
    public override void Initialize(ref ProbeState state){state.Count=2300;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        var value=context.GetCharacterStance(new(11,50)) ?? throw new Exception("Ground stance absent.");
        if(!value.Configured || !value.Crouched || value.RequestedCrouch || !value.StandBlocked ||
           (uint)value.GroundState!=(uint)(state.Count-2300) || value.CameraDrop!=.5f)throw new Exception("Blocked ground state differs.");
        ++state.Count;
    }
}
static unsafe partial class Program
{
    static int stanceReads,stanceWrites,stanceMode;
    static uint stanceGround,stanceFlags=11;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ReadStance(void* context,Id* entity,StanceState* value,void* error)
    {
        ++stanceReads;if((nint)context!=0x1234 || entity->High!=11 || value->Version!=1 || value->Bytes!=64)badPayload=true;
        *value=new(){Version=1,Bytes=64};
        if(entity->Low!=51)*value=new(){Version=1,Bytes=64,Entity=*entity,Present=1,Flags=11,Ground=0,
            Radius=.25f,StandingHeight=2,CrouchedHeight=1,Height=1,Speed=2,Drop=.5f};
        if(entity->Low==50){value->Ground=stanceGround;value->Flags=stanceFlags;}
        if(entity->Low==52){value->Flags=0;value->CrouchedHeight=value->Height=2;value->Speed=4;value->Drop=0;}
        switch(stanceMode)
        {
            case 1:return LifecycleError(error);
            case 2:value->Version=2;break;
            case 3:value->Bytes=63;break;
            case 4:value->Present=2;break;
            case 5:value->Flags=16;break;
            case 6:value->Reserved=1;break;
            case 7:value->Ground=4;break;
            case 8:value->Entity.Low=99;break;
            case 9:value->Radius=float.NaN;break;
            case 10:value->StandingHeight=0;break;
            case 11:value->CrouchedHeight=3;break;
            case 12:value->Height=2;break;
            case 13:value->Speed=float.PositiveInfinity;break;
            case 14:value->Drop=-1;break;
            case 15:value->Flags=13;break; // blocked without actual crouch
            case 16:value->Present=0;break; // absent retains nonzero payload
            case 17:value->Flags=0;break; // unconfigured retains crouch values
            case 18:value->Flags=8;value->Height=2;break; // standing retains camera drop
            case 20:value->Flags=9;value->Height=2;value->Drop=0;break; // committed crouch intent without shape
            case 21:value->Flags=10;break; // standing intent/crouched shape without blocked flag
            case 22:value->Radius=.01f;break;
            case 23:value->Radius=2.1f;break;
            case 24:value->Flags=0;value->StandingHeight=value->CrouchedHeight=value->Height=.5f;value->Drop=0;break; // standing capsule diameter
            case 25:value->StandingHeight=5;break;
            case 26:value->CrouchedHeight=value->Height=.5f;break; // configured crouched capsule diameter
            case 27:value->CrouchedHeight=value->Height=2;value->Drop=0;break; // configured crouch must shrink
            case 28:value->Speed=0;break;
            case 29:value->Speed=31;break;
            case 30:value->Drop=1.01f;break; // actual drop exceeds profile height difference
            case 31:value->Flags=8;value->Height=2;value->CrouchedHeight=.5f;value->Drop=0;break; // standing configured capsule still validates profile
            case 32:value->Flags=8;value->Height=value->CrouchedHeight=2;value->Drop=0;break; // standing configured profile must shrink

        }
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int WriteStance(void* context,StanceCommand* value,void* error)
    {
        ++stanceWrites;if((nint)context!=0x1234 || value->Version!=1 || value->Bytes!=32 || value->Entity.High!=11 ||
            value->Entity.Low!=50 || value->Crouch!=0 || value->Reserved!=0)badPayload=true;
        if(stanceMode==19)return LifecycleError(error);return 0;
    }
    static JsonElement StanceHost(uint bytes=336,bool stance=true,bool all=false,uint epoch=7)
    {
        var names=new List<string>{"baseline_v7"};if(stance)names.Add("character_stance_v1");
        if(all)names.AddRange(["camera_lens_v1","animation_root_motion_v1","animation_ik_v1","animation_inertial_v1","animation_layers_v1",
            "character_input_v1","navigation_query_v1","hierarchical_instances_v1","player_preferences_v1","input_actions_v1","hit_regions_v1"]);
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void CharacterStanceContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(StanceCommand)==32 && sizeof(StanceState)==64 && sizeof(StanceServices)==336 &&
            Marshal.OffsetOf<StanceServices>(nameof(StanceServices.Get)).ToInt64()==320 && Marshal.OffsetOf<StanceServices>(nameof(StanceServices.Set)).ToInt64()==328 &&
            Marshal.OffsetOf<StanceState>(nameof(StanceState.Entity)).ToInt64()==8 && Marshal.OffsetOf<StanceState>(nameof(StanceState.Present)).ToInt64()==24 &&
            Marshal.OffsetOf<StanceState>(nameof(StanceState.Flags)).ToInt64()==28 && Marshal.OffsetOf<StanceState>(nameof(StanceState.Ground)).ToInt64()==32 &&
            Marshal.OffsetOf<StanceState>(nameof(StanceState.Radius)).ToInt64()==36 && Marshal.OffsetOf<StanceState>(nameof(StanceState.Drop)).ToInt64()==56 &&
            Marshal.OffsetOf<StanceState>(nameof(StanceState.Reserved)).ToInt64()==60,"Independent stance layouts differ.");
        Check(typeof(ControlContext).GetMethod("GetCharacterStance")==null && typeof(ControlContext).GetMethod("SetCharacterStance")==null,"Stance leaked into Control.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,StanceHost(176),StanceHost(320),StanceHost(328),StanceHost(335),StanceHost(stance:false),StanceHost(epoch:8)})
        {Check(LoadExtension(&call,typeof(StanceConstructorProbe),host)!=0 && !Output(output).Contains("STANCE CONSTRUCTOR REACHED"),"Invalid stance host reached constructor.");NoExtensionModules(&call);}
        foreach(uint bytes in new[]{336u,352u})
        {Check(LoadExtension(&call,typeof(StanceConstructorProbe),StanceHost(bytes))!=0 && Output(output).Contains("STANCE CONSTRUCTOR REACHED"),"Valid stance host denied constructor.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(StanceAllFeaturesProbe),StanceHost(all:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var requirements=doc.RootElement.GetProperty("requirements");
            Check(requirements.GetProperty("services_bytes").GetUInt32()==336 && requirements.GetProperty("features").GetArrayLength()==13,"Stance union lost prefix/names.");
            var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;
            Check(Convert.ToUInt32(parser.Invoke(null,[requirements.GetRawText()]))==4095,"Stance feature flags differ.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(StanceProbeGame),StanceHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        StanceServices services=default;services.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline=good;
        services.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline.Bytes=336;
        services.Get=&ReadStance;services.Set=&WriteStance;new Span<byte>((byte*)&services+176,144).Fill(0xa5);
        stanceReads=stanceWrites=stanceMode=0;call.Operation=3;
        foreach(int bytes in new[]{176,192,208,216,224,232,256,264,272,288,304,320,328,335})
        {
            using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==2100 && stanceReads==0 && stanceWrites==0,"Protected short stance tail accessed.");
        }
        for(int missing=0;missing<2;++missing)
        {
            var absent=services;if(missing==0)absent.Get=null;else absent.Set=null;
            call.Services=&absent.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && stanceReads==0 && stanceWrites==0,"Missing stance callback entered game.");
        }
        call.Services=&services.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==2101 && stanceReads==4 && stanceWrites==1 && !badPayload,Output(output));
        for(int mode=1;mode<=32;++mode){stanceMode=mode;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==2101,"Malformed stance failed rollback: "+mode);}
        stanceMode=0;call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(StanceGroundProbe),StanceHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        call.Services=&services.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
        stanceFlags=14;call.Operation=3;
        for(uint ground=0;ground<4;++ground){stanceGround=ground;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==2301+(int)ground,Output(output));}
        stanceGround=0;stanceFlags=11;call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(StanceUnmarkedProbe),null)==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(176))
        {*(Services*)guard.Header=good;call.Services=(Services*)guard.Header;call.Operation=3;int reads=stanceReads;
         Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare ICharacterStanceGame") && ((ProbeState*)state)->Count==2200 && stanceReads==reads,"Unmarked stance accessed protected tail.");}
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("independent stance336/CMD32/STATE64 layouts,13feature union and constructor/short-tail denial");
        checks.Add("stance preTick committed reads, staged invisibility, canonical absent/legacy replies and no Control rights");
        checks.Add("positive blocked standing retry and all four native ground values transfer exactly");
        checks.Add("malformed stance transfers and native errors preserve managed state; marker grants no camera/character-input rights");
    }
}
