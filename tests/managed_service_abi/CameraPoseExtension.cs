// SPDX-License-Identifier: Apache-2.0
// Independent wire declarations; no production internal POD layouts are reused.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;
[StructLayout(LayoutKind.Sequential)] struct PoseWireValue {public double X,Y,Z,Qx,Qy,Qz,Qw;}
[StructLayout(LayoutKind.Sequential)] struct PoseWireCommand
{public uint Version,Bytes;public Id Entity;public uint Channels,Reserved;public PoseWireValue World,Viewmodel;}
[StructLayout(LayoutKind.Sequential)] unsafe struct PoseWireState
{public uint Version,Bytes,Present,Flags;public Id Entity;public PoseWireValue World,Viewmodel;public fixed double BaseWorld[16];}
[StructLayout(LayoutKind.Sequential)] unsafe struct PoseWireServices
{public RateServices Rate;public delegate* unmanaged[Cdecl]<void*,Id*,PoseWireState*,void*,int> Get;public delegate* unmanaged[Cdecl]<void*,PoseWireCommand*,void*,int> Set;}
[GameModule("poima-test-camera-pose")]
public sealed class PoseProbe:Game<ProbeState>,ICameraPoseGame
{
    public override void Initialize(ref ProbeState state){state.Count=3200;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        EntityId entity=new(11,70);var before=context.GetCameraPose(entity) ?? throw new Exception("Camera pose absent.");
        if(before.Entity!=entity || before.World!=new CameraPoseOffset(new(.1,.2,.3),Quaterniond.Identity) || before.Viewmodel!=CameraPoseOffset.Identity || before.BaseWorld.Position!=new Vector3d(7,8,9) || before.BaseWorld.Forward!=new Vector3d(0,0,-1))throw new Exception("Committed pose/base transfer differs.");
        context.SetCameraPose(entity,new CameraPoseOffset(new(.4,.5,.6),Quaterniond.Identity),new CameraPoseOffset(new(-.1,-.2,-.3),Quaterniond.Identity));
        var after=context.GetCameraPose(entity) ?? throw new Exception("Staged getter lost camera.");
        if(after.Entity!=before.Entity || after.World!=before.World || after.Viewmodel!=before.Viewmodel)throw new Exception("Staged setter leaked offsets.");
        for(int c=0;c<4;++c)for(int r=0;r<4;++r)if(after.BaseWorld[c,r]!=before.BaseWorld[c,r])throw new Exception("Staged setter leaked base matrix.");
        if(after!=before || !after.Equals(before) || after.GetHashCode()!=before.GetHashCode() || after.BaseWorld!=before.BaseWorld || !after.BaseWorld.Equals((object)before.BaseWorld) || after.BaseWorld.GetHashCode()!=before.BaseWorld.GetHashCode())throw new Exception("Camera pose/base equality or hash lost a staged committed value.");
        if(context.GetCameraPose(new(11,71))!=null)throw new Exception("Noncamera not absent.");
        var worldOnly=context.GetCameraPose(new(11,72)) ?? throw new Exception("World-only camera absent.");
        if(worldOnly.Viewmodel!=null || worldOnly.World!=before.World)throw new Exception("Absent foreground transfer differs.");
        foreach(var command in new[]{new CameraPoseCommand(entity),new CameraPoseCommand(default,CameraPoseOffset.Identity),
            new CameraPoseCommand(entity,new CameraPoseOffset(new(double.NaN,0,0),Quaterniond.Identity)),
            new CameraPoseCommand(entity,new CameraPoseOffset(new(10.01,0,0),Quaterniond.Identity)),
            new CameraPoseCommand(entity,new CameraPoseOffset(new(0,0,0),new(0,0,0,0))),
            new CameraPoseCommand(entity,new CameraPoseOffset(new(0,0,0),new(0,0,0,double.PositiveInfinity)))})
        {bool rejected=false;try{context.SetCameraPose(command);}catch(ArgumentException){rejected=true;}if(!rejected)throw new Exception("Invalid SDK pose entered host.");}
        bool lens=false;try{_=context.GetCameraLens(entity);}catch(ArgumentException error){lens=error.Message.Contains("Declare ICameraLensGame");}
        if(!lens)throw new Exception("Camera pose marker granted camera lens.");++state.Count;
    }
}
[GameModule("poima-test-camera-pose-constructor")]
public sealed class PoseConstructorProbe:Game<ProbeState>,ICameraPoseGame
{
    public PoseConstructorProbe(){throw new Exception("POSE CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){}
    public override void Tick(ref ProbeState state,GameContext context){}
}
[GameModule("poima-test-camera-pose-unmarked")]
public sealed class PoseUnmarkedProbe:Game<ProbeState>
{
    public override void Initialize(ref ProbeState state){state.Count=3300;}
    public override void Tick(ref ProbeState state,GameContext context){_=context.GetCameraPose(new(11,70));++state.Count;}
}
[GameModule("poima-test-camera-pose-all")]
public sealed class PoseAllProbe:Game<ProbeState>,ICameraPoseGame,IAnimationRateGame,IGroundingControlGame,ICharacterStanceGame,ICameraLensGame,IRootMotionGame,
    IHitRegionsGame,IInputActionsGame,IPlayerPreferencesGame,IHierarchicalInstancesGame,INavigationGame,ICharacterInputGame,IMaskedAnimationGame
{
    public override void Initialize(ref ProbeState state){}
    public override void Tick(ref ProbeState state,GameContext context){}
}
static unsafe partial class Program
{
    static int poseReads,poseWrites,poseMode;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PoseGet(void* context,Id* entity,PoseWireState* output,void* error)
    {
        ++poseReads;if((nint)context!=0x1234 || output->Version!=1 || output->Bytes!=272 || output->Flags!=0 || entity->High!=11 || entity->Low<70 || entity->Low>72)badPayload=true;
        if(poseMode==1)return LifecycleError(error);
        *output=new(){Version=1,Bytes=272,Entity=*entity};
        if(entity->Low!=71){output->Present=1;output->Flags=entity->Low==70?1u:0u;output->World=new(){X=.1,Y=.2,Z=.3,Qw=1};if(entity->Low==70)output->Viewmodel.Qw=1;
            output->BaseWorld[0]=output->BaseWorld[5]=output->BaseWorld[10]=output->BaseWorld[15]=1;output->BaseWorld[12]=7;output->BaseWorld[13]=8;output->BaseWorld[14]=9;}
        switch(poseMode){
            case 2:output->Version=2;break;case 3:output->Bytes=271;break;case 4:output->Bytes=280;break;
            case 5:output->Present=2;break;case 6:output->Flags=2;break;case 7:output->Entity.Low=73;break;
            case 8:output->World.X=double.NaN;break;case 9:output->World.X=10.01;break;case 10:output->World.Qw=0;break;
            case 11:output->Viewmodel.Qw=double.PositiveInfinity;break;case 12:output->BaseWorld[0]=double.NaN;break;
            case 13:output->BaseWorld[15]=0;break;case 14:output->BaseWorld[3]=1;break;
            case 15:if(entity->Low==71)output->BaseWorld[0]=BitConverter.Int64BitsToDouble(long.MinValue);break;
            case 16:if(entity->Low==72)output->Viewmodel.Qw=1;break;case 17:output->Present=0;break;
        }return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PoseSet(void* context,PoseWireCommand* command,void* error)
    {
        ++poseWrites;if((nint)context!=0x1234 || command->Version!=1 || command->Bytes!=144 || command->Entity.High!=11 || command->Entity.Low!=70 || command->Channels!=3 || command->Reserved!=0 ||
            command->World.X!=.4 || command->World.Y!=.5 || command->World.Z!=.6 || command->World.Qx!=0 || command->World.Qy!=0 || command->World.Qz!=0 || command->World.Qw!=1 ||
            command->Viewmodel.X!=-.1 || command->Viewmodel.Y!=-.2 || command->Viewmodel.Z!=-.3 || command->Viewmodel.Qx!=0 || command->Viewmodel.Qy!=0 || command->Viewmodel.Qz!=0 || command->Viewmodel.Qw!=1)badPayload=true;
        return poseMode==18?LifecycleError(error):0;
    }
    static JsonElement PoseHost(uint bytes=392,bool pose=true,uint epoch=7,bool all=false)
    {
        var names=new List<string>{"baseline_v7"};if(pose)names.Add("camera_pose_v1");
        if(all)foreach(var item in RateHost(all:true).GetProperty("features").EnumerateArray()){var name=item.GetString()!;if(!names.Contains(name))names.Add(name);}
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void CameraPoseContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(PoseWireValue)==56 && sizeof(PoseWireCommand)==144 && sizeof(PoseWireState)==272 && sizeof(PoseWireServices)==392 &&
            Marshal.OffsetOf<PoseWireCommand>(nameof(PoseWireCommand.Entity)).ToInt64()==8 && Marshal.OffsetOf<PoseWireCommand>(nameof(PoseWireCommand.Channels)).ToInt64()==24 && Marshal.OffsetOf<PoseWireCommand>(nameof(PoseWireCommand.Reserved)).ToInt64()==28 &&
            Marshal.OffsetOf<PoseWireCommand>(nameof(PoseWireCommand.World)).ToInt64()==32 && Marshal.OffsetOf<PoseWireCommand>(nameof(PoseWireCommand.Viewmodel)).ToInt64()==88 &&
            Marshal.OffsetOf<PoseWireState>(nameof(PoseWireState.Entity)).ToInt64()==16 && Marshal.OffsetOf<PoseWireState>(nameof(PoseWireState.World)).ToInt64()==32 && Marshal.OffsetOf<PoseWireState>(nameof(PoseWireState.Viewmodel)).ToInt64()==88 && Marshal.OffsetOf<PoseWireState>(nameof(PoseWireState.BaseWorld)).ToInt64()==144 &&
            Marshal.OffsetOf<PoseWireServices>(nameof(PoseWireServices.Get)).ToInt64()==376 && Marshal.OffsetOf<PoseWireServices>(nameof(PoseWireServices.Set)).ToInt64()==384,"Independent camera pose layout differs.");
        Check(typeof(ControlContext).GetMethod("GetCameraPose")==null && typeof(ControlContext).GetMethod("SetCameraPose")==null,"Camera pose entered Control.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,PoseHost(176),PoseHost(376),PoseHost(384),PoseHost(391),PoseHost(pose:false),PoseHost(epoch:8)})
        {Check(LoadExtension(&call,typeof(PoseConstructorProbe),host)!=0 && !Output(output).Contains("POSE CONSTRUCTOR REACHED"),"Invalid pose host reached constructor.");NoExtensionModules(&call);}
        foreach(uint bytes in new[]{392u,400u}){Check(LoadExtension(&call,typeof(PoseConstructorProbe),PoseHost(bytes))!=0 && Output(output).Contains("POSE CONSTRUCTOR REACHED"),"Valid pose host denied constructor.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(PoseAllProbe),PoseHost(all:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output))){call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var requirements=doc.RootElement.GetProperty("requirements");Check(requirements.GetProperty("features").GetArrayLength()==17 && requirements.GetProperty("services_bytes").GetUInt32()==392,"All17 pose union differs.");var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;Check(Convert.ToUInt32(parser.Invoke(null,[requirements.GetRawText()]))==65535,"Pose generated union flags differ.");}
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(PoseProbe),PoseHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output))){call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var req=doc.RootElement.GetProperty("requirements");Check(req.GetProperty("services_bytes").GetUInt32()==392 && req.GetProperty("features").EnumerateArray().Select(x=>x.GetString()).SequenceEqual(new[]{"baseline_v7","camera_pose_v1"}),"Pose declaration grants unrelated features.");}
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        PoseWireServices services=default;var baseline=&services.Rate.Grounding.Orientation.Stance.Lens.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;*baseline=good;baseline->Bytes=392;services.Get=&PoseGet;services.Set=&PoseSet;
        new Span<byte>((byte*)&services+176,200).Fill(0xa5);poseReads=poseWrites=poseMode=0;call.Operation=3;
        foreach(int bytes in new[]{176,192,320,336,352,368,376,384,391}){using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && poseReads==0 && poseWrites==0 && ((ProbeState*)state)->Count==3200,"Protected short pose tail accessed.");}
        for(int missing=0;missing<2;++missing){var absent=services;if(missing==0)absent.Get=null;else absent.Set=null;call.Services=(Services*)(&absent);Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && poseReads==0 && poseWrites==0,"Missing pose callback entered game.");}
        call.Services=baseline;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==3201 && poseReads==4 && poseWrites==1 && !badPayload,Output(output));
        for(int mode=1;mode<=18;++mode){poseMode=mode;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==3201,"Malformed/error camera pose accepted: "+mode);if(mode is 1 or 18)Check(Output(output).Contains("lifecycle rejected"),"Camera pose native error lost.");}
        poseMode=0;call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(PoseUnmarkedProbe),null)==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(176)){*(Services*)guard.Header=good;call.Services=(Services*)guard.Header;call.Operation=3;int before=poseReads;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare ICameraPoseGame") && poseReads==before && ((ProbeState*)state)->Count==3300,"Unmarked game accessed pose tail.");}
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("camera_pose392/CMD144/STATE272 independent wire layout,17feature union, pre-construction/protected-short-tail/marker rights");
        checks.Add("committed world/viewmodel/base matrix transfer, staged invisibility, canonical absence, malformed state/command bounds and retained compiled state on callback errors");
    }
}
