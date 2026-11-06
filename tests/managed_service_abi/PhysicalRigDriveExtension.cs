// SPDX-License-Identifier: Apache-2.0
// Independent PODs and synthetic host callbacks test ABI/SDK boundaries only.
// Native joint torques, ordering and whole-save behavior need runtime evidence.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;
[StructLayout(LayoutKind.Sequential)] unsafe struct DriveRequestWire
{public uint Version,Bytes;public Id Entity;public byte* Joint;public uint JointBytes,Reserved;}
[StructLayout(LayoutKind.Sequential)] unsafe struct DriveStateWire
{
    public uint Version,Bytes,Present,Reserved;public Id Entity;public uint JointBytes,ReservedName;public fixed byte Joint[64];
    public fixed double Target[4];public double Strength,Stiffness,Damping,MaxTorque;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct DriveCommandWire
{
    public uint Version,Bytes;public Id Entity;public byte* Joint;public uint JointBytes,Reserved;
    public fixed double Target[4];public double Strength;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct DriveServicesWire
{
    public PhysicalServicesWire Physical;
    public delegate* unmanaged[Cdecl]<void*,Id*,PhysicalSummaryWire*,void*,int> SummaryV2;
    public delegate* unmanaged[Cdecl]<void*,DriveRequestWire*,DriveStateWire*,void*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,DriveCommandWire*,void*,int> Set;
}
[GameModule("poima-test-physical-drive")]
public sealed class DriveProbe:Game<ProbeState>,IPhysicalRigDriveGame
{
    public override void Initialize(ref ProbeState state){state.Count=4800;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        EntityId entity=new(11,90);var summary=context.GetPhysicalRig(entity);
        if(summary!=new PhysicalRigState(entity,PhysicalRigMode.Driven,2,1))throw new Exception("Drive summary did not use V2.");
        if(context.GetPhysicalRig(new(11,91))!=null)throw new Exception("Absent drive summary has payload.");
        var value=context.GetPhysicalRigDrive(entity,"spine");
        if(value!=new PhysicalRigDriveState(entity,"spine",new(0,0,0,-1),.5,12,1.3,100))throw new Exception("Committed drive state differs or quaternion sign changed.");
        if(context.GetPhysicalRigDrive(entity,"undriven")!=null)throw new Exception("Unauthored drive has state.");
        context.SetPhysicalRigDrive(entity,"spine",new(0,0,0,-1),1);
        context.SetPhysicalRigDrive(new(entity,"spine",new(0,0,0,-1),0));
        if(context.GetPhysicalRigDrive(entity,"spine")!=value)throw new Exception("Staged drive leaked into committed read.");
        foreach(var bad in new PhysicalRigDriveCommand[]{new(default,"spine",Quaterniond.Identity,1),new(entity,"",Quaterniond.Identity,1),new(entity,"a/b",Quaterniond.Identity,1),
            new(entity,"é",Quaterniond.Identity,1),new(entity,new string('x',65),Quaterniond.Identity,1),new(entity,"spine",new(),1),new(entity,"spine",new(0,0,0,1.00001),1),
            new(entity,"spine",new(double.NaN,0,0,1),1),new(entity,"spine",Quaterniond.Identity,double.NaN),new(entity,"spine",Quaterniond.Identity,-.01),new(entity,"spine",Quaterniond.Identity,1.01)})
        {bool rejected=false;try{context.SetPhysicalRigDrive(bad);}catch(ArgumentException){rejected=true;}if(!rejected)throw new Exception("Invalid drive reached host.");}
        bool unrelated=false;try{_=context.CapsuleCast(new(),Quaterniond.Identity,.5,1,new(1,0,0),1);}catch(ArgumentException e){unrelated=e.Message.Contains("Declare ICapsuleCastGame");}
        if(!unrelated)throw new Exception("Drive marker granted unrelated capsule rights.");++state.Count;
    }
}
[GameModule("poima-test-physical-drive-constructor")]
public sealed class DriveConstructorProbe:Game<ProbeState>,IPhysicalRigDriveGame
{public DriveConstructorProbe(){throw new Exception("DRIVE CONSTRUCTOR REACHED");}public override void Initialize(ref ProbeState state){}public override void Tick(ref ProbeState state,GameContext context){}}
[GameModule("poima-test-physical-drive-unmarked")]
public sealed class DriveUnmarkedProbe:Game<ProbeState>,IPhysicalRigGame
{public override void Initialize(ref ProbeState state){state.Count=4900;}public override void Tick(ref ProbeState state,GameContext context){_=context.GetPhysicalRigDrive(new(11,90),"spine");++state.Count;}}
[GameModule("poima-test-physical-drive-all")]
public sealed class DriveAllProbe:Game<ProbeState>,IPhysicalRigDriveGame,ICapsuleCastGame,ICameraPoseGame,IAnimationRateGame,IGroundingControlGame,
    ICharacterStanceGame,ICameraLensGame,IRootMotionGame,IHitRegionsGame,IInputActionsGame,IPlayerPreferencesGame,IHierarchicalInstancesGame,INavigationGame,ICharacterInputGame,IMaskedAnimationGame
{public override void Initialize(ref ProbeState state){}public override void Tick(ref ProbeState state,GameContext context){}}
static unsafe partial class Program
{
    static int driveSummaryReads,driveReads,driveWrites,driveMode;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int DriveSummary(void* context,Id* entity,PhysicalSummaryWire* output,void* error)
    {
        ++driveSummaryReads;if((nint)context!=0x1234 || entity==null || entity->High!=11 || entity->Low is <90 or >91 || output->Version!=2 || output->Bytes!=48 || output->Present!=0 || output->Mode!=0 || output->Reserved!=0)badPayload=true;
        *output=new(){Version=2,Bytes=48,Entity=*entity};if(entity->Low==90){output->Present=1;output->Mode=2;output->BodyCount=2;output->JointCount=1;}
        switch(driveMode){case 2:output->Version=1;break;case 3:output->Mode=3;break;case 23:if(entity->Low==91)output->Mode=1;break;case 24:output->Entity.Low=99;break;case 25:output->Reserved=1;break;case 26:output->BodyCount=65;break;}return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int DriveGet(void* context,DriveRequestWire* request,DriveStateWire* output,void* error)
    {
        ++driveReads;if((nint)context!=0x1234 || request->Version!=1 || request->Bytes!=40 || request->Reserved!=0 || request->Entity.High!=11 || request->Entity.Low!=90 || request->Joint==null || request->JointBytes is <1 or >64 || output->Version!=1 || output->Bytes!=168 || output->Present!=0 || output->Reserved!=0 || output->ReservedName!=0)badPayload=true;
        if(driveMode==1)return LifecycleError(error);
        string name=Encoding.ASCII.GetString(new ReadOnlySpan<byte>(request->Joint,(int)request->JointBytes));*output=new(){Version=1,Bytes=168,Entity=request->Entity};
        if(name=="spine")
        {
            output->Present=1;output->JointBytes=5;for(int i=0;i<5;++i)output->Joint[i]=(byte)name[i];output->Target[3]=-1;
            output->Strength=.5;output->Stiffness=12;output->Damping=1.3;output->MaxTorque=100;
        }else if(name!="undriven")badPayload=true;
        switch(driveMode){case 4:output->Version=2;break;case 5:output->Bytes=167;break;case 6:output->Present=2;break;case 7:output->Entity.Low=99;break;
            case 8:output->Reserved=1;break;case 9:output->ReservedName=1;break;case 10:output->JointBytes=6;break;case 11:output->Joint[5]=1;break;
            case 12:output->Target[3]=0;break;case 13:output->Strength=double.NaN;break;case 14:output->Strength=1.01;break;case 15:output->Stiffness=0;break;
            case 16:output->Stiffness=1000001;break;case 17:output->Damping=-1;break;case 18:output->Damping=10001;break;case 19:output->MaxTorque=0;break;
            case 20:output->MaxTorque=100001;break;case 21:if(name=="undriven")output->Strength=BitConverter.Int64BitsToDouble(long.MinValue);break;}
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int DriveSet(void* context,DriveCommandWire* command,void* error)
    {
        ++driveWrites;if((nint)context!=0x1234 || command->Version!=1 || command->Bytes!=80 || command->Entity.High!=11 || command->Entity.Low!=90 || command->Reserved!=0 || command->Joint==null || command->JointBytes!=5 || Encoding.ASCII.GetString(new ReadOnlySpan<byte>(command->Joint,5))!="spine" || command->Target[0]!=0 || command->Target[1]!=0 || command->Target[2]!=0 || command->Target[3]!=-1 || command->Strength!=((driveWrites&1)==1?1:0))badPayload=true;
        return driveMode==22 ? LifecycleError(error) : 0;
    }
    static JsonElement DriveHost(uint bytes=448,bool physical=true,bool drive=true,uint epoch=7,bool all=false)
    {
        var names=new List<string>{"baseline_v7"};if(physical)names.Add("physical_rig_v1");if(drive)names.Add("physical_rig_drive_v1");
        if(all)foreach(var feature in PhysicalHost(all:true).GetProperty("features").EnumerateArray()){string name=feature.GetString()!;if(!names.Contains(name))names.Add(name);}
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void PhysicalDriveContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(DriveServicesWire)==448 && sizeof(PhysicalServicesWire)==424 && sizeof(PhysicalCommandWire)==96 && sizeof(DriveRequestWire)==40 && sizeof(DriveStateWire)==168 && sizeof(DriveCommandWire)==80 &&
            Marshal.OffsetOf<DriveServicesWire>(nameof(DriveServicesWire.SummaryV2)).ToInt64()==424 && Marshal.OffsetOf<DriveServicesWire>(nameof(DriveServicesWire.Get)).ToInt64()==432 && Marshal.OffsetOf<DriveServicesWire>(nameof(DriveServicesWire.Set)).ToInt64()==440 &&
            Marshal.OffsetOf<DriveStateWire>(nameof(DriveStateWire.Joint)).ToInt64()==40 && Marshal.OffsetOf<DriveStateWire>(nameof(DriveStateWire.Target)).ToInt64()==104 && Marshal.OffsetOf<DriveStateWire>(nameof(DriveStateWire.Strength)).ToInt64()==136 && Marshal.OffsetOf<DriveStateWire>(nameof(DriveStateWire.MaxTorque)).ToInt64()==160 && Marshal.OffsetOf<DriveCommandWire>(nameof(DriveCommandWire.Target)).ToInt64()==40,"Independent drive wire layouts differ.");
        Check(typeof(ControlContext).GetMethod("GetPhysicalRigDrive")==null && !typeof(ControlContext).GetMethods().Any(m=>m.Name=="SetPhysicalRigDrive"),"Drives entered Control API.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,DriveHost(176),DriveHost(424),DriveHost(447),DriveHost(physical:false),DriveHost(drive:false),DriveHost(epoch:8)})
        {Check(LoadExtension(&call,typeof(DriveConstructorProbe),host)!=0 && !Output(output).Contains("DRIVE CONSTRUCTOR REACHED"),"Invalid drive host reached constructor.");NoExtensionModules(&call);}
        foreach(uint size in new[]{448u,456u}){Check(LoadExtension(&call,typeof(DriveConstructorProbe),DriveHost(size))!=0 && Output(output).Contains("DRIVE CONSTRUCTOR REACHED"),"Valid drive host denied constructor.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(DriveAllProbe),DriveHost(all:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var required=doc.RootElement.GetProperty("requirements");
            var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;
            Check(required.GetProperty("services_bytes").GetUInt32()==448 && required.GetProperty("features").GetArrayLength()==20 && Convert.ToUInt32(parser.Invoke(null,[required.GetRawText()]))==524287,"Full20 generated drive union differs.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(DriveProbe),DriveHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var required=doc.RootElement.GetProperty("requirements");
            Check(required.GetProperty("services_bytes").GetUInt32()==448 && required.GetProperty("features").EnumerateArray().Select(x=>x.GetString()).SequenceEqual(new[]{"baseline_v7","physical_rig_drive_v1","physical_rig_v1"}),"Drive declaration grants unrelated rights.");
            var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;
            Check(Convert.ToUInt32(parser.Invoke(null,[required.GetRawText()]))==393216,"Generated drive flags differ.");
        }
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        DriveServicesWire services=default;var baseline=(Services*)(&services);*baseline=good;baseline->Bytes=448;
        new Span<byte>((byte*)&services+176,224).Fill(0xa5);services.Physical.Summary=&PhysicalSummary;services.Physical.Part=&PhysicalPart;services.Physical.Command=&PhysicalCommand;
        services.SummaryV2=&DriveSummary;services.Get=&DriveGet;services.Set=&DriveSet;
        driveSummaryReads=driveReads=driveWrites=driveMode=0;call.Operation=3;
        foreach(int size in new[]{176,400,424,432,440,447})
        {
            using var guard=new GuardedHeader(size);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)size;call.Services=(Services*)guard.Header;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && driveSummaryReads==0 && driveReads==0 && driveWrites==0 && ((ProbeState*)state)->Count==4800,"Protected short drive tail accessed.");
        }
        call.Services=baseline;
        var summary=services.SummaryV2;services.SummaryV2=null;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && driveSummaryReads==0,"Missing V2 summary accepted.");services.SummaryV2=summary;
        var get=services.Get;services.Get=null;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && driveSummaryReads==0,"Missing drive read accepted.");services.Get=get;
        var set=services.Set;services.Set=null;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && driveSummaryReads==0,"Missing drive write accepted.");services.Set=set;
        int legacyReads=physicalSummaryReads;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        Check(driveSummaryReads==2 && driveReads==3 && driveWrites==2 && physicalSummaryReads==legacyReads && ((ProbeState*)state)->Count==4801,"V2 dispatch or staged drive callback counts differ.");
        for(int mode=1;mode<=26;++mode)
        {
            driveMode=mode;driveWrites=0;int committed=((ProbeState*)state)->Count;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==committed,"Malformed drive/native error accepted: "+mode);
            if(mode is 1 or 22)Check(Output(output).Contains("lifecycle rejected"),"Drive native diagnostic lost.");
        }
        driveMode=0;call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(DriveUnmarkedProbe),PhysicalHost())==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(424))
        {
            var legacy=(PhysicalServicesWire*)guard.Header;*(Services*)legacy=good;((Services*)legacy)->Bytes=424;legacy->Summary=&PhysicalSummary;legacy->Part=&PhysicalPart;legacy->Command=&PhysicalCommand;call.Services=(Services*)legacy;call.Operation=3;
            int reads=driveReads;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare IPhysicalRigDriveGame") && driveReads==reads && ((ProbeState*)state)->Count==4900,"Legacy marker accessed drive tail.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("physical_rig_drive448/SUMMARY-v2-48/QUERY40/STATE168/COMMAND80 independent wires; frozen424/96 prefix, named dependency and protected tails");
        checks.Add("synthetic V2 Driven dispatch, literal signed target transfer, committed reads, staged zero/positive writes and26 malformed/native-error cases; no native torque claim");
    }
}
