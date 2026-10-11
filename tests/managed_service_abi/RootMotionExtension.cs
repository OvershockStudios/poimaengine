// SPDX-License-Identifier: Apache-2.0
// Independent PODs guard the appended profile and committed-state transfer.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;
[StructLayout(LayoutKind.Sequential)] struct RootTransferCommand
{
    public uint Version,Bytes;public Id Entity;public uint Enabled,Reserved;
}
[StructLayout(LayoutKind.Sequential)] struct RootTransferState
{
    public uint Version,Bytes,Present,Reserved;public Id Entity,Actor;
    public uint Node,Mode,Enabled,Yaw;public ulong FromTick,ToTick;
    public Vector3d LocalDelta;public double YawDelta;public Vector3d WorldRequested,WorldApplied;
    public double AppliedYaw,TranslationError;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct RootServices
{
    public IkServices Ik;
    public delegate* unmanaged[Cdecl]<void*,Id*,RootTransferState*,void*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,RootTransferCommand*,void*,int> Set;
}
[GameModule("poima-test-root")]
public sealed class RootProbeGame:Game<ProbeState>,IRootMotionGame
{
    public override void Initialize(ref ProbeState state){state.Count=1700;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        bool denied=false;try{_=context.GetTwoBoneIk(new(11,40),1);}catch(ArgumentException e){denied=e.Message.Contains("Declare ITwoBoneIkGame");}
        if(!denied)throw new Exception("Root marker granted unrelated IK.");
        EntityId entity=new(11,40);var before=context.GetRootMotion(entity) ?? throw new Exception("Missing root motion.");
        if(before.Entity!=entity || before.Actor!=new EntityId(11,42) || before.Node!=2 || before.Mode!=RootMotionMode.Character ||
            !before.Enabled || !before.Yaw || before.FromTick!=122 || before.ToTick!=123 || before.LocalDelta!=new Vector3d(.5,0,0) || before.YawDelta!=.125 ||
            before.WorldRequested!=new Vector3d(.5,0,0) || before.WorldApplied!=new Vector3d(.25,-.5,0) || before.AppliedYaw!=.125 || before.TranslationError!=.25)
            throw new Exception("Committed root transfer mismatch.");
        context.SetRootMotion(entity);
        if(context.GetRootMotion(entity)!=before)throw new Exception("Root setter altered committed reply.");
        if(context.GetRootMotion(new(11,41))!=null)throw new Exception("Absent root state was not null.");
        var extract=context.GetRootMotion(new(11,43)) ?? throw new Exception("Missing Extract state.");
        if(extract.Mode!=RootMotionMode.Extract || extract.Actor!=default || extract.WorldRequested!=new Vector3d(.5,.5,0) || extract.WorldApplied!=default || extract.AppliedYaw!=0 || extract.TranslationError!=0)
            throw new Exception("Projected Extract transfer mismatch.");
        var disabled=context.GetRootMotion(new(11,44)) ?? throw new Exception("Missing disabled state.");
        if(disabled.Enabled || disabled.LocalDelta!=default || disabled.WorldRequested!=default || disabled.WorldApplied!=default || disabled.YawDelta!=0 || disabled.AppliedYaw!=0 || disabled.TranslationError!=0)
            throw new Exception("Disabled cached diagnostics not neutral.");
        ++state.Count;
    }
}
[GameModule("poima-test-root-constructor")]
public sealed class RootConstructorProbe:Game<ProbeState>,IRootMotionGame
{
    public RootConstructorProbe(){throw new Exception("ROOT CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){ }
    public override void Tick(ref ProbeState state,GameContext context){ }
}
[GameModule("poima-test-root-all")]
public sealed class RootAllFeaturesProbe:Game<ProbeState>,IRootMotionGame,ITwoBoneIkGame,IHitRegionsGame,IInputActionsGame,IPlayerPreferencesGame,
    IHierarchicalInstancesGame,INavigationGame,ICharacterInputGame,IMaskedAnimationGame
{
    public override void Initialize(ref ProbeState state){ }
    public override void Tick(ref ProbeState state,GameContext context){ }
}
[GameModule("poima-test-root-unmarked")]
public sealed class RootUnmarkedProbe:Game<ProbeState>
{
    public override void Initialize(ref ProbeState state){state.Count=1800;}
    public override void Tick(ref ProbeState state,GameContext context){_=context.GetRootMotion(new(11,40));++state.Count;}
}
static unsafe partial class Program
{
    static int rootReads,rootWrites,rootMode;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ReadRoot(void* context,Id* entity,RootTransferState* output,void* error)
    {
        ++rootReads;
        if((nint)context!=0x1234 || entity->High!=11 || (entity->Low!=40 && entity->Low!=41 && entity->Low!=43 && entity->Low!=44) || output->Version!=1 || output->Bytes!=176 || output->Reserved!=0)badPayload=true;
        if(rootMode==1)return LifecycleError(error);
        *output=new(){Version=1,Bytes=176};
        if(entity->Low==40)*output=new(){Version=1,Bytes=176,Present=1,Entity=*entity,Actor=new(){High=11,Low=42},
            Node=2,Mode=1,Enabled=1,Yaw=1,FromTick=122,ToTick=123,LocalDelta=new(.5,0,0),YawDelta=.125,WorldRequested=new(.5,0,0),WorldApplied=new(.25,-.5,0),AppliedYaw=.125,TranslationError=.25};
        if(entity->Low==43)*output=new(){Version=1,Bytes=176,Present=1,Entity=*entity,Node=2,Mode=0,Enabled=1,FromTick=122,ToTick=123,LocalDelta=new(.5,0,0),WorldRequested=new(.5,.5,0)};
        if(entity->Low==44)*output=new(){Version=1,Bytes=176,Present=1,Entity=*entity,Actor=new(){High=11,Low=42},Node=2,Mode=1,Enabled=0,Yaw=1,FromTick=122,ToTick=123};
        switch(rootMode){
            case 2:output->Version=2;break;case 3:output->Bytes=175;break;case 4:output->Bytes=184;break;
            case 5:output->Reserved=1;break;case 6:output->Present=2;break;case 7:output->Present=0;break;
            case 8:output->Entity.Low=41;break;case 9:output->Node=10000;break;case 10:output->Mode=2;break;
            case 11:output->Enabled=2;break;case 12:output->Yaw=2;break;case 13:output->FromTick=124;break;
            case 14:output->FromTick=121;break;case 15:output->LocalDelta=new(double.NaN,0,0);break;
            case 16:output->LocalDelta=new(0,1,0);break;case 17:output->WorldRequested=new(0,1,0);break;
            case 18:output->WorldApplied=new(0,0,double.PositiveInfinity);break;case 19:output->YawDelta=double.NaN;break;
            case 20:output->AppliedYaw=double.PositiveInfinity;break;case 21:output->TranslationError=-1;break;
            case 22:output->TranslationError=double.NaN;break;case 23:output->Yaw=0;break;
            case 24:output->Enabled=0;break;case 25:output->Actor=default;break;
            case 26:output->Mode=0;break;case 28:output->FromTick=output->ToTick;break;
        }
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int WriteRoot(void* context,RootTransferCommand* command,void* error)
    {
        ++rootWrites;
        if((nint)context!=0x1234 || command->Version!=1 || command->Bytes!=32 || command->Reserved!=0 || command->Entity.High!=11 || command->Entity.Low!=40 || command->Enabled!=1)badPayload=true;
        return rootMode==27 ? LifecycleError(error) : 0;
    }
    static JsonElement RootHost(uint bytes=304,bool root=true,uint epoch=7,bool all=false)
    {
        var names=new List<string>{"baseline_v7"};if(root)names.Add("animation_root_motion_v1");
        if(all)names.AddRange(["animation_ik_v1","animation_inertial_v1","animation_layers_v1","character_input_v1","navigation_query_v1","hierarchical_instances_v1","player_preferences_v1","input_actions_v1","hit_regions_v1"]);
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void RootMotionContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(RootServices)==304 && sizeof(RootTransferCommand)==32 && sizeof(RootTransferState)==176 &&
            Marshal.OffsetOf<RootServices>(nameof(RootServices.Get)).ToInt64()==288 && Marshal.OffsetOf<RootServices>(nameof(RootServices.Set)).ToInt64()==296 &&
            Marshal.OffsetOf<RootTransferCommand>(nameof(RootTransferCommand.Entity)).ToInt64()==8 && Marshal.OffsetOf<RootTransferCommand>(nameof(RootTransferCommand.Enabled)).ToInt64()==24 &&
            Marshal.OffsetOf<RootTransferState>(nameof(RootTransferState.Actor)).ToInt64()==32 && Marshal.OffsetOf<RootTransferState>(nameof(RootTransferState.FromTick)).ToInt64()==64 &&
            Marshal.OffsetOf<RootTransferState>(nameof(RootTransferState.LocalDelta)).ToInt64()==80 && Marshal.OffsetOf<RootTransferState>(nameof(RootTransferState.YawDelta)).ToInt64()==104 &&
            Marshal.OffsetOf<RootTransferState>(nameof(RootTransferState.WorldRequested)).ToInt64()==112 && Marshal.OffsetOf<RootTransferState>(nameof(RootTransferState.WorldApplied)).ToInt64()==136 &&
            Marshal.OffsetOf<RootTransferState>(nameof(RootTransferState.AppliedYaw)).ToInt64()==160 && Marshal.OffsetOf<RootTransferState>(nameof(RootTransferState.TranslationError)).ToInt64()==168,"Independent root ABI layout mismatch.");
        Check(typeof(ControlContext).GetMethod("GetRootMotion")==null && typeof(ControlContext).GetMethod("SetRootMotion")==null,"Root control leaked into Control.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,RootHost(176),RootHost(288),RootHost(303),RootHost(root:false),RootHost(epoch:8)})
        {Check(LoadExtension(&call,typeof(RootConstructorProbe),host)!=0 && !Output(output).Contains("ROOT CONSTRUCTOR REACHED"),"Invalid root host reached constructor.");NoExtensionModules(&call);}
        foreach(uint bytes in new[]{304u,312u})
        {Check(LoadExtension(&call,typeof(RootConstructorProbe),RootHost(bytes))!=0 && Output(output).Contains("ROOT CONSTRUCTOR REACHED"),"Valid root host denied constructor.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(RootAllFeaturesProbe),RootHost())!=0,"Root granted intermediate features.");NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(RootAllFeaturesProbe),RootHost(all:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var declaration=doc.RootElement.GetProperty("requirements");
            Check(declaration.GetProperty("features").GetArrayLength()==11 && declaration.GetProperty("services_bytes").GetUInt32()==304,"Root lost eleventh named feature.");
            var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;
            Check(Convert.ToUInt32(parser.Invoke(null,[declaration.GetRawText()]))==1023,"Generated root requirement parser lost flags.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(RootProbeGame),RootHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output))){call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();Check(doc.RootElement.GetProperty("requirements").GetProperty("features").EnumerateArray().Select(v=>v.GetString()).SequenceEqual(new[]{"animation_root_motion_v1","baseline_v7"}),"Root gained unrelated permissions.");}
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        RootServices services=default;services.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline=good;
        services.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline.Bytes=304;services.Get=&ReadRoot;services.Set=&WriteRoot;
        new Span<byte>((byte*)&services+176,112).Fill(0xa5);rootReads=rootWrites=rootMode=0;call.Operation=3;
        foreach(int bytes in new[]{176,192,208,216,224,232,256,264,272,280,288,296,303})
        {
            using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==1700 && rootReads==0 && rootWrites==0,"Protected short root tail accessed.");
        }
        for(int missing=0;missing<2;++missing)
        {
            var absent=services;if(missing==0)absent.Get=null;else absent.Set=null;
            call.Services=&absent.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && rootReads==0 && rootWrites==0,"Missing root callback entered game.");
        }
        call.Services=&services.Ik.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==1701 && rootReads==5 && rootWrites==1 && !badPayload,Output(output));
        for(int mode=1;mode<=28;++mode)
        {
            rootMode=mode;int before=rootReads;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==1701 && rootReads==before+1,"Malformed/error root transfer succeeded: "+mode);
            if(mode is 1 or 27)Check(Output(output).Contains("lifecycle rejected"),"Root native error lost.");
        }
        rootMode=0;call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(RootUnmarkedProbe),null)==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(176))
        {
            *(Services*)guard.Header=good;call.Services=(Services*)guard.Header;call.Operation=3;int before=rootReads;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare IRootMotionGame") && ((ProbeState*)state)->Count==1800 && rootReads==before,"Unmarked root accessed protected tail.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("independent root304 CMD32 STATE176: eleven-feature negotiation, protected/poisoned tails, committed immutable Character diagnostics, canonical absent/malformed/error transfer, Tick-only enable");
    }
}
