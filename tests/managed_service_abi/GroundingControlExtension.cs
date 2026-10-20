// SPDX-License-Identifier: Apache-2.0
// Independent wire declarations. These callbacks test SDK transfer guards;
// actual physics, automatic ownership and rollback belong to native consumers.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)] struct GroundingCommand
{ public uint Version,Bytes;public Id Entity;public uint Slot,Enabled;public double Weight;public ulong Reserved; }
[StructLayout(LayoutKind.Sequential)] struct GroundingState
{ public uint Version,Bytes,Present,Reserved;public Id Entity;public uint Slot,Enabled;public double Weight; }
[StructLayout(LayoutKind.Sequential)] unsafe struct GroundingServices
{
    public OrientedIkServices Orientation;
    public delegate* unmanaged[Cdecl]<void*,Id*,uint,GroundingState*,void*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,GroundingCommand*,void*,int> Set;
}
[GameModule("poima-test-grounding-control")]
public sealed class GroundingProbe:Game<ProbeState>,IGroundingControlGame
{
    public override void Initialize(ref ProbeState state){state.Count=2600;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        EntityId entity=new(11,40);var before=context.GetGroundingControl(entity,1);
        if(before!=new GroundingControl(true,.75) || context.GetGroundingControl(entity,2)!=null)
            throw new Exception("Committed grounding control/absence differs.");
        context.SetGroundingControl(new(entity,1,false,.25));
        if(context.GetGroundingControl(entity,1)!=before)throw new Exception("Staged grounding control leaked into read.");
        foreach(uint slot in new[]{0u,9u})
        {
            bool denied=false;try{_=context.GetGroundingControl(entity,slot);}catch(ArgumentException){denied=true;}
            if(!denied)throw new Exception("Invalid SDK grounding slot entered host.");
        }
        foreach(double weight in new[]{-.1,1.1,double.NaN,double.PositiveInfinity})
        {
            bool denied=false;try{context.SetGroundingControl(new(entity,1,true,weight));}catch(ArgumentException){denied=true;}
            if(!denied)throw new Exception("Invalid SDK grounding weight entered host.");
        }
        bool unrelated=false;try{_=context.GetCharacterStance(entity);}catch(ArgumentException e){unrelated=e.Message.Contains("Declare ICharacterStanceGame");}
        if(!unrelated)throw new Exception("Grounding granted unrelated stance rights.");
        ++state.Count;
    }
}
[GameModule("poima-test-grounding-constructor")]
public sealed class GroundingConstructorProbe:Game<ProbeState>,IGroundingControlGame
{
    public GroundingConstructorProbe(){throw new Exception("GROUNDING CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){ }
    public override void Tick(ref ProbeState state,GameContext context){ }
}
[GameModule("poima-test-grounding-unmarked")]
public sealed class GroundingUnmarkedProbe:Game<ProbeState>,ITwoBoneIkOrientationGame
{
    public override void Initialize(ref ProbeState state){state.Count=2700;}
    public override void Tick(ref ProbeState state,GameContext context){_=context.GetGroundingControl(new(11,40),1);++state.Count;}
}
[GameModule("poima-test-grounding-all")]
public sealed class GroundingAllProbe:Game<ProbeState>,IGroundingControlGame,ICharacterStanceGame,ICameraLensGame,IRootMotionGame,
    IHitRegionsGame,IInputActionsGame,IPlayerPreferencesGame,IHierarchicalInstancesGame,INavigationGame,ICharacterInputGame,IMaskedAnimationGame
{
    public override void Initialize(ref ProbeState state){ }
    public override void Tick(ref ProbeState state,GameContext context){ }
}
static unsafe partial class Program
{
    static int groundingReads,groundingWrites,groundingMode;
    static GroundingCommand groundingCommand;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ReadGrounding(void* context,Id* entity,uint slot,GroundingState* output,void* error)
    {
        ++groundingReads;
        if((nint)context!=0x1234 || output->Version!=1 || output->Bytes!=48 || output->Reserved!=0)badPayload=true;
        if(groundingMode==1)return LifecycleError(error);
        *output=new(){Version=1,Bytes=48};
        if(slot!=2)*output=new(){Version=1,Bytes=48,Present=1,Entity=*entity,Slot=slot,Enabled=1,Weight=.75};
        switch(groundingMode)
        {
            case 2:output->Version=2;break;case 3:output->Bytes=47;break;case 4:output->Bytes=49;break;
            case 5:output->Reserved=1;break;case 6:output->Present=2;break;case 7:output->Entity.Low=41;break;
            case 8:output->Slot=8;break;case 9:output->Enabled=2;break;case 10:output->Weight=-.1;break;
            case 11:output->Weight=1.1;break;case 12:output->Weight=double.NaN;break;
            case 13:output->Present=0;break;
            case 14:*output=new(){Version=1,Bytes=48,Weight=BitConverter.Int64BitsToDouble(long.MinValue)};break;
            case 15:*output=new(){Version=1,Bytes=48,Entity=new(){Low=1}};break;
            case 16:*output=new(){Version=1,Bytes=48,Slot=1};break;
        }
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int WriteGrounding(void* context,GroundingCommand* command,void* error)
    {
        ++groundingWrites;groundingCommand=*command;
        if((nint)context!=0x1234 || command->Version!=1 || command->Bytes!=48 || command->Reserved!=0)badPayload=true;
        return groundingMode==17?LifecycleError(error):0;
    }
    static JsonElement GroundingHost(uint bytes=368,bool grounding=true,bool orientation=true,bool point=true,uint epoch=7,bool all=false)
    {
        var names=new List<string>{"baseline_v7"};if(point)names.Add("animation_ik_v1");if(orientation)names.Add("animation_ik_end_orientation_v1");if(grounding)names.Add("animation_grounding_control_v1");
        if(all)names.AddRange(["animation_inertial_v1","animation_layers_v1","character_input_v1","navigation_query_v1","hierarchical_instances_v1","player_preferences_v1","input_actions_v1","hit_regions_v1","animation_root_motion_v1","camera_lens_v1","character_stance_v1"]);
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void GroundingControlContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(GroundingCommand)==48 && sizeof(GroundingState)==48 && sizeof(GroundingServices)==368 &&
            Marshal.OffsetOf<GroundingCommand>(nameof(GroundingCommand.Entity)).ToInt64()==8 && Marshal.OffsetOf<GroundingCommand>(nameof(GroundingCommand.Slot)).ToInt64()==24 &&
            Marshal.OffsetOf<GroundingCommand>(nameof(GroundingCommand.Enabled)).ToInt64()==28 && Marshal.OffsetOf<GroundingCommand>(nameof(GroundingCommand.Weight)).ToInt64()==32 &&
            Marshal.OffsetOf<GroundingCommand>(nameof(GroundingCommand.Reserved)).ToInt64()==40 && Marshal.OffsetOf<GroundingState>(nameof(GroundingState.Present)).ToInt64()==8 &&
            Marshal.OffsetOf<GroundingState>(nameof(GroundingState.Reserved)).ToInt64()==12 && Marshal.OffsetOf<GroundingState>(nameof(GroundingState.Entity)).ToInt64()==16 &&
            Marshal.OffsetOf<GroundingState>(nameof(GroundingState.Slot)).ToInt64()==32 && Marshal.OffsetOf<GroundingState>(nameof(GroundingState.Enabled)).ToInt64()==36 &&
            Marshal.OffsetOf<GroundingState>(nameof(GroundingState.Weight)).ToInt64()==40 && Marshal.OffsetOf<GroundingServices>(nameof(GroundingServices.Get)).ToInt64()==352 &&
            Marshal.OffsetOf<GroundingServices>(nameof(GroundingServices.Set)).ToInt64()==360,"Independent grounding POD layout differs.");
        Check(typeof(ControlContext).GetMethod("GetGroundingControl")==null && typeof(ControlContext).GetMethod("SetGroundingControl")==null,"Grounding entered Control API.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,GroundingHost(352),GroundingHost(367),GroundingHost(grounding:false),GroundingHost(orientation:false),GroundingHost(point:false),GroundingHost(epoch:8)})
        {Check(LoadExtension(&call,typeof(GroundingConstructorProbe),host)!=0&&!Output(output).Contains("GROUNDING CONSTRUCTOR REACHED"),"Invalid grounding host reached constructor.");NoExtensionModules(&call);}
        foreach(uint bytes in new[]{368u,376u})
        {Check(LoadExtension(&call,typeof(GroundingConstructorProbe),GroundingHost(bytes))!=0&&Output(output).Contains("GROUNDING CONSTRUCTOR REACHED"),"Valid grounding host denied constructor.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(GroundingAllProbe),GroundingHost(all:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var required=doc.RootElement.GetProperty("requirements");
            Check(required.GetProperty("features").GetArrayLength()==15&&required.GetProperty("services_bytes").GetUInt32()==368,"All15 grounding union differs.");
            var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;
            Check(Convert.ToUInt32(parser.Invoke(null,[required.GetRawText()]))==16383,"Grounding generated declaration flags differ.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(GroundingProbe),GroundingHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();Check(doc.RootElement.GetProperty("requirements").GetProperty("features").EnumerateArray().Select(x=>x.GetString()).SequenceEqual(new[]{"animation_grounding_control_v1","animation_ik_end_orientation_v1","animation_ik_v1","baseline_v7"}),"Grounding grants unrelated requirements.");}
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        GroundingServices services=default;
        services.Orientation.Stance.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline=good;
        new Span<byte>((byte*)&services+176,176).Fill(0xa5);
        services.Orientation.Stance.Lens.Root.Ik.Get=&ReadIk;services.Orientation.Stance.Lens.Root.Ik.Set=&WriteIk;
        services.Orientation.Get=&ReadOrientedIk;services.Orientation.Set=&WriteOrientedIk;services.Get=&ReadGrounding;services.Set=&WriteGrounding;
        var baseline=&services.Orientation.Stance.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;baseline->Bytes=368;
        groundingReads=groundingWrites=groundingMode=0;call.Operation=3;
        foreach(int bytes in new[]{176,288,336,352,360,367})
        {using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0&&groundingReads==0&&groundingWrites==0,"Protected short grounding tail accessed.");}
        for(int missing=0;missing<2;++missing)
        {var absent=services;if(missing==0)absent.Get=null;else absent.Set=null;call.Services=&absent.Orientation.Stance.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0&&groundingReads==0,"Missing grounding callback entered game.");}
        call.Services=baseline;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0&&((ProbeState*)state)->Count==2601&&groundingReads==3&&groundingWrites==1&&!badPayload,Output(output));
        Check(groundingCommand.Entity.High==11&&groundingCommand.Entity.Low==40&&groundingCommand.Slot==1&&groundingCommand.Enabled==0&&groundingCommand.Weight==.25&&groundingCommand.Reserved==0,"Grounding full command payload differs.");
        for(int mode=1;mode<=17;++mode)
        {groundingMode=mode;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0&&((ProbeState*)state)->Count==2601,"Malformed/error grounding transfer accepted: "+mode);if(mode is 1 or 17)Check(Output(output).Contains("lifecycle rejected"),"Grounding native error text lost.");}
        groundingMode=0;call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(GroundingUnmarkedProbe),OrientationHost())==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(352))
        {*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=352;var prefix=(OrientedIkServices*)guard.Header;prefix->Stance.Lens.Root.Ik.Get=&ReadIk;prefix->Stance.Lens.Root.Ik.Set=&WriteIk;prefix->Get=&ReadOrientedIk;prefix->Set=&WriteOrientedIk;call.Services=(Services*)guard.Header;call.Operation=3;int reads=groundingReads;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0&&Output(output).Contains("Declare IGroundingControlGame")&&groundingReads==reads,"Unmarked orientation game accessed grounding tail.");}
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("grounding368/CMD48/STATE48 independent PODs,15feature union, protected short-tail and pre-construction negotiation");
        checks.Add("requested grounding full replacement/staged invisibility, canonical absence, strict malformed response and retained352 denial");
    }
}
