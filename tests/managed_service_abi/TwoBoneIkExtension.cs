// SPDX-License-Identifier: Apache-2.0
// Independent PODs exercise negotiation, protected tails and complete transfer.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)] struct IkTransferCommand
{
    public uint Version,Bytes;public Id Entity;public uint Slot,Enabled,Space,Reserved;
    public double Weight;public Vector3d Target,Pole;
}
[StructLayout(LayoutKind.Sequential)] struct IkTransferState
{
    public uint Version,Bytes,Present,Slot;public Id Entity;public uint Root,Middle,End,Enabled,Space,Reserved;
    public double Weight;public Vector3d Target,Pole,EndPosition;public double TargetError;
    public uint Clamped,PoleFallback;public ulong ReservedTail;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct IkServices
{
    public RegionServices Regions;
    public delegate* unmanaged[Cdecl]<void*,Id*,uint,IkTransferState*,void*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,IkTransferCommand*,void*,int> Set;
}
[GameModule("poima-test-ik")]
public sealed class IkProbeGame:Game<ProbeState>,ITwoBoneIkGame
{
    public override void Initialize(ref ProbeState state){state.Count=1500;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        bool denied=false;try{_=context.HitTest(default,new(0,0,-1),1);}catch(ArgumentException e){denied=e.Message.Contains("Declare IHitRegionsGame");}
        if(!denied)throw new Exception("IK granted unrelated hit callback.");
        EntityId entity=new(11,40);var before=context.GetTwoBoneIk(entity,1) ?? throw new Exception("Missing IK state.");
        if(before.Entity!=entity || before.Slot!=1 || before.Root!=0 || before.Middle!=1 || before.End!=2 ||
            !before.Enabled || before.Space!=IkSpace.Rig || before.Weight!=.5 || before.Target!=new Vector3d(1,2,3) ||
            before.Pole!=new Vector3d(0,0,1) || before.EndPosition!=new Vector3d(1,2,2) || before.TargetError!=1 || !before.Clamped || before.PoleFallback)
            throw new Exception("IK committed transfer mismatch.");
        context.SetTwoBoneIk(entity,1,new(2,3,4),new(0,1,0),false,IkSpace.World,.25);
        if(context.GetTwoBoneIk(entity,1)!=before)throw new Exception("Setter altered committed reply.");
        if(context.GetTwoBoneIk(entity,2)!=null)throw new Exception("Absent chain was not null.");
        // Rejected local values cannot invoke a host callback.
        for(int kind=0;kind<7;++kind)
        {
            denied=false;try{
                switch(kind){
                    case 0:context.SetTwoBoneIk(entity,0,default,default);break;
                    case 1:context.SetTwoBoneIk(entity,9,default,default);break;
                    case 2:context.SetTwoBoneIk(entity,1,default,default,space:(IkSpace)2);break;
                    case 3:context.SetTwoBoneIk(entity,1,default,default,weight:double.NaN);break;
                    case 4:context.SetTwoBoneIk(entity,1,default,default,weight:1.01);break;
                    case 5:context.SetTwoBoneIk(entity,1,new(1e6+1,0,0),default);break;
                    case 6:context.SetTwoBoneIk(entity,1,default,new(0,double.PositiveInfinity,0));break;
                }
            }catch(ArgumentException){denied=true;}
            if(!denied)throw new Exception("Invalid IK command accepted.");
        }
        ++state.Count;
    }
}
[GameModule("poima-test-ik-constructor")]
public sealed class IkConstructorProbe:Game<ProbeState>,ITwoBoneIkGame
{
    public IkConstructorProbe(){throw new Exception("IK CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){ }
    public override void Tick(ref ProbeState state,GameContext context){ }
}
[GameModule("poima-test-ik-all")]
public sealed class IkAllFeaturesProbe:Game<ProbeState>,ITwoBoneIkGame,IHitRegionsGame,IInputActionsGame,IPlayerPreferencesGame,
    IHierarchicalInstancesGame,INavigationGame,ICharacterInputGame,IMaskedAnimationGame
{
    public override void Initialize(ref ProbeState state){ }
    public override void Tick(ref ProbeState state,GameContext context){ }
}
[GameModule("poima-test-ik-unmarked")]
public sealed class IkUnmarkedProbe:Game<ProbeState>
{
    public override void Initialize(ref ProbeState state){state.Count=1600;}
    public override void Tick(ref ProbeState state,GameContext context){_=context.GetTwoBoneIk(new(11,40),1);++state.Count;}
}
static unsafe partial class Program
{
    static int ikReads,ikWrites,ikMode;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ReadIk(void* context,Id* entity,uint slot,IkTransferState* output,void* error)
    {
        ++ikReads;
        if((nint)context!=0x1234 || entity->High!=11 || entity->Low!=40 || output->Version!=1 || output->Bytes!=160 || output->Reserved!=0 || output->ReservedTail!=0)badPayload=true;
        if(ikMode==1)return LifecycleError(error);
        *output=new(){Version=1,Bytes=160};
        if(slot!=2)*output=new(){Version=1,Bytes=160,Present=1,Slot=slot,Entity=*entity,Root=0,Middle=1,End=2,Enabled=1,Space=1,
            Weight=.5,Target=new(1,2,3),Pole=new(0,0,1),EndPosition=new(1,2,2),TargetError=1,Clamped=1};
        switch(ikMode){
            case 2:output->Version=2;break;case 3:output->Bytes=159;break;case 4:output->Bytes=168;break;
            case 5:output->Reserved=1;break;case 6:output->ReservedTail=1;break;case 7:output->Present=2;break;
            case 8:output->Present=0;break;case 9:output->Entity.Low=41;break;case 10:output->Slot=8;break;
            case 11:output->Middle=0;break;case 12:output->Enabled=2;break;case 13:output->Space=2;break;
            case 14:output->Weight=double.NaN;break;case 15:output->Weight=-.1;break;case 16:output->Weight=1.01;break;
            case 17:output->Target=new(double.NaN,0,0);break;case 18:output->Target=new(1e6+1,0,0);break;
            case 19:output->Pole=new(0,double.PositiveInfinity,0);break;case 20:output->EndPosition=new(0,0,double.NaN);break;
            case 21:output->TargetError=-1;break;case 22:output->TargetError=double.NaN;break;
            case 23:output->Clamped=2;break;case 24:output->PoleFallback=2;break;
        }
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int WriteIk(void* context,IkTransferCommand* command,void* error)
    {
        ++ikWrites;
        if((nint)context!=0x1234 || command->Version!=1 || command->Bytes!=96 || command->Reserved!=0 ||
            command->Entity.High!=11 || command->Entity.Low!=40 || command->Slot!=1 || command->Enabled!=0 || command->Space!=0 ||
            command->Weight!=.25 || command->Target!=new Vector3d(2,3,4) || command->Pole!=new Vector3d(0,1,0))badPayload=true;
        if(ikMode==25)return LifecycleError(error);return 0;
    }
    static JsonElement IkHost(uint bytes=288,bool ik=true,uint epoch=7,bool all=false)
    {
        var names=new List<string>{"baseline_v7"};if(ik)names.Add("animation_ik_v1");
        if(all)names.AddRange(["animation_inertial_v1","animation_layers_v1","character_input_v1","navigation_query_v1","hierarchical_instances_v1","player_preferences_v1","input_actions_v1","hit_regions_v1"]);
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void TwoBoneIkContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(IkServices)==288 && sizeof(IkTransferCommand)==96 && sizeof(IkTransferState)==160 &&
            Marshal.OffsetOf<IkServices>(nameof(IkServices.Get)).ToInt64()==272 && Marshal.OffsetOf<IkServices>(nameof(IkServices.Set)).ToInt64()==280 &&
            Marshal.OffsetOf<IkTransferCommand>(nameof(IkTransferCommand.Weight)).ToInt64()==40 && Marshal.OffsetOf<IkTransferCommand>(nameof(IkTransferCommand.Pole)).ToInt64()==72 &&
            Marshal.OffsetOf<IkTransferState>(nameof(IkTransferState.Entity)).ToInt64()==16 && Marshal.OffsetOf<IkTransferState>(nameof(IkTransferState.EndPosition)).ToInt64()==112 &&
            Marshal.OffsetOf<IkTransferState>(nameof(IkTransferState.ReservedTail)).ToInt64()==152,"Independent IK ABI layout mismatch.");
        Check(typeof(ControlContext).GetMethod("GetTwoBoneIk")==null && typeof(ControlContext).GetMethod("SetTwoBoneIk")==null,"IK leaked into Control.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,IkHost(176),IkHost(272),IkHost(287),IkHost(ik:false),IkHost(epoch:8)})
        {Check(LoadExtension(&call,typeof(IkConstructorProbe),host)!=0 && !Output(output).Contains("IK CONSTRUCTOR REACHED"),"Invalid IK host reached constructor.");NoExtensionModules(&call);}
        foreach(uint bytes in new[]{288u,296u})
        {Check(LoadExtension(&call,typeof(IkConstructorProbe),IkHost(bytes))!=0 && Output(output).Contains("IK CONSTRUCTOR REACHED"),"Valid IK host denied constructor.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(IkAllFeaturesProbe),IkHost())!=0,"IK granted intermediate features.");NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(IkAllFeaturesProbe),IkHost(all:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var declaration=doc.RootElement.GetProperty("requirements");
            Check(declaration.GetProperty("features").GetArrayLength()==10 && declaration.GetProperty("services_bytes").GetUInt32()==288,"IK lost tenth named feature.");
            var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;
            Check(Convert.ToUInt32(parser.Invoke(null,[declaration.GetRawText()]))==511,"Generated IK requirement parser lost flags.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(IkProbeGame),IkHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();Check(doc.RootElement.GetProperty("requirements").GetProperty("features").EnumerateArray().Select(v=>v.GetString()).SequenceEqual(new[]{"animation_ik_v1","baseline_v7"}),"IK-only game gained other permissions.");}
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        IkServices services=default;services.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline=good;
        services.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline.Bytes=288;services.Get=&ReadIk;services.Set=&WriteIk;
        new Span<byte>((byte*)&services+176,96).Fill(0xa5);ikReads=ikWrites=ikMode=0;call.Operation=3;
        foreach(int bytes in new[]{176,192,208,216,224,232,256,264,272,280,287})
        {
            using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==1500 && ikReads==0 && ikWrites==0,"Protected short IK tail was accessed.");
        }
        for(int missing=0;missing<2;++missing)
        {
            var absent=services;if(missing==0)absent.Get=null;else absent.Set=null;
            call.Services=&absent.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ikReads==0 && ikWrites==0,"Missing IK callback entered game.");
        }
        call.Services=&services.Regions.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==1501 && ikReads==3 && ikWrites==1 && !badPayload,Output(output));
        for(int mode=1;mode<=25;++mode)
        {
            ikMode=mode;int before=ikReads;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==1501 && ikReads==before+1,"Malformed/error IK transfer succeeded: "+mode);
            if(mode is 1 or 25)Check(Output(output).Contains("lifecycle rejected"),"Native IK error message lost.");
        }
        ikMode=0;call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(IkUnmarkedProbe),null)==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(176))
        {
            *(Services*)guard.Header=good;call.Services=(Services*)guard.Header;call.Operation=3;int before=ikReads;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare ITwoBoneIkGame") && ((ProbeState*)state)->Count==1600 && ikReads==before,"Unmarked IK touched protected tail.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("independent IK288 CMD96 STATE160: pre-construction negotiation, ten-feature parser, protected/poisoned tails, staged immutable reads, canonical absent and malformed/error transfer, Tick-only controls");
    }
}
