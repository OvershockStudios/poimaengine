// SPDX-License-Identifier: Apache-2.0
// Independently declared transfer PODs; no native solver is simulated here.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;
[StructLayout(LayoutKind.Sequential)] struct OrientationQuaternion { public double X,Y,Z,W; }
[StructLayout(LayoutKind.Sequential)] struct OrientedIkCommand
{ public IkTransferCommand Point;public uint Present,Reserved;public OrientationQuaternion Rotation;public double Weight; }
[StructLayout(LayoutKind.Sequential)] struct OrientedIkState
{ public IkTransferState Point;public uint Present,Reserved;public OrientationQuaternion Rotation;public double Weight;public OrientationQuaternion Actual;public double Error; }
[StructLayout(LayoutKind.Sequential)] unsafe struct OrientedIkServices
{
    public StanceServices Stance;
    public delegate* unmanaged[Cdecl]<void*,Id*,uint,OrientedIkState*,void*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,OrientedIkCommand*,void*,int> Set;
}
[GameModule("poima-test-ik-orientation")]
public sealed class IkOrientationProbe:Game<ProbeState>,ITwoBoneIkOrientationGame
{
    public override void Initialize(ref ProbeState state){state.Count=2300;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        EntityId entity=new(11,40);var before=context.GetTwoBoneIkOrientation(entity,1) ?? throw new Exception("Oriented chain absent.");
        if(before.Point.Entity!=entity || before.Point.Slot!=1 || before.Point.Root!=0 || before.Point.Middle!=1 || before.Point.End!=2 ||
           before.EndOrientation is not { } orientation || orientation.Goal!=new IkEndOrientationGoal(new(0,0,0,1),.5) ||
           orientation.ActualRotation!=new Quaterniond(.5,.5,.5,.5) || orientation.AngularError!=2*Math.PI/3)
            throw new Exception("Oriented committed transfer differs.");
        bool denied=false;try{_=context.GetCharacterStance(entity);}catch(ArgumentException e){denied=e.Message.Contains("Declare ICharacterStanceGame");}
        if(!denied)throw new Exception("Oriented IK granted unrelated stance rights.");
        context.SetTwoBoneIkOrientation(new(entity,1,new(2,3,4),new(0,1,0),false,IkSpace.World,.25,new(new(0,0,0,-2),.25)));
        if(context.GetTwoBoneIkOrientation(entity,1)!=before)throw new Exception("Oriented staged write leaked into committed read.");
        if(context.GetTwoBoneIkOrientation(entity,2)!=null)throw new Exception("Absent chain not null.");
        var noGoal=context.GetTwoBoneIkOrientation(entity,3) ?? throw new Exception("Point-only chain absent.");
        if(noGoal.EndOrientation!=null)throw new Exception("Point-only goal fabricated.");
        context.SetTwoBoneIkOrientation(new(entity,1,default,default,true,IkSpace.Rig,0,new(new(0,0,0,2),0)));
        context.SetTwoBoneIkOrientation(new(entity,1,default,default,true,IkSpace.Rig,0,new(new(double.Epsilon,double.Epsilon,0,0),1)));
        foreach(var goal in new[]{new IkEndOrientationGoal(default,1),new(new(double.NaN,0,0,1),1),
            new(new(double.MaxValue,double.MaxValue,0,0),1),new(new(0,0,0,1),-.1),new(new(0,0,0,1),double.NaN),new(default,0)})
        {
            denied=false;try{context.SetTwoBoneIkOrientation(new(entity,1,default,default,true,IkSpace.Rig,0,goal));}catch(ArgumentException){denied=true;}
            if(!denied)throw new Exception("Invalid SDK orientation entered host.");
        }
        ++state.Count;
    }
}
[GameModule("poima-test-ik-orientation-constructor")]
public sealed class IkOrientationConstructorProbe:Game<ProbeState>,ITwoBoneIkOrientationGame
{
    public IkOrientationConstructorProbe(){throw new Exception("ORIENTATION CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){ }
    public override void Tick(ref ProbeState state,GameContext context){ }
}
[GameModule("poima-test-ik-orientation-unmarked")]
public sealed class IkOrientationUnmarkedProbe:Game<ProbeState>,ITwoBoneIkGame
{
    public override void Initialize(ref ProbeState state){state.Count=2400;}
    public override void Tick(ref ProbeState state,GameContext context){_=context.GetTwoBoneIkOrientation(new(11,40),1);++state.Count;}
}
[GameModule("poima-test-ik-orientation-all")]
public sealed class IkOrientationAllProbe:Game<ProbeState>,ITwoBoneIkOrientationGame,ICharacterStanceGame,ICameraLensGame,IRootMotionGame,
    IHitRegionsGame,IInputActionsGame,IPlayerPreferencesGame,IHierarchicalInstancesGame,INavigationGame,ICharacterInputGame,IMaskedAnimationGame
{
    public override void Initialize(ref ProbeState state){ }
    public override void Tick(ref ProbeState state,GameContext context){ }
}
static unsafe partial class Program
{
    static int orientationReads,orientationWrites,orientationMode;
    static readonly List<OrientedIkCommand> orientationCommands=[];
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ReadOrientedIk(void* context,Id* entity,uint slot,OrientedIkState* output,void* error)
    {
        ++orientationReads;
        if((nint)context!=0x1234 || output->Point.Version!=2 || output->Point.Bytes!=248 || output->Point.Reserved!=0 || output->Point.ReservedTail!=0 || output->Reserved!=0)badPayload=true;
        if(orientationMode==1)return LifecycleError(error);
        *output=new(){Point=new(){Version=2,Bytes=248}};
        if(slot!=2)output->Point=new(){Version=2,Bytes=248,Present=1,Slot=slot,Entity=*entity,Root=0,Middle=1,End=2,Enabled=1,Space=1,
            Weight=.5,Target=new(1,2,3),Pole=new(0,0,1),EndPosition=new(1,2,2),TargetError=1};
        if(slot==1){output->Present=1;output->Rotation.W=1;output->Weight=.5;output->Actual=new(){X=.5,Y=.5,Z=.5,W=.5};output->Error=2*Math.PI/3;}
        switch(orientationMode){
            case 2:output->Point.Version=1;break;case 3:output->Point.Bytes=247;break;case 4:output->Point.Bytes=256;break;
            case 5:output->Reserved=1;break;case 6:output->Present=2;break;case 7:output->Point.ReservedTail=1;break;
            case 8:output->Point.Entity.Low=41;break;case 9:output->Rotation.X=double.NaN;break;
            case 10:output->Rotation=default;break;case 11:output->Rotation.W=-1;break;case 12:output->Weight=0;break;
            case 13:output->Weight=1.1;break;case 14:output->Weight=double.NaN;break;case 15:output->Actual=default;break;
            case 16:output->Actual.X=double.NaN;break;case 17:output->Error=-1;break;case 18:output->Error=Math.PI+.1;break;
            case 19:output->Error=double.NaN;break;case 20:output->Present=0;break;case 21:output->Point.Present=0;break;
            case 22:if(slot==3)output->Actual.X=BitConverter.Int64BitsToDouble(long.MinValue);break;
            case 24:output->Actual=new(){X=-.5,Y=-.5,Z=-.5,W=-.5};break;
            case 25:output->Actual=new(){X=BitConverter.Int64BitsToDouble(long.MinValue),W=1};break;
            case 26:output->Rotation.W=1+4e-13;break;case 27:output->Actual=new(){W=1+4e-13};break;
            case 28:output->Rotation.X=BitConverter.Int64BitsToDouble(long.MinValue);break;
        }
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int WriteOrientedIk(void* context,OrientedIkCommand* command,void* error)
    {
        ++orientationWrites;orientationCommands.Add(*command);
        if((nint)context!=0x1234 || command->Point.Version!=2 || command->Point.Bytes!=144 || command->Point.Reserved!=0 || command->Reserved!=0)badPayload=true;
        if(orientationMode==23)return LifecycleError(error);return 0;
    }
    static JsonElement OrientationHost(uint bytes=352,bool orientation=true,bool point=true,uint epoch=7,bool all=false)
    {
        var names=new List<string>{"baseline_v7"};if(point)names.Add("animation_ik_v1");if(orientation)names.Add("animation_ik_end_orientation_v1");
        if(all)names.AddRange(["animation_inertial_v1","animation_layers_v1","character_input_v1","navigation_query_v1","hierarchical_instances_v1","player_preferences_v1","input_actions_v1","hit_regions_v1","animation_root_motion_v1","camera_lens_v1","character_stance_v1"]);
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void TwoBoneIkOrientationContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(OrientedIkCommand)==144 && sizeof(OrientedIkState)==248 && sizeof(OrientedIkServices)==352 &&
            Marshal.OffsetOf<OrientedIkCommand>(nameof(OrientedIkCommand.Present)).ToInt64()==96 && Marshal.OffsetOf<OrientedIkCommand>(nameof(OrientedIkCommand.Rotation)).ToInt64()==104 &&
            Marshal.OffsetOf<OrientedIkCommand>(nameof(OrientedIkCommand.Weight)).ToInt64()==136 && Marshal.OffsetOf<OrientedIkState>(nameof(OrientedIkState.Present)).ToInt64()==160 &&
            Marshal.OffsetOf<OrientedIkState>(nameof(OrientedIkState.Rotation)).ToInt64()==168 && Marshal.OffsetOf<OrientedIkState>(nameof(OrientedIkState.Weight)).ToInt64()==200 &&
            Marshal.OffsetOf<OrientedIkState>(nameof(OrientedIkState.Actual)).ToInt64()==208 && Marshal.OffsetOf<OrientedIkState>(nameof(OrientedIkState.Error)).ToInt64()==240 &&
            Marshal.OffsetOf<OrientedIkServices>(nameof(OrientedIkServices.Get)).ToInt64()==336 && Marshal.OffsetOf<OrientedIkServices>(nameof(OrientedIkServices.Set)).ToInt64()==344,"Independent orientation POD layout differs.");
        Check(typeof(ControlContext).GetMethod("GetTwoBoneIkOrientation")==null && typeof(ControlContext).GetMethod("SetTwoBoneIkOrientation")==null,"Orientation entered Control API.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,OrientationHost(176),OrientationHost(336),OrientationHost(351),OrientationHost(orientation:false),OrientationHost(point:false),OrientationHost(epoch:8)})
        {Check(LoadExtension(&call,typeof(IkOrientationConstructorProbe),host)!=0 && !Output(output).Contains("ORIENTATION CONSTRUCTOR REACHED"),"Invalid orientation host reached constructor.");NoExtensionModules(&call);}
        foreach(uint bytes in new[]{352u,360u})
        {Check(LoadExtension(&call,typeof(IkOrientationConstructorProbe),OrientationHost(bytes))!=0 && Output(output).Contains("ORIENTATION CONSTRUCTOR REACHED"),"Valid orientation host denied constructor.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(IkOrientationAllProbe),OrientationHost(all:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var required=doc.RootElement.GetProperty("requirements");Check(required.GetProperty("features").GetArrayLength()==14 && required.GetProperty("services_bytes").GetUInt32()==352,"All14feature union differs.");
         var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;Check(Convert.ToUInt32(parser.Invoke(null,[required.GetRawText()]))==8191,"Orientation generated requirement flags differ.");}
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(IkOrientationProbe),OrientationHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output))){call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();Check(doc.RootElement.GetProperty("requirements").GetProperty("features").EnumerateArray().Select(x=>x.GetString()).SequenceEqual(new[]{"animation_ik_end_orientation_v1","animation_ik_v1","baseline_v7"}),"Orientation grants unrelated requirements.");}
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        OrientedIkServices services=default;services.Stance.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline=good;
        new Span<byte>((byte*)&services+176,160).Fill(0xa5);
        services.Stance.Lens.Root.Ik.Get=&ReadIk;services.Stance.Lens.Root.Ik.Set=&WriteIk;services.Get=&ReadOrientedIk;services.Set=&WriteOrientedIk;
        services.Stance.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline.Bytes=352;
        orientationReads=orientationWrites=orientationMode=0;orientationCommands.Clear();call.Operation=3;
        foreach(int bytes in new[]{176,288,304,320,336,344,351})
        {using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && orientationReads==0 && orientationWrites==0,"Protected short orientation tail accessed.");}
        for(int missing=0;missing<2;++missing)
        {var absent=services;if(missing==0)absent.Get=null;else absent.Set=null;call.Services=&absent.Stance.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && orientationReads==0,"Missing orientation callback entered game.");}
        call.Services=&services.Stance.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==2301 && orientationReads==4 && orientationWrites==3 && !badPayload,Output(output));
        var first=orientationCommands[0];Check(first.Point.Entity.High==11 && first.Point.Entity.Low==40 && first.Point.Slot==1 && first.Point.Enabled==0 && first.Point.Space==0 && first.Point.Weight==.25 && first.Point.Target==new Vector3d(2,3,4) && first.Point.Pole==new Vector3d(0,1,0) && first.Present==1 && first.Rotation.W==1 && first.Weight==.25,"Complete normalized orientation command differs.");
        var zero=orientationCommands[1];Check(zero.Present==0 && zero.Rotation.X==0 && zero.Rotation.Y==0 && zero.Rotation.Z==0 && zero.Rotation.W==0 && zero.Weight==0,"Zero orientation weight not absent.");
        var tiny=orientationCommands[2];Check(tiny.Present==1 && Math.Abs(tiny.Rotation.X-Math.Sqrt(.5))<1e-14 && tiny.Rotation.X==tiny.Rotation.Y && tiny.Rotation.Z==0 && tiny.Rotation.W==0,"Subnormal quaternion normalization differs.");
        for(int mode=1;mode<=28;++mode)
        {orientationMode=mode;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==2301,"Malformed/error oriented transfer accepted: "+mode);if(mode is 1 or 23)Check(Output(output).Contains("lifecycle rejected"),"Oriented native error text lost.");}
        orientationMode=0;call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(IkOrientationUnmarkedProbe),IkHost())==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(288))
        {*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=288;var prefix=(IkServices*)guard.Header;prefix->Get=&ReadIk;prefix->Set=&WriteIk;call.Services=(Services*)guard.Header;call.Operation=3;int before=orientationReads;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare ITwoBoneIkOrientationGame") && orientationReads==before,"Unmarked game accessed orientation tail.");}
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("oriented IK352/CMD144/STATE248 independent PODs,14feature union, pre-construction and protected-tail denial");
        checks.Add("orientation full replacements, staged invisibility, canonical missing/null/zero goals, subnormal normalization and strict malformed transfers");
    }
}
