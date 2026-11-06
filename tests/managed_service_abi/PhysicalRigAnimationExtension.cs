// SPDX-License-Identifier: Apache-2.0
// Independent wire declarations and synthetic callbacks qualify SDK/bridge
// transfer only. Animation sampling, physical response and saves need native evidence.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;
[StructLayout(LayoutKind.Sequential)] unsafe struct PhysicalAnimationSourceWire
{
    public uint Version,Bytes,Present,Flags;public Id Entity;public uint JointBytes,ReservedName;public fixed byte Joint[64];
    public uint Source,Policy;public ulong Tick;public fixed double Requested[4];public fixed double Target[4];
    public double Correction,Condition;public ulong ReservedTail;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct PhysicalAnimationSourceCommandWire
{
    public uint Version,Bytes;public Id Entity;public byte* Joint;public uint JointBytes,Reserved;
    public uint Source,ReservedSource;public double Strength;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct PhysicalAnimationServicesWire
{
    public DriveServicesWire Drive;
    public delegate* unmanaged[Cdecl]<void*,Id*,ExtendedAnimation*,void*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,Id*,uint,LayerAnimation*,void*,int> LayerGet;
    public delegate* unmanaged[Cdecl]<void*,LayerCommand*,void*,int> Set;
    public delegate* unmanaged[Cdecl]<void*,RateCommand*,void*,int> RateSet;
    public delegate* unmanaged[Cdecl]<void*,DriveRequestWire*,PhysicalAnimationSourceWire*,void*,int> SourceGet;
    public delegate* unmanaged[Cdecl]<void*,PhysicalAnimationSourceCommandWire*,void*,int> SourceSet;
}
[GameModule("poima-test-physical-animation")]
public sealed class PhysicalAnimationProbe:Game<ProbeState>,IPhysicalRigAnimationGame
{
    public override void Initialize(ref ProbeState state){state.Count=5100;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        EntityId entity=new(11,90);
        var playback=context.GetPhysicalRigAnimation(entity) ?? throw new Exception("Target playback absent.");
        if(playback.Mode!=AnimationTransitionMode.Inertial || playback.Progress!=.75 || playback.State.Clip!=3 || playback.State.Time!=.125 || playback.State.Speed!=2)
            throw new Exception("Target playback wire differs.");
        if(context.GetPhysicalRigAnimation(new(11,91))!=null)throw new Exception("Absent target playback has payload.");
        var layer=context.GetPhysicalRigAnimationLayer(entity,1) ?? throw new Exception("Target layer absent.");
        if(layer.Slot!=1 || layer.Mode!=AnimationLayerMode.Additive || layer.MaskNodes!=2 || layer.Weight!=.5 || layer.TargetWeight!=1 ||
            layer.Playback!=playback || layer.WeightTransition is not {} ramp || ramp.StartTick!=113 || ramp.DurationTicks!=20 || ramp.ElapsedTicks!=10 || ramp.Source!=0 || ramp.Target!=1)
            throw new Exception("Target layer wire differs.");
        if(context.GetPhysicalRigAnimationLayer(entity,2)!=null)throw new Exception("Absent target layer has payload.");
        var source=context.GetPhysicalRigDriveSource(entity,"spine") ?? throw new Exception("Target source absent.");
        if(source.Entity!=entity || source.Joint!="spine" || source.Source!=PhysicalRigDriveSource.Animation || source.LimitPolicy!=PhysicalRigTargetLimitPolicy.Fit || source.SampledTick!=123 ||
            source.Sample!=new PhysicalRigAnimationTarget(new(0,0,0,-1),new(0,0,0,-1),0,1,false,false,false))
            throw new Exception("Source wire differs or quaternion sign changed.");
        var manual=context.GetPhysicalRigDriveSource(entity,"manual") ?? throw new Exception("Manual source absent.");
        if(manual.Source!=PhysicalRigDriveSource.Manual || manual.LimitPolicy!=PhysicalRigTargetLimitPolicy.Fit || manual.SampledTick!=123 || manual.Sample!=null)
            throw new Exception("Manual source invented an animation sample.");
        if(context.GetPhysicalRigDriveSource(entity,"undriven")!=null)throw new Exception("Unauthored target source has payload.");
        context.SetPhysicalRigAnimation(entity,null,AnimationTransitionMode.Inertial,time:0,speed:.5,loop:false,playing:false,blendTicks:12);
        context.SetPhysicalRigAnimationLayer(entity,2,3,.7,AnimationTransitionMode.Crossfade,.25,1.5,true,true,15,19);
        context.SetPhysicalRigAnimationSpeed(entity,0);context.SetPhysicalRigAnimationLayerSpeed(entity,2,8);
        context.SetPhysicalRigDriveSource(entity,"spine",PhysicalRigDriveSource.Manual,0);
        context.SetPhysicalRigDriveSource(entity,"spine",PhysicalRigDriveSource.Animation,.8);
        if(context.GetPhysicalRigAnimation(entity)!=playback || context.GetPhysicalRigAnimationLayer(entity,1)!=layer || context.GetPhysicalRigDriveSource(entity,"spine")!=source)
            throw new Exception("Staged target writes leaked into committed reads.");
        foreach(double value in new[]{double.NaN,double.PositiveInfinity,-.01,8.01})
        {bool failed=false;try{context.SetPhysicalRigAnimationSpeed(entity,value);}catch(ArgumentException){failed=true;}if(!failed)throw new Exception("Invalid target rate reached host.");}
        foreach(uint slot in new[]{0u,5u})
        {
            bool failed=false;try{_=context.GetPhysicalRigAnimationLayer(entity,slot);}catch(ArgumentException){failed=true;}if(!failed)throw new Exception("Invalid target query slot reached host.");
            failed=false;try{context.SetPhysicalRigAnimationLayer(entity,slot,1,.5);}catch(ArgumentException){failed=true;}if(!failed)throw new Exception("Invalid target layer slot reached host.");
            failed=false;try{context.SetPhysicalRigAnimationLayerSpeed(entity,slot,1);}catch(ArgumentException){failed=true;}if(!failed)throw new Exception("Invalid target rate slot reached host.");
        }
        foreach(string name in new[]{"","a/b","é",new string('x',65)})
        {bool failed=false;try{context.SetPhysicalRigDriveSource(entity,name,PhysicalRigDriveSource.Animation,1);}catch(ArgumentException){failed=true;}if(!failed)throw new Exception("Invalid source name reached host.");}
        foreach(double strength in new[]{double.NaN,double.NegativeInfinity,-.01,1.01})
        {bool failed=false;try{context.SetPhysicalRigDriveSource(entity,"spine",PhysicalRigDriveSource.Manual,strength);}catch(ArgumentException){failed=true;}if(!failed)throw new Exception("Invalid source strength reached host.");}
        bool rejected=false;try{context.SetPhysicalRigDriveSource(entity,"spine",(PhysicalRigDriveSource)2,1);}catch(ArgumentException){rejected=true;}if(!rejected)throw new Exception("Invalid source enum reached host.");
        rejected=false;try{context.SetPhysicalRigAnimation(entity,-1);}catch(ArgumentException){rejected=true;}if(!rejected)throw new Exception("Negative target clip reached host.");
        rejected=false;try{context.SetPhysicalRigAnimation(entity,1,(AnimationTransitionMode)2);}catch(ArgumentException){rejected=true;}if(!rejected)throw new Exception("Invalid target mode reached host.");
        rejected=false;try{context.SetPhysicalRigAnimation(entity,1,time:double.NaN);}catch(ArgumentException){rejected=true;}if(!rejected)throw new Exception("Invalid target time reached host.");
        rejected=false;try{context.SetPhysicalRigAnimation(entity,1,blendTicks:3601);}catch(ArgumentException){rejected=true;}if(!rejected)throw new Exception("Invalid target fade reached host.");
        rejected=false;try{context.SetPhysicalRigAnimationLayer(entity,1,1,1.01);}catch(ArgumentException){rejected=true;}if(!rejected)throw new Exception("Invalid target weight reached host.");
        rejected=false;try{context.SetPhysicalRigAnimationLayer(entity,1,1,.5,weightBlendTicks:3601);}catch(ArgumentException){rejected=true;}if(!rejected)throw new Exception("Invalid target weight fade reached host.");
        rejected=false;try{_=context.GetPhysicalRigAnimation(default);}catch(ArgumentException){rejected=true;}if(!rejected)throw new Exception("Absent target entity reached host.");
        rejected=false;try{_=context.GetAnimationExtended(entity);}catch(ArgumentException e){rejected=e.Message.Contains("Declare IInertialAnimationGame");}if(!rejected)throw new Exception("Physical animation granted ordinary animation rights.");
        rejected=false;try{_=context.GetAnimationLayer(entity,1);}catch(ArgumentException e){rejected=e.Message.Contains("Declare IMaskedAnimationGame");}if(!rejected)throw new Exception("Physical animation granted ordinary layer rights.");
        rejected=false;try{context.SetAnimationSpeed(entity,1);}catch(ArgumentException e){rejected=e.Message.Contains("Declare IAnimationRateGame");}if(!rejected)throw new Exception("Physical animation granted ordinary rate rights.");
        ++state.Count;
    }
}
[GameModule("poima-test-physical-animation-constructor")]
public sealed class PhysicalAnimationConstructorProbe:Game<ProbeState>,IPhysicalRigAnimationGame
{public PhysicalAnimationConstructorProbe(){throw new Exception("PHYSICAL ANIMATION CONSTRUCTOR REACHED");}public override void Initialize(ref ProbeState state){}public override void Tick(ref ProbeState state,GameContext context){}}
[GameModule("poima-test-physical-animation-unmarked")]
public sealed class PhysicalAnimationUnmarkedProbe:Game<ProbeState>,IPhysicalRigDriveGame
{public override void Initialize(ref ProbeState state){state.Count=5200;}public override void Tick(ref ProbeState state,GameContext context){_=context.GetPhysicalRigAnimation(new(11,90));++state.Count;}}
[GameModule("poima-test-physical-animation-all")]
public sealed class PhysicalAnimationAllProbe:Game<ProbeState>,IPhysicalRigAnimationGame,ICapsuleCastGame,ICameraPoseGame,IAnimationRateGame,IGroundingControlGame,
    ICharacterStanceGame,ICameraLensGame,IRootMotionGame,IHitRegionsGame,IInputActionsGame,IPlayerPreferencesGame,IHierarchicalInstancesGame,INavigationGame,ICharacterInputGame,IMaskedAnimationGame
{public override void Initialize(ref ProbeState state){}public override void Tick(ref ProbeState state,GameContext context){}}
static unsafe partial class Program
{
    static bool physicalAnimationBadPayload;
    static int physicalAnimationMode,physicalAnimationGets,physicalAnimationLayerGets,physicalAnimationSets,physicalAnimationRates,physicalAnimationSources,physicalAnimationSourceSets;
    static Animation PhysicalAnimationPlayback(Id id,bool present)=>present ? new(){Entity=id,Present=1,Clip=3,Time=.125,Speed=2,Duration=4,Loop=1,TransitionPresent=1,
        Transition=new(){Start=99,Duration=32,Elapsed=24,Weight=.75,Clip=-1,Frozen=1}} : new(){Entity=id,Clip=-1,Transition=new(){Clip=-1}};
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PhysicalAnimationGet(void* context,Id* id,ExtendedAnimation* output,void* error)
    {
        ++physicalAnimationGets;if((nint)context!=0x1234 || output->Version!=1 || output->Bytes!=136 || output->Reserved!=0)physicalAnimationBadPayload=true;
        if(physicalAnimationMode==90)return LifecycleError(error);
        *output=new(){Version=1,Bytes=136,State=PhysicalAnimationPlayback(*id,id->Low==90),Mode=id->Low==90?1u:0u};
        switch(physicalAnimationMode){case 40:output->Version=2;break;case 41:output->Bytes=135;break;case 42:output->Reserved=1;break;case 43:output->Mode=2;break;
            case 44:output->State.Entity.Low=99;break;case 45:output->State.Present=2;break;case 46:output->State.Speed=9;break;case 47:output->State.Time=double.NaN;break;
            case 48:output->State.Transition.Weight=.5;break;case 49:output->State.Transition.Clip=0;break;case 50:output->State.TransitionPresent=0;break;
            case 51:if(id->Low==91)output->State.Clip=0;break;case 52:output->Bytes=137;break;}
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PhysicalAnimationLayerGet(void* context,Id* id,uint slot,LayerAnimation* output,void* error)
    {
        ++physicalAnimationLayerGets;if((nint)context!=0x1234 || output->Version!=1 || output->Bytes!=200 || output->Reserved!=0)physicalAnimationBadPayload=true;
        if(physicalAnimationMode==91)return LifecycleError(error);
        *output=new(){Version=1,Bytes=200,Slot=slot,State=PhysicalAnimationPlayback(*id,slot==1)};
        if(slot==1){output->Mode=1;output->LayerMode=1;output->MaskNodes=2;output->Weight=.5;output->TargetWeight=1;output->WeightStart=113;output->WeightDuration=20;output->WeightElapsed=10;output->WeightTarget=1;output->WeightPresent=1;}
        switch(physicalAnimationMode){case 60:output->Version=2;break;case 61:output->Bytes=201;break;case 62:output->Reserved=1;break;case 63:output->Slot=4;break;
            case 64:output->LayerMode=2;break;case 65:output->MaskNodes=10001;break;case 66:output->Weight=.25;break;case 67:output->WeightDuration=0;break;
            case 68:output->State.Present=0;break;case 69:output->State.Clip=-2;break;case 70:output->Weight=double.NaN;break;}
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PhysicalAnimationSet(void* context,LayerCommand* value,void* error)
    {
        ++physicalAnimationSets;var c=value->Command;
        if((nint)context!=0x1234 || value->Version!=1 || value->Bytes!=80 || value->Reserved!=0 || c.Entity.High!=11 || c.Entity.Low!=90)physicalAnimationBadPayload=true;
        if(value->Slot==0){if(value->Mode!=1 || value->Weight!=1 || value->WeightTicks!=0 || c.Clip!=-1 || c.Time!=0 || c.Speed!=.5 || c.Loop!=0 || c.Playing!=0 || c.BlendTicks!=12)physicalAnimationBadPayload=true;}
        else if(value->Slot==2){if(value->Mode!=0 || value->Weight!=.7 || value->WeightTicks!=19 || c.Clip!=3 || c.Time!=.25 || c.Speed!=1.5 || c.Loop!=1 || c.Playing!=1 || c.BlendTicks!=15)physicalAnimationBadPayload=true;}
        else physicalAnimationBadPayload=true;
        return physicalAnimationMode==92?LifecycleError(error):0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PhysicalAnimationRateSet(void* context,RateCommand* value,void* error)
    {
        ++physicalAnimationRates;if((nint)context!=0x1234 || value->Version!=1 || value->Bytes!=40 || value->Reserved!=0 || value->Entity.High!=11 || value->Entity.Low!=90 ||
            !((value->Layer==0 && value->Speed==0)||(value->Layer==2 && value->Speed==8)))physicalAnimationBadPayload=true;
        return physicalAnimationMode==93?LifecycleError(error):0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PhysicalAnimationSourceGet(void* context,DriveRequestWire* request,PhysicalAnimationSourceWire* output,void* error)
    {
        ++physicalAnimationSources;if((nint)context!=0x1234 || request->Version!=1 || request->Bytes!=40 || request->Reserved!=0 || request->Entity.High!=11 || request->Entity.Low!=90 || request->Joint==null || request->JointBytes is <1 or >64 || output->Version!=1 || output->Bytes!=208 || output->Present!=0 || output->Flags!=0 || output->ReservedName!=0 || output->ReservedTail!=0)physicalAnimationBadPayload=true;
        if(physicalAnimationMode==94)return LifecycleError(error);
        string name=Encoding.ASCII.GetString(new ReadOnlySpan<byte>(request->Joint,(int)request->JointBytes));*output=new(){Version=1,Bytes=208,Entity=request->Entity};
        if(name is "spine" or "manual")
        {
            output->Present=1;output->JointBytes=(uint)name.Length;for(int i=0;i<name.Length;++i)output->Joint[i]=(byte)name[i];output->Policy=1;output->Tick=123;
            if(name=="spine"){output->Source=1;output->Flags=1;output->Requested[3]=-1;output->Target[3]=-1;output->Condition=1;}
        }
        else if(name!="undriven")physicalAnimationBadPayload=true;
        switch(physicalAnimationMode){case 1:output->Version=2;break;case 2:output->Bytes=207;break;case 3:output->Present=2;break;case 4:output->Flags=16;break;
            case 5:output->Entity.Low=99;break;case 6:output->ReservedName=1;break;case 7:output->ReservedTail=1;break;case 8:output->JointBytes=64;break;
            case 9:output->Joint[0]=(byte)'x';break;case 10:output->Joint[63]=1;break;case 11:output->Source=2;break;case 12:output->Policy=2;break;
            case 13:output->Tick=122;break;case 14:if(name=="manual")output->Requested[3]=1;break;case 15:output->Flags=0;break;case 16:output->Flags=11;break;
            case 17:output->Policy=0;output->Flags=5;break;case 18:output->Requested[3]=0;break;case 19:output->Target[0]=double.NaN;break;
            case 20:output->Correction=-.01;break;case 21:output->Correction=Math.PI+.01;break;case 22:output->Condition=double.NaN;break;
            case 23:output->Condition=1.00001;break;case 24:output->Condition=-.01;break;case 25:if(name=="undriven")output->Tick=123;break;
            case 26:if(name=="manual")output->Flags=1;break;case 27:if(name=="manual")output->Requested[0]=BitConverter.Int64BitsToDouble(long.MinValue);break;
            case 28:if(name=="undriven")output->Flags=1;break;case 29:output->Bytes=209;break;}
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PhysicalAnimationSourceSet(void* context,PhysicalAnimationSourceCommandWire* value,void* error)
    {
        ++physicalAnimationSourceSets;if((nint)context!=0x1234 || value->Version!=1 || value->Bytes!=56 || value->Entity.High!=11 || value->Entity.Low!=90 ||
            value->Joint==null || value->JointBytes!=5 || Encoding.ASCII.GetString(new ReadOnlySpan<byte>(value->Joint,5))!="spine" || value->Reserved!=0 || value->ReservedSource!=0 ||
            !((value->Source==0 && value->Strength==0)||(value->Source==1 && value->Strength==.8)))physicalAnimationBadPayload=true;
        return physicalAnimationMode==95?LifecycleError(error):0;
    }
    static JsonElement PhysicalAnimationHost(uint bytes=496,bool physical=true,bool drive=true,bool animation=true,uint epoch=7,bool all=false)
    {
        var names=DriveHost(physical:physical,drive:drive,all:all).GetProperty("features").EnumerateArray().Select(x=>x.GetString()!).ToList();if(animation)names.Add("physical_rig_animation_v1");
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void PhysicalAnimationContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(PhysicalAnimationServicesWire)==496 && sizeof(DriveServicesWire)==448 && sizeof(PhysicalAnimationSourceWire)==208 && sizeof(PhysicalAnimationSourceCommandWire)==56,
            "Independent physical animation wire sizes differ.");
        foreach(var (name,offset) in new[]{("Get",448),("LayerGet",456),("Set",464),("RateSet",472),("SourceGet",480),("SourceSet",488)})
            Check(Marshal.OffsetOf<PhysicalAnimationServicesWire>(name).ToInt64()==offset,"Physical animation callback offset differs: "+name);
        foreach(var (name,offset) in new[]{("Entity",16),("JointBytes",32),("ReservedName",36),("Joint",40),("Source",104),("Policy",108),("Tick",112),("Requested",120),("Target",152),("Correction",184),("Condition",192),("ReservedTail",200)})
            Check(Marshal.OffsetOf<PhysicalAnimationSourceWire>(name).ToInt64()==offset,"Physical animation state offset differs: "+name);
        foreach(var (name,offset) in new[]{("Entity",8),("Joint",24),("JointBytes",32),("Reserved",36),("Source",40),("ReservedSource",44),("Strength",48)})
            Check(Marshal.OffsetOf<PhysicalAnimationSourceCommandWire>(name).ToInt64()==offset,"Physical animation command offset differs: "+name);
        Check(!typeof(ControlContext).GetMethods().Any(m=>m.Name.Contains("PhysicalRigAnimation") || m.Name.Contains("PhysicalRigDriveSource")),"Physical target APIs entered Control context.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,PhysicalAnimationHost(176),PhysicalAnimationHost(448),PhysicalAnimationHost(495),PhysicalAnimationHost(physical:false),PhysicalAnimationHost(drive:false),PhysicalAnimationHost(animation:false),PhysicalAnimationHost(epoch:8)})
        {Check(LoadExtension(&call,typeof(PhysicalAnimationConstructorProbe),host)!=0 && !Output(output).Contains("PHYSICAL ANIMATION CONSTRUCTOR REACHED"),"Invalid physical animation host reached constructor.");NoExtensionModules(&call);}
        foreach(uint bytes in new[]{496u,504u})
        {Check(LoadExtension(&call,typeof(PhysicalAnimationConstructorProbe),PhysicalAnimationHost(bytes))!=0 && Output(output).Contains("PHYSICAL ANIMATION CONSTRUCTOR REACHED"),"Valid physical animation host denied constructor.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(PhysicalAnimationAllProbe),PhysicalAnimationHost(all:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var required=doc.RootElement.GetProperty("requirements");
            var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;
            Check(required.GetProperty("services_bytes").GetUInt32()==496 && required.GetProperty("features").GetArrayLength()==21 && Convert.ToUInt32(parser.Invoke(null,[required.GetRawText()]))==1048575,"Full21 physical animation union differs.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(PhysicalAnimationProbe),PhysicalAnimationHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var required=doc.RootElement.GetProperty("requirements");
            Check(required.GetProperty("services_bytes").GetUInt32()==496 && required.GetProperty("features").EnumerateArray().Select(x=>x.GetString()).SequenceEqual(new[]{"baseline_v7","physical_rig_animation_v1","physical_rig_drive_v1","physical_rig_v1"}),"Physical animation grants unrelated rights.");
            var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;
            Check(Convert.ToUInt32(parser.Invoke(null,[required.GetRawText()]))==917504,"Physical animation generated flags differ.");
            foreach(var bad in new[]{PhysicalAnimationHost(495),PhysicalAnimationHost(drive:false),PhysicalAnimationHost(physical:false)})
            {bool rejected=false;try{_=parser.Invoke(null,[bad.GetRawText()]);}catch(TargetInvocationException e)when(e.InnerException is ArgumentException){rejected=true;}Check(rejected,"Invalid physical animation declaration accepted.");}
        }
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        PhysicalAnimationServicesWire services=default;var baseline=(Services*)&services;*baseline=good;baseline->Bytes=496;
        new Span<byte>((byte*)&services+176,224).Fill(0xa5);
        services.Drive.Physical.Summary=&PhysicalSummary;services.Drive.Physical.Part=&PhysicalPart;services.Drive.Physical.Command=&PhysicalCommand;
        services.Drive.SummaryV2=&DriveSummary;services.Drive.Get=&DriveGet;services.Drive.Set=&DriveSet;
        services.Get=&PhysicalAnimationGet;services.LayerGet=&PhysicalAnimationLayerGet;services.Set=&PhysicalAnimationSet;services.RateSet=&PhysicalAnimationRateSet;services.SourceGet=&PhysicalAnimationSourceGet;services.SourceSet=&PhysicalAnimationSourceSet;
        physicalAnimationGets=physicalAnimationLayerGets=physicalAnimationSets=physicalAnimationRates=physicalAnimationSources=physicalAnimationSourceSets=physicalAnimationMode=driveMode=0;physicalAnimationBadPayload=false;call.Operation=3;
        foreach(int bytes in new[]{176,424,448,455,456,463,464,471,472,479,480,487,488,495})
        {using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && physicalAnimationGets==0 && ((ProbeState*)state)->Count==5100,"Protected short physical animation tail accessed.");}
        call.Services=baseline;
        for(int slot=0;slot<6;++slot)
        {
            nint* callback=(nint*)((byte*)&services+448+slot*8);nint saved=*callback;*callback=0;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && physicalAnimationGets==0,"Missing physical animation callback accepted: "+slot);*callback=saved;
        }
        var driveGet=services.Drive.Get;services.Drive.Get=null;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && physicalAnimationGets==0,"Missing required drive callback accepted.");services.Drive.Get=driveGet;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        Check(physicalAnimationGets==3 && physicalAnimationLayerGets==3 && physicalAnimationSources==4 && physicalAnimationSets==2 && physicalAnimationRates==2 && physicalAnimationSourceSets==2 && ((ProbeState*)state)->Count==5101 && !physicalAnimationBadPayload,"Target transfer counts differ or invalid public input reached callback.");
        foreach(int mode in Enumerable.Range(1,29).Concat(Enumerable.Range(40,13)).Concat(Enumerable.Range(60,11)).Concat(Enumerable.Range(90,6)))
        {
            physicalAnimationMode=mode;int committed=((ProbeState*)state)->Count;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==committed,"Malformed physical animation/native error accepted: "+mode);
            if(mode>=90)Check(Output(output).Contains("lifecycle rejected"),"Physical animation native diagnostic lost.");
        }
        physicalAnimationMode=0;baseline->Bytes=504;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));baseline->Bytes=496;
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(PhysicalAnimationUnmarkedProbe),DriveHost())==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(448))
        {
            var old=(DriveServicesWire*)guard.Header;*old=services.Drive;((Services*)old)->Bytes=448;call.Services=(Services*)old;call.Operation=3;int reads=physicalAnimationGets;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare IPhysicalRigAnimationGame") && physicalAnimationGets==reads && ((ProbeState*)state)->Count==5200,"Old drive marker accessed target tail.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(!physicalAnimationBadPayload,"A physical animation callback received a malformed SDK payload.");
        checks.Add("physical_rig_animation496/STATE208/COMMAND56 independent wires; six appended callbacks, frozen448 prefix, isolated marker, constructor gates and protected tails");
        checks.Add("synthetic base/layer/rate/source transfer, literal quaternion signs, canonical absent/manual data, staged committed reads and59 malformed/native-error cases; no physical sampling claim");
    }
}
