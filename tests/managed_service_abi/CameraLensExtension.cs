// SPDX-License-Identifier: Apache-2.0
// Independent transfer PODs preserve all historical shorter feature contracts.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;
[StructLayout(LayoutKind.Sequential)] struct LensValue { public double Fov,Near,Far; }
[StructLayout(LayoutKind.Sequential)] struct LensCommand
{ public uint Version,Bytes;public Id Entity;public uint Channels,Reserved;public LensValue World,Viewmodel; }
[StructLayout(LayoutKind.Sequential)] struct LensState
{ public uint Version,Bytes,Present,Flags;public Id Entity;public LensValue World,Viewmodel,Authored; }
[StructLayout(LayoutKind.Sequential)] unsafe struct LensServices
{
    public RootServices Root;
    public delegate* unmanaged[Cdecl]<void*,Id*,LensState*,void*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,LensCommand*,void*,int> Set;
}
[GameModule("poima-test-lens")]
public sealed class LensProbeGame:Game<ProbeState>,ICameraLensGame
{
    public override void Initialize(ref ProbeState state){state.Count=1900;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        bool denied=false;try{_=context.GetRootMotion(new(11,50));}catch(ArgumentException e){denied=e.Message.Contains("Declare IRootMotionGame");}
        if(!denied)throw new Exception("Camera marker granted root motion.");
        EntityId id=new(11,50);var before=context.GetCameraLens(id) ?? throw new Exception("Camera lens absent.");
        if(before.Entity!=id || before.World!=new CameraLens(40,.02,500) || before.Viewmodel!=new CameraLens(60,.01,10) || before.AuthoredWorld!=new CameraLens(60,.1,1000))throw new Exception("Committed camera transfer differs.");
        context.SetCameraLens(id,new CameraLens(30,.03,300),new CameraLens(50,.02,20));
        if(context.GetCameraLens(id)!=before)throw new Exception("Camera setter exposed staged values.");
        if(context.GetCameraLens(new(11,51))!=null)throw new Exception("Noncamera was not absent.");
        var worldOnly=context.GetCameraLens(new(11,52)) ?? throw new Exception("World-only camera absent.");
        if(worldOnly.Viewmodel!=null || worldOnly.World!=before.World || worldOnly.AuthoredWorld!=before.AuthoredWorld)throw new Exception("World-only camera transfer differs.");
        // Invalid commands must fail in the SDK without entering the host callback.
        foreach(var command in new[]{new CameraLensCommand(id),new CameraLensCommand(default,new(30,.03,300)),
            new CameraLensCommand(id,new(double.NaN,.03,300)),new CameraLensCommand(id,new(4,.03,300)),
            new CameraLensCommand(id,new(151,.03,300)),new CameraLensCommand(id,new(30,.0001,300)),
            new CameraLensCommand(id,new(30,300,300)),new CameraLensCommand(id,new(30,.03,10000001)),
            new CameraLensCommand(id,null,new(30,.03,double.PositiveInfinity))})
        {
            bool rejected=false;try{context.SetCameraLens(command);}catch(ArgumentException){rejected=true;}
            if(!rejected)throw new Exception("Invalid SDK lens command reached host.");
        }
        ++state.Count;
    }
}
[GameModule("poima-test-lens-constructor")]
public sealed class LensConstructorProbe:Game<ProbeState>,ICameraLensGame
{
    public LensConstructorProbe(){throw new Exception("LENS CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){ }
    public override void Tick(ref ProbeState state,GameContext context){ }
}
[GameModule("poima-test-lens-all")]
public sealed class LensAllFeaturesProbe:Game<ProbeState>,ICameraLensGame,IRootMotionGame,ITwoBoneIkGame,IHitRegionsGame,IInputActionsGame,
    IPlayerPreferencesGame,IHierarchicalInstancesGame,INavigationGame,ICharacterInputGame,IMaskedAnimationGame
{
    public override void Initialize(ref ProbeState state){ }
    public override void Tick(ref ProbeState state,GameContext context){ }
}
[GameModule("poima-test-lens-unmarked")]
public sealed class LensUnmarkedProbe:Game<ProbeState>
{
    public override void Initialize(ref ProbeState state){state.Count=2000;}
    public override void Tick(ref ProbeState state,GameContext context){_=context.GetCameraLens(new(11,50));++state.Count;}
}
static unsafe partial class Program
{
    static int lensReads,lensWrites,lensMode;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ReadLens(void* context,Id* entity,LensState* output,void* error)
    {
        ++lensReads;
        if((nint)context!=0x1234 || entity->High!=11 || entity->Low<50 || entity->Low>52 || output->Version!=1 || output->Bytes!=104 || output->Flags!=0)badPayload=true;
        if(lensMode==1)return LifecycleError(error);
        *output=new(){Version=1,Bytes=104};
        if(entity->Low!=51)*output=new(){Version=1,Bytes=104,Present=1,Flags=entity->Low==50?1u:0u,Entity=*entity,
            World=new(){Fov=40,Near=.02,Far=500},Viewmodel=entity->Low==50?new(){Fov=60,Near=.01,Far=10}:default,
            Authored=new(){Fov=60,Near=.1,Far=1000}};
        switch(lensMode){
            case 2:output->Version=2;break;case 3:output->Bytes=103;break;case 4:output->Bytes=112;break;
            case 5:output->Present=2;break;case 6:output->Flags=2;break;case 7:output->Present=0;break;
            case 8:output->Entity.Low=51;break;case 9:output->World.Fov=double.NaN;break;
            case 10:output->World.Fov=4;break;case 11:output->World.Fov=151;break;case 12:output->World.Near=.0001;break;
            case 13:output->World.Far=output->World.Near;break;case 14:output->World.Far=10000001;break;
            case 15:output->Authored.Fov=double.PositiveInfinity;break;case 16:output->Viewmodel.Near=0;break;
            case 17:output->Flags=0;break;case 18:output->Entity=default;break;
            case 19:if(entity->Low==51)output->Authored.Near=BitConverter.Int64BitsToDouble(long.MinValue);break;
            case 20:if(entity->Low==52)output->Viewmodel.Far=BitConverter.Int64BitsToDouble(long.MinValue);break;
        }
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int WriteLens(void* context,LensCommand* command,void* error)
    {
        ++lensWrites;
        if((nint)context!=0x1234 || command->Version!=1 || command->Bytes!=80 || command->Entity.High!=11 || command->Entity.Low!=50 ||
            command->Channels!=3 || command->Reserved!=0 || command->World.Fov!=30 || command->World.Near!=.03 || command->World.Far!=300 ||
            command->Viewmodel.Fov!=50 || command->Viewmodel.Near!=.02 || command->Viewmodel.Far!=20)badPayload=true;
        return lensMode==21?LifecycleError(error):0;
    }
    static JsonElement LensHost(uint bytes=320,bool lens=true,uint epoch=7,bool all=false)
    {
        var names=new List<string>{"baseline_v7"};if(lens)names.Add("camera_lens_v1");
        if(all)names.AddRange(["animation_root_motion_v1","animation_ik_v1","animation_inertial_v1","animation_layers_v1","character_input_v1","navigation_query_v1","hierarchical_instances_v1","player_preferences_v1","input_actions_v1","hit_regions_v1"]);
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void CameraLensContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(LensServices)==320 && sizeof(LensCommand)==80 && sizeof(LensState)==104 && sizeof(LensValue)==24 &&
            Marshal.OffsetOf<LensServices>(nameof(LensServices.Get)).ToInt64()==304 && Marshal.OffsetOf<LensServices>(nameof(LensServices.Set)).ToInt64()==312 &&
            Marshal.OffsetOf<LensCommand>(nameof(LensCommand.Entity)).ToInt64()==8 && Marshal.OffsetOf<LensCommand>(nameof(LensCommand.Channels)).ToInt64()==24 &&
            Marshal.OffsetOf<LensCommand>(nameof(LensCommand.World)).ToInt64()==32 && Marshal.OffsetOf<LensCommand>(nameof(LensCommand.Viewmodel)).ToInt64()==56 &&
            Marshal.OffsetOf<LensState>(nameof(LensState.Entity)).ToInt64()==16 && Marshal.OffsetOf<LensState>(nameof(LensState.World)).ToInt64()==32 &&
            Marshal.OffsetOf<LensState>(nameof(LensState.Viewmodel)).ToInt64()==56 && Marshal.OffsetOf<LensState>(nameof(LensState.Authored)).ToInt64()==80,"Independent camera lens layout differs.");
        Check(typeof(ControlContext).GetMethod("GetCameraLens")==null && typeof(ControlContext).GetMethod("SetCameraLens")==null,"Camera lenses leaked into Control.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,LensHost(176),LensHost(304),LensHost(312),LensHost(319),LensHost(lens:false),LensHost(epoch:8)})
        {Check(LoadExtension(&call,typeof(LensConstructorProbe),host)!=0 && !Output(output).Contains("LENS CONSTRUCTOR REACHED"),"Invalid lens host reached constructor.");NoExtensionModules(&call);}
        foreach(uint bytes in new[]{320u,328u})
        {Check(LoadExtension(&call,typeof(LensConstructorProbe),LensHost(bytes))!=0 && Output(output).Contains("LENS CONSTRUCTOR REACHED"),"Valid lens host denied constructor.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(LensAllFeaturesProbe),LensHost())!=0,"Camera marker granted intermediate features.");NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(LensAllFeaturesProbe),LensHost(all:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var requirements=doc.RootElement.GetProperty("requirements");
            Check(requirements.GetProperty("features").GetArrayLength()==12 && requirements.GetProperty("services_bytes").GetUInt32()==320,"Camera feature union lost names/prefix.");
            var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;
            Check(Convert.ToUInt32(parser.Invoke(null,[requirements.GetRawText()]))==2047,"Camera union lost flags.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(LensProbeGame),LensHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();Check(doc.RootElement.GetProperty("requirements").GetProperty("features").EnumerateArray().Select(v=>v.GetString()).SequenceEqual(new[]{"baseline_v7","camera_lens_v1"}),"Camera requirement gained unrelated features.");}
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        LensServices services=default;services.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline=good;
        services.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline.Bytes=320;services.Get=&ReadLens;services.Set=&WriteLens;
        new Span<byte>((byte*)&services+176,128).Fill(0xa5);lensReads=lensWrites=lensMode=0;call.Operation=3;
        foreach(int bytes in new[]{176,192,208,216,224,232,256,264,272,288,304,312,319})
        {
            using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==1900 && lensReads==0 && lensWrites==0,"Protected short lens tail accessed.");
        }
        for(int missing=0;missing<2;++missing)
        {
            var absent=services;if(missing==0)absent.Get=null;else absent.Set=null;
            call.Services=&absent.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && lensReads==0 && lensWrites==0,"Missing lens callback entered game.");
        }
        call.Services=&services.Root.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==1901 && lensReads==4 && lensWrites==1 && !badPayload,Output(output));
        for(int mode=1;mode<=21;++mode)
        {
            lensMode=mode;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==1901,"Malformed/error lens transfer succeeded: "+mode);
            if(mode is 1 or 21)Check(Output(output).Contains("lifecycle rejected"),"Camera native error lost.");
        }
        lensMode=0;call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(LensUnmarkedProbe),null)==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(176))
        {
            *(Services*)guard.Header=good;call.Services=(Services*)guard.Header;call.Operation=3;int before=lensReads;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare ICameraLensGame") && ((ProbeState*)state)->Count==2000 && lensReads==before,"Unmarked lens accessed protected tail.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("camera lens independent320 prefix, CMD80/STATE104 layouts, negotiated12-feature union and protected shorter tails");
        checks.Add("committed world/viewmodel/authored lenses, staged invisibility, canonical absence and bounded finite SDK commands");
        checks.Add("camera lens native errors/malformed transfers rollback state; unmarked and Control contexts do not grant access");
    }
}
