// SPDX-License-Identifier: Apache-2.0
// Independent wire declarations and synthetic callbacks. This checks SDK/CLR
// boundaries; it is not native Jolt, native gameplay, or continuation evidence.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;
[StructLayout(LayoutKind.Sequential)] struct PhysicalSummaryWire
{public uint Version,Bytes,Present,Mode;public Id Entity;public uint BodyCount,JointCount;public ulong Reserved;}
[StructLayout(LayoutKind.Sequential)] unsafe struct PhysicalPartQueryWire
{public uint Version,Bytes;public Id Entity;public byte* Part;public uint PartBytes,Reserved;}
[StructLayout(LayoutKind.Sequential)] unsafe struct PhysicalPartWire
{
    public uint Version,Bytes,Present,Flags;public Id Entity,NodeEntity;public uint Node,BodyId,PartBytes,Reserved;public fixed byte Part[64];
    public fixed double Position[3];public fixed double Rotation[4];public fixed double Linear[3];public fixed double Angular[3];
    public double Mass;public fixed double Inertia[9];public double MaxLinear,MaxAngular,LinearDamping,AngularDamping,Friction,Restitution;public ulong ReservedTail;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct PhysicalCommandWire
{public uint Version,Bytes;public Id Entity;public uint Flags,Reserved;public byte* Part;public uint PartBytes,ReservedName;public fixed double Point[3];public fixed double Impulse[3];}
[StructLayout(LayoutKind.Sequential)] unsafe struct PhysicalServicesWire
{
    public fixed byte Prefix[400];
    public delegate* unmanaged[Cdecl]<void*,Id*,PhysicalSummaryWire*,void*,int> Summary;
    public delegate* unmanaged[Cdecl]<void*,PhysicalPartQueryWire*,PhysicalPartWire*,void*,int> Part;
    public delegate* unmanaged[Cdecl]<void*,PhysicalCommandWire*,void*,int> Command;
}
[GameModule("poima-test-physical-rig")]
public class PhysicalProbe:Game<ProbeState>,IPhysicalRigGame
{
    // Module code executes in a collectible assembly context. Its oracle must
    // be literal module data, independent of the host callback's static fields.
    protected virtual uint ExpectedBodyId=>0;
    public override void Initialize(ref ProbeState state){state.Count=4300;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        EntityId entity=new(11,90);var before=context.GetPhysicalRig(entity) ?? throw new Exception("Physical summary absent.");
        if(before!=new PhysicalRigState(entity,PhysicalRigMode.Animated,2,1))throw new Exception("Physical committed summary differs.");
        var part=context.GetPhysicalRigPart(entity,"pelvis") ?? throw new Exception("Physical part absent.");
        if(part.Entity!=entity || part.NodeEntity!=new EntityId(11,92) || part.Node!=7 || part.BodyId!=ExpectedBodyId || part.Part!="pelvis" || part.Position!=new Vector3d(1,2,3) || part.Rotation!=new Quaterniond(0,0,0,-1) || part.LinearVelocity!=new Vector3d(4,5,6) || part.AngularVelocity!=new Vector3d(7,8,9) || part.Mass!=2 || part.Inertia!=new PhysicalRigInertia(1e-12,0,0,0,2e-12,0,0,0,3e-12) || part.Enrolled || part.Active || part.Sleeping || part.AtLinearVelocityLimit || part.AtAngularVelocityLimit || part.MaxLinearVelocity!=1000 || part.MaxAngularVelocity!=20 || part.LinearDamping!=.05 || part.AngularDamping!=.07 || part.Friction!=.5 || part.Restitution!=.1)throw new Exception("Actual allocated COM/material fields or quaternion sign differ.");
        if(context.GetPhysicalRig(new(11,91))!=null || context.GetPhysicalRigPart(entity,"missing")!=null)throw new Exception("Absent physical state has payload.");
        context.ActivatePhysicalRig(entity);context.ImpulsePhysicalRig(entity,"pelvis",new(1,2,3),new(10,0,0));
        if(context.GetPhysicalRig(entity)!=before || context.GetPhysicalRigPart(entity,"pelvis")!=part)throw new Exception("Staged physical writes leaked into committed synthetic reads.");
        foreach(var bad in new[]{new PhysicalRigCommand(entity),new PhysicalRigCommand(default,true),new PhysicalRigCommand(entity,false,new("",new(),new())),
            new PhysicalRigCommand(entity,false,new("é",new(),new())),new PhysicalRigCommand(entity,false,new("a/b",new(),new())),new PhysicalRigCommand(entity,false,new(new string('x',65),new(),new())),
            new PhysicalRigCommand(entity,false,new("pelvis",new(double.NaN,0,0),new())),new PhysicalRigCommand(entity,false,new("pelvis",new(1000001,0,0),new())),
            new PhysicalRigCommand(entity,false,new("pelvis",new(),new(double.PositiveInfinity,0,0))),new PhysicalRigCommand(entity,false,new("pelvis",new(),new(80000,80000,0)))})
        {bool rejected=false;try{context.SetPhysicalRig(bad);}catch(ArgumentException){rejected=true;}if(!rejected)throw new Exception("Invalid physical command entered host.");}
        foreach(string bad in new[]{"","é","a/b",new string('x',65)}){bool rejected=false;try{_=context.GetPhysicalRigPart(entity,bad);}catch(ArgumentException){rejected=true;}if(!rejected)throw new Exception("Invalid part name entered host.");}
        bool unrelated=false;try{_=context.CapsuleCast(new(),Quaterniond.Identity,.5,1,new(1,0,0),1);}catch(ArgumentException error){unrelated=error.Message.Contains("Declare ICapsuleCastGame");}if(!unrelated)throw new Exception("Physical marker granted unrelated capsule rights.");++state.Count;
    }
}
[GameModule("poima-test-physical-rig-generation1")]
public sealed class PhysicalGeneration1Probe:PhysicalProbe
{protected override uint ExpectedBodyId=>0x00800001u;}
[GameModule("poima-test-physical-rig-generation255")]
public sealed class PhysicalGeneration255Probe:PhysicalProbe
{protected override uint ExpectedBodyId=>0x7f800fffu;}
[GameModule("poima-test-physical-rig-constructor")]
public sealed class PhysicalConstructorProbe:Game<ProbeState>,IPhysicalRigGame
{public PhysicalConstructorProbe(){throw new Exception("PHYSICAL CONSTRUCTOR REACHED");}public override void Initialize(ref ProbeState state){}public override void Tick(ref ProbeState state,GameContext context){}}
[GameModule("poima-test-physical-rig-unmarked")]
public sealed class PhysicalUnmarkedProbe:Game<ProbeState>
{public override void Initialize(ref ProbeState state){state.Count=4400;}public override void Tick(ref ProbeState state,GameContext context){_=context.GetPhysicalRig(new(11,90));++state.Count;}}
[GameModule("poima-test-physical-rig-all")]
public sealed class PhysicalAllProbe:Game<ProbeState>,IPhysicalRigGame,ICapsuleCastGame,ICameraPoseGame,IAnimationRateGame,IGroundingControlGame,
    ICharacterStanceGame,ICameraLensGame,IRootMotionGame,IHitRegionsGame,IInputActionsGame,IPlayerPreferencesGame,IHierarchicalInstancesGame,INavigationGame,ICharacterInputGame,IMaskedAnimationGame
{public override void Initialize(ref ProbeState state){}public override void Tick(ref ProbeState state,GameContext context){}}
static unsafe partial class Program
{
    static int physicalSummaryReads,physicalPartReads,physicalWrites,physicalMode;
    // Independent pinned-Jolt wire fixture: index mask0x007fffff, sequence shift23, broadphase bit31.
    internal static uint PhysicalBodyId;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PhysicalSummary(void* context,Id* entity,PhysicalSummaryWire* output,void* error)
    {
        ++physicalSummaryReads;if((nint)context!=0x1234 || entity==null || output->Version!=1 || output->Bytes!=48 || output->Present!=0 || output->Mode!=0 || output->Reserved!=0 || entity->High!=11 || entity->Low is <90 or >91)badPayload=true;
        if(physicalMode==1)return LifecycleError(error);*output=new(){Version=1,Bytes=48,Entity=*entity};if(entity->Low==90){output->Present=1;output->BodyCount=2;output->JointCount=1;}
        switch(physicalMode){case 2:output->Bytes=49;break;case 3:output->Present=2;break;case 4:output->Mode=2;break;case 5:output->Entity.Low=99;break;case 6:output->Reserved=1;break;case 7:output->BodyCount=65;break;case 8:output->JointCount=0;break;case 9:if(entity->Low==91)output->BodyCount=1;break;}return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PhysicalPart(void* context,PhysicalPartQueryWire* request,PhysicalPartWire* output,void* error)
    {
        ++physicalPartReads;if((nint)context!=0x1234 || request->Version!=1 || request->Bytes!=40 || request->Reserved!=0 || request->Entity.High!=11 || request->Entity.Low!=90 || request->Part==null || request->PartBytes is <1 or >64 || output->Version!=1 || output->Bytes!=368 || output->Present!=0 || output->Flags!=0 || output->Reserved!=0 || output->ReservedTail!=0)badPayload=true;
        string name=Encoding.ASCII.GetString(new ReadOnlySpan<byte>(request->Part,(int)request->PartBytes));*output=new(){Version=1,Bytes=368,Entity=request->Entity};
        if(name=="pelvis")
        {
            output->Present=1;output->NodeEntity=new(){High=11,Low=92};output->Node=7;output->BodyId=PhysicalBodyId;output->PartBytes=6;for(int i=0;i<6;++i)output->Part[i]=(byte)name[i];
            for(int i=0;i<3;++i){output->Position[i]=i+1;output->Linear[i]=i+4;output->Angular[i]=i+7;}output->Rotation[3]=-1;output->Mass=2;output->Inertia[0]=1e-12;output->Inertia[4]=2e-12;output->Inertia[8]=3e-12;output->MaxLinear=1000;output->MaxAngular=20;output->LinearDamping=.05;output->AngularDamping=.07;output->Friction=.5;output->Restitution=.1;
        }else if(name!="missing")badPayload=true;
        switch(physicalMode){
            case 10:output->Version=2;break;case 11:output->Bytes=367;break;case 12:output->Present=2;break;case 13:output->Flags=32;break;case 14:output->Entity.Low=99;break;case 15:output->Reserved=1;break;case 16:output->ReservedTail=1;break;
            case 17:output->NodeEntity=default;break;case 18:output->Node=10000;break;case 19:output->BodyId=4096;break;case 20:output->PartBytes=7;break;case 21:output->Part[6]=1;break;
            case 22:output->Position[0]=double.NaN;break;case 23:output->Rotation[3]=0;break;case 24:output->Linear[0]=double.PositiveInfinity;break;case 25:output->Mass=0;break;case 26:output->Inertia[4]=-1;break;case 27:output->Inertia[1]=1;break;
            case 28:output->Friction=2.1;break;case 29:output->Flags=2;break;case 30:if(name=="missing")output->Node=1;break;
            case 32:output->BodyId=0x80800001u;break; // Native broadphase bit31 is never a portable raw body ID.
            case 33:output->BodyId=(1u<<23)|4096u;break; // Gen1 cannot conceal an out-of-pool slot.
            case 34:output->BodyId=0xffffffffu;break;
        }return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PhysicalCommand(void* context,PhysicalCommandWire* command,void* error)
    {
        ++physicalWrites;if((nint)context!=0x1234 || command->Version!=1 || command->Bytes!=96 || command->Entity.High!=11 || command->Entity.Low!=90 || command->Reserved!=0 || command->ReservedName!=0)badPayload=true;
        if(physicalMode==31)return LifecycleError(error);
        if((physicalWrites&1)==1){if(command->Flags!=1 || command->Part!=null || command->PartBytes!=0)badPayload=true;for(int i=0;i<3;++i)if(BitConverter.DoubleToInt64Bits(command->Point[i])!=0 || BitConverter.DoubleToInt64Bits(command->Impulse[i])!=0)badPayload=true;}
        else{if(command->Flags!=2 || command->PartBytes!=6 || command->Part==null || Encoding.ASCII.GetString(new ReadOnlySpan<byte>(command->Part,6))!="pelvis" || command->Impulse[0]!=10 || command->Impulse[1]!=0 || command->Impulse[2]!=0)badPayload=true;for(int i=0;i<3;++i)if(command->Point[i]!=i+1)badPayload=true;}return 0;
    }
    static JsonElement PhysicalHost(uint bytes=424,bool feature=true,uint epoch=7,bool all=false)
    {
        var names=new List<string>{"baseline_v7"};if(feature)names.Add("physical_rig_v1");
        if(all)foreach(var item in CapsuleHost(all:true).GetProperty("features").EnumerateArray()){string name=item.GetString()!;if(!names.Contains(name))names.Add(name);}
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void PhysicalRigContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(PhysicalSummaryWire)==48 && sizeof(PhysicalPartQueryWire)==40 && sizeof(PhysicalPartWire)==368 && sizeof(PhysicalCommandWire)==96 && sizeof(PhysicalServicesWire)==424 && Marshal.OffsetOf<PhysicalServicesWire>(nameof(PhysicalServicesWire.Summary)).ToInt64()==400 && Marshal.OffsetOf<PhysicalServicesWire>(nameof(PhysicalServicesWire.Part)).ToInt64()==408 && Marshal.OffsetOf<PhysicalServicesWire>(nameof(PhysicalServicesWire.Command)).ToInt64()==416 && Marshal.OffsetOf<PhysicalPartWire>(nameof(PhysicalPartWire.Position)).ToInt64()==128 && Marshal.OffsetOf<PhysicalPartWire>(nameof(PhysicalPartWire.Inertia)).ToInt64()==240 && Marshal.OffsetOf<PhysicalPartWire>(nameof(PhysicalPartWire.ReservedTail)).ToInt64()==360,"Independent physical POD layouts differ.");
        Check(typeof(ControlContext).GetMethod("GetPhysicalRig")==null && typeof(ControlContext).GetMethod("SetPhysicalRig")==null,"Physical writes entered Control.");Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,PhysicalHost(176),PhysicalHost(400),PhysicalHost(423),PhysicalHost(feature:false),PhysicalHost(epoch:8)}){Check(LoadExtension(&call,typeof(PhysicalConstructorProbe),host)!=0 && !Output(output).Contains("PHYSICAL CONSTRUCTOR REACHED"),"Invalid physical host reached constructor.");NoExtensionModules(&call);}
        foreach(uint bytes in new[]{424u,432u}){Check(LoadExtension(&call,typeof(PhysicalConstructorProbe),PhysicalHost(bytes))!=0 && Output(output).Contains("PHYSICAL CONSTRUCTOR REACHED"),"Valid physical host denied constructor.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(PhysicalAllProbe),PhysicalHost(all:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output))){call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var required=doc.RootElement.GetProperty("requirements");Check(required.GetProperty("services_bytes").GetUInt32()==424 && required.GetProperty("features").GetArrayLength()==19,"Full19 physical feature union differs.");var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;Check(Convert.ToUInt32(parser.Invoke(null,[required.GetRawText()]))==262143,"Full physical generated union lost feature flags.");}
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(PhysicalProbe),PhysicalHost())==0,Output(output));using(var doc=JsonDocument.Parse(Output(output))){call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var required=doc.RootElement.GetProperty("requirements");Check(required.GetProperty("services_bytes").GetUInt32()==424 && required.GetProperty("features").EnumerateArray().Select(x=>x.GetString()).SequenceEqual(new[]{"baseline_v7","physical_rig_v1"}),"Physical declaration granted unrelated rights.");var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;Check(Convert.ToUInt32(parser.Invoke(null,[required.GetRawText()]))==131072,"Physical generated requirement flags differ.");}
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));PhysicalServicesWire services=default;var baseline=(Services*)(&services);*baseline=good;baseline->Bytes=424;new Span<byte>((byte*)&services+176,224).Fill(0xa5);services.Summary=&PhysicalSummary;services.Part=&PhysicalPart;services.Command=&PhysicalCommand;physicalSummaryReads=physicalPartReads=physicalWrites=physicalMode=0;PhysicalBodyId=0;call.Operation=3;
        foreach(int bytes in new[]{176,392,400,408,416,423}){using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && physicalSummaryReads==0 && physicalPartReads==0 && physicalWrites==0 && ((ProbeState*)state)->Count==4300,"Protected short physical table accessed.");}
        for(int missing=0;missing<3;++missing){var absent=services;if(missing==0)absent.Summary=null;else if(missing==1)absent.Part=null;else absent.Command=null;call.Services=(Services*)(&absent);Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && physicalSummaryReads==0 && physicalPartReads==0 && physicalWrites==0,"Absent physical callback entered game.");}
        call.Services=baseline;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==4301 && physicalSummaryReads==3 && physicalPartReads==3 && physicalWrites==2 && !badPayload,Output(output));
        foreach(var item in new[]{(Raw:0x00800001u,Type:typeof(PhysicalGeneration1Probe)),(Raw:0x7f800fffu,Type:typeof(PhysicalGeneration255Probe))}) {
            call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
            Check(LoadExtension(&call,item.Type,PhysicalHost())==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
            call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            PhysicalBodyId=item.Raw;call.Services=baseline;call.Operation=3;
            Check((item.Raw&0x007fffffu)<4096 && (item.Raw&0x80000000u)==0 && Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==4301 && !badPayload,"Valid native generation1/255 raw ID was rejected or changed: "+Output(output));
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(PhysicalProbe),PhysicalHost())==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        PhysicalBodyId=0;call.Services=baseline;call.Operation=3;int physicalCommittedCount=((ProbeState*)state)->Count;
        for(int mode=1;mode<=34;++mode){physicalMode=mode;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==physicalCommittedCount,"Malformed physical state or native error accepted: "+mode);if(mode is 1 or 31)Check(Output(output).Contains("lifecycle rejected"),"Physical native diagnostic lost.");}
        physicalMode=0;call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(PhysicalUnmarkedProbe),null)==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(176)){*(Services*)guard.Header=good;call.Services=(Services*)guard.Header;call.Operation=3;int reads=physicalSummaryReads;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare IPhysicalRigGame") && physicalSummaryReads==reads && ((ProbeState*)state)->Count==4400,"Unmarked physical game accessed tail.");}
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);checks.Add("physical_rig424/SUMMARY48/QUERY40/PART368/COMMAND96 independent wires; optional19-feature union and exact old prefixes; protected tails, marker isolation and missing callbacks");checks.Add("synthetic actual-COM/negative-quaternion/tiny-SPD transfer, ordered borrowed-name staged writes and committed reads, valid generation0/1/255 raw IDs and34 malformed/native-error cases; not native Jolt evidence");
    }
}
