// SPDX-License-Identifier: Apache-2.0
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

// Independently declared wire layouts. These mock callbacks exercise the SDK
// and real collectible bridge, not malformed inputs to Runtime's callbacks.
[StructLayout(LayoutKind.Sequential)] unsafe struct LayerServices
{
    public AnimationServices Animation;
    public delegate* unmanaged[Cdecl]<void*,Id*,uint,LayerAnimation*,void*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,LayerCommand*,void*,int> Set;
}
[StructLayout(LayoutKind.Sequential)] struct LayerCommand
{
    public uint Version,Bytes;public Command Command;public uint Mode,Slot;
    public double Weight;public uint WeightTicks,Reserved;
}
[StructLayout(LayoutKind.Sequential)] struct LayerAnimation
{
    public uint Version,Bytes;public Animation State;
    public uint Mode,Slot,LayerMode,MaskNodes;public double Weight,TargetWeight;
    public ulong WeightStart;public uint WeightDuration,WeightElapsed;
    public double WeightSource,WeightTarget;public uint WeightPresent,Reserved;
}
[GameModule("poima-test-layer-services")]
public sealed class LayerProbeGame : Game<ProbeState>,IMaskedAnimationGame
{
    public override void Initialize(ref ProbeState state) {state.Count=40;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        var layer=context.GetAnimationLayer(new(11,31),1) ?? throw new Exception("Missing layer.");
        if(layer.Slot!=1 || layer.Mode!=AnimationLayerMode.Additive || layer.MaskNodes!=2 ||
            layer.Weight!=.5 || layer.TargetWeight!=1 || layer.Playback.Mode!=AnimationTransitionMode.Inertial ||
            layer.Playback.Progress!=.75 || layer.Playback.State.Clip!=3 ||
            layer.WeightTransition is not {} fade || fade.StartTick!=113 || fade.DurationTicks!=20 ||
            fade.ElapsedTicks!=10 || fade.Source!=0 || fade.Target!=1)
            throw new Exception("Layer wire payload differs from independent reference.");
        if(context.GetAnimationLayer(new(11,31),2)!=null || context.GetAnimationLayer(new(11,34),1)!=null)
            throw new Exception("Missing valid slot/nonrig must return null.");
        context.SetAnimationLayer(new(11,31),1,2,.7,AnimationTransitionMode.Inertial,.2,.8,false,true,17,13);
        // Invalid public values must reject before invoking a mock native slot.
        bool rejected=false;try{context.GetAnimationLayer(new(11,31),0);}catch(ArgumentOutOfRangeException){rejected=true;}
        if(!rejected)throw new Exception("Invalid query slot accepted.");
        rejected=false;try{context.SetAnimationLayer(new(11,31),5,2,.5);}catch(ArgumentOutOfRangeException){rejected=true;}
        if(!rejected)throw new Exception("Invalid command slot accepted.");
        rejected=false;try{context.SetAnimationLayer(new(11,31),1,2,double.NaN);}catch(ArgumentOutOfRangeException){rejected=true;}
        if(!rejected)throw new Exception("Invalid weight accepted.");
        rejected=false;try{context.SetAnimationLayer(new(11,31),1,2,.5,(AnimationTransitionMode)2);}catch(ArgumentOutOfRangeException){rejected=true;}
        if(!rejected)throw new Exception("Invalid transition mode accepted.");
        ++state.Count;
    }
    public override void Control(ref ProbeState state,ControlContext context){++state.Count;}
}
[GameModule("poima-test-layer-constructor-gate")]
public sealed class LayerConstructorProbe : Game<ProbeState>,IMaskedAnimationGame
{
    public LayerConstructorProbe(){throw new Exception("LAYER CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){throw new Exception("LAYER INITIALIZE REACHED");}
    public override void Tick(ref ProbeState state,GameContext context){throw new Exception("LAYER TICK REACHED");}
}
[GameModule("poima-test-unmarked-layer-helper")]
public sealed class UnmarkedLayerProbe : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state){state.Count=50;}
    public override void Tick(ref ProbeState state,GameContext context){_=context.GetAnimationLayer(new(11,31),1);++state.Count;}
}
static unsafe partial class Program
{
    static int layerGets,layerSets,malformedLayer;static bool layerError;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int LayerGet(void* context,Id* id,uint slot,LayerAnimation* output,void* error)
    {
        ++layerGets;
        if((nint)context!=0x1234 || output->Version!=1 || output->Bytes!=200 || output->Reserved!=0)badPayload=true;
        if(layerError){Encoding.UTF8.GetBytes("layer rejected\0",new Span<byte>(error,2048));return -1;}
        bool present=id->Low==31 && slot==1;
        *output=new(){Version=1,Bytes=200,Slot=slot,State=new(){Entity=*id,Clip=-1,Transition=new(){Clip=-1}}};
        if(present){
            output->State=new(){Entity=*id,Present=1,Clip=3,Time=.125,Speed=2,Duration=4,Loop=1,TransitionPresent=1,
                Transition=new(){Start=99,Duration=32,Elapsed=24,Weight=.75,Clip=-1,Frozen=1}};
            output->Mode=1;output->LayerMode=1;output->MaskNodes=2;output->Weight=.5;output->TargetWeight=1;
            output->WeightStart=113;output->WeightDuration=20;output->WeightElapsed=10;
            output->WeightSource=0;output->WeightTarget=1;output->WeightPresent=1;
        }
        switch(malformedLayer){
            case 1:output->Version=2;break;
            case 2:output->Bytes=199;break;
            case 3:output->Reserved=1;break;
            case 4:output->State.Entity.Low=999;break;
            case 5:output->Slot=2;break;
            case 6:output->LayerMode=2;break;
            case 7:output->Mode=2;break;
            case 8:output->State.Present=2;break;
            case 9:output->State.Reserved=1;break;
            case 10:output->Weight=double.NaN;break;
            case 11:output->Weight=-.1;break;
            case 12:output->TargetWeight=1.1;break;
            case 13:output->WeightPresent=2;break;
            case 14:output->WeightDuration=0;break;
            case 15:output->WeightDuration=3601;break;
            case 16:output->WeightElapsed=20;break;
            case 17:output->WeightSource=double.NaN;break;
            case 18:output->WeightTarget=.9;break;
            case 19:output->Weight=.25;break;
            case 20:output->WeightPresent=0;break;
            case 21:output->State.Present=0;break;
            case 22:output->State.TransitionPresent=0;break;
            case 23:output->State.TransitionPresent=2;break;
            case 24:output->State.Transition.Duration=0;break;
            case 25:output->State.Transition.Elapsed=32;break;
            case 26:output->State.Transition.Weight=double.NaN;break;
            case 27:output->State.Transition.Weight=.5;break;
            case 28:output->State.Transition.Frozen=0;break;
            case 29:output->State.Time=double.NaN;break;
            case 30:output->State.Speed=double.PositiveInfinity;break;
            case 31:output->State.Loop=2;break;
            case 32:output->State.Playing=2;break;
            case 33:output->State.Clip=-2;break;
            case 34:output->MaskNodes=10001;break;
        }
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int LayerSet(void* context,LayerCommand* value,void* error)
    {
        ++layerSets;var c=value->Command;
        if((nint)context!=0x1234 || value->Version!=1 || value->Bytes!=80 || value->Reserved!=0 ||
            value->Mode!=1 || value->Slot!=1 || value->Weight!=.7 || value->WeightTicks!=13 ||
            c.Entity.High!=11 || c.Entity.Low!=31 || c.Clip!=2 || c.Time!=.2 || c.Speed!=.8 ||
            c.Loop!=0 || c.Playing!=1 || c.BlendTicks!=17)badPayload=true;
        return 0;
    }
    static JsonElement LayerHost(uint bytes=208,bool inertia=true,bool layers=true)
        =>JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=7,services_bytes=bytes,
            features=new[]{"baseline_v7",inertia?"animation_inertial_v1":null,layers?"animation_layers_v1":null}.Where(v=>v!=null).ToArray()});
    static void AnimationLayerContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(LayerServices)==208 && sizeof(LayerCommand)==80 && sizeof(LayerAnimation)==200 &&
            Marshal.OffsetOf<LayerServices>(nameof(LayerServices.Get)).ToInt64()==192 &&
            Marshal.OffsetOf<LayerServices>(nameof(LayerServices.Set)).ToInt64()==200 &&
            Marshal.OffsetOf<LayerCommand>(nameof(LayerCommand.Weight)).ToInt64()==64 &&
            Marshal.OffsetOf<LayerCommand>(nameof(LayerCommand.Reserved)).ToInt64()==76 &&
            Marshal.OffsetOf<LayerAnimation>(nameof(LayerAnimation.Weight)).ToInt64()==144 &&
            Marshal.OffsetOf<LayerAnimation>(nameof(LayerAnimation.WeightPresent)).ToInt64()==192,
            "Independent masked animation ABI layout mismatch.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,LayerHost(176),LayerHost(192),LayerHost(200),LayerHost(207),LayerHost(inertia:false),LayerHost(layers:false)}){
            Check(LoadExtension(&call,typeof(LayerConstructorProbe),host)!=0 && !Output(output).Contains("LAYER CONSTRUCTOR REACHED"),
                "Missing layer negotiation reached constructor.");NoExtensionModules(&call);
        }
        Check(LoadExtension(&call,typeof(LayerConstructorProbe),LayerHost())!=0 && Output(output).Contains("LAYER CONSTRUCTOR REACHED"),
            "Valid208 negotiation did not reach constructor sentinel.");NoExtensionModules(&call);
        checks.Add("masked CoreCLR marker rejects missing/short/unnamed/dependency-incomplete hosts before constructor;208 positive sentinel executes");
        Check(LoadExtension(&call,typeof(LayerProbeGame),LayerHost())==0,Output(output));
        using(var manifest=JsonDocument.Parse(Output(output))){
            call.Handle=manifest.RootElement.GetProperty("handle").GetUInt64();var r=manifest.RootElement.GetProperty("requirements");
            Check(r.GetProperty("services_bytes").GetUInt32()==208 && r.GetProperty("features").EnumerateArray().Select(v=>v.GetString()).Order().SequenceEqual(new[]{"animation_inertial_v1","animation_layers_v1","baseline_v7"}),
                "Masked manifest requirement differs from208 profile.");
        }
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==40,Output(output));
        LayerServices services=new(){Animation=new(){Baseline=good,Get=&ExtendedGet,Set=&ExtendedSet},Get=&LayerGet,Set=&LayerSet};services.Animation.Baseline.Bytes=208;
        foreach(uint operation in new uint[]{3,6}){
            call.Operation=operation;call.InputCount=0;call.Inputs=null;
            foreach(int size in new[]{176,192,200}){
                using var guarded=new GuardedHeader(size);*(Services*)guarded.Header=good;((Services*)guarded.Header)->Bytes=(uint)size;
                call.Services=(Services*)guarded.Header;int before=((ProbeState*)state)->Count,reads=layerGets,writes=layerSets;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0,"Short masked services touched protected tail or entered game.");
                Check(((ProbeState*)state)->Count==before && reads==layerGets && writes==layerSets,"Short masked service entered game.");
            }
            for(int slot=0;slot<2;++slot){
                var missing=services;if(slot==0)missing.Get=null;else missing.Set=null;call.Services=&missing.Animation.Baseline;
                int before=((ProbeState*)state)->Count,reads=layerGets,writes=layerSets;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0,"Missing masked callback accepted.");
                Check(((ProbeState*)state)->Count==before && reads==layerGets && writes==layerSets,"Missing masked callback entered game.");
            }
        }
        checks.Add("masked Tick/Control reject176/192/200 real guarded allocations and either missing layer callback before game execution");
        call.Services=&services.Animation.Baseline;call.Operation=3;layerGets=0;layerSets=0;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==41 && layerGets==3 && layerSets==1 && !badPayload,Output(output));
        call.Operation=6;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==42,Output(output));
        checks.Add("independent80/200 mock PODs transfer additive metadata, active clip inertia and weight fade, canonical missing slots and complete layer command; invalid public arguments do not invoke callbacks");
        call.Operation=3;
        for(int mutation=1;mutation<=34;++mutation){
            malformedLayer=mutation;int before=((ProbeState*)state)->Count,reads=layerGets,writes=layerSets;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0,"Malformed layer reply accepted: "+mutation);
            Check(((ProbeState*)state)->Count==before && layerGets==reads+1 && layerSets==writes,"Malformed layer reply continued gameplay.");
        }
        malformedLayer=0;layerError=true;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output)=="layer rejected","Layer native error not preserved.");layerError=false;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==43 && !badPayload,"Layer mock error recovery failed.");
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("SDK rejects34 independently malformed mock layer replies and transfers native error; valid subsequent Tick and collectible cleanup pass");
        Check(LoadExtension(&call,typeof(UnmarkedLayerProbe),null)==0,Output(output));
        using(var manifest=JsonDocument.Parse(Output(output)))call.Handle=manifest.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guarded=new GuardedHeader(176)){
            *(Services*)guarded.Header=good;((Services*)guarded.Header)->Bytes=176;call.Services=(Services*)guarded.Header;call.Operation=3;
            int reads=layerGets;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==50 && layerGets==reads,
                "Unmarked layer helper read unnegotiated protected tail.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("unmarked SDK layer helper rejects its actual176 guarded baseline without reading the208 tail");
    }
}
