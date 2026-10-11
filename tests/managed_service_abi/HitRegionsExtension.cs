// SPDX-License-Identifier: Apache-2.0
// Independent declarations verify offsets and bounded feature access.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)] unsafe struct HitRay
{ public Vector3d Origin,Direction;public double Distance;public Id* Ignore;public uint IgnoreCount,Reserved; }
[StructLayout(LayoutKind.Sequential)] struct HitGeometry
{ public Id Entity;public double Fraction,Distance;public Vector3d Position,Normal;public uint Hit,NormalValid; }
[StructLayout(LayoutKind.Sequential)] unsafe struct RegionReply
{
    public uint Version,Bytes;public HitGeometry Hit;public Id RegionEntity;
    public uint RegionNameBytes,TrianglePresent,Triangle,Reserved;public fixed byte RegionName[64];
}
[StructLayout(LayoutKind.Sequential)] unsafe struct RegionServices
{
    public ActionServices Actions;
    public delegate* unmanaged[Cdecl]<void*,HitRay*,RegionReply*,void*,int> HitTest;
}
[GameModule("poima-test-hit-regions")]
public sealed class HitRegionProbeGame : Game<ProbeState>,IHitRegionsGame
{
    public override void Initialize(ref ProbeState state){state.Count=1200;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        bool denied=false;try{_=context.GetInputAction(new(11,40),"fire");}catch(ArgumentException e){denied=e.Message.Contains("Declare IInputActionsGame");}
        if(!denied)throw new Exception("Hit regions granted input actions.");
        denied=false;try{_=context.GetPlayerPreferences();}catch(ArgumentException e){denied=e.Message.Contains("Declare IPlayerPreferencesGame");}
        if(!denied)throw new Exception("Hit regions granted preferences.");
        denied=false;try{_=context.HitTest(default,new(0,0,-1),20,new EntityId[129]);}catch(ArgumentException){denied=true;}
        if(!denied)throw new Exception("Unbounded ignore array reached host.");
        var region=context.HitTest(new(1,2,3),new(0,0,-1),20,[new(11,90)]) ?? throw new Exception("Missing region.");
        if(region.Entity!=new EntityId(11,40) || region.RegionEntity!=new EntityId(11,41) || region.Region!="head" ||
            region.Fraction!=.25 || region.Distance!=5 || region.Position!=new Vector3d(1,2,-2) || region.Normal!=new Vector3d(0,1,0) || region.Triangle!=null)
            throw new Exception("Region result lost identity or geometry.");
        var geometry=context.HitTest(new(2,2,3),new(0,0,-1),20,[new(11,90)]) ?? throw new Exception("Missing geometry.");
        if(geometry.Entity!=new EntityId(11,42) || geometry.RegionEntity!=null || geometry.Region!=null ||
            geometry.Fraction!=.5 || geometry.Distance!=10 || geometry.Triangle!=7 || geometry.Normal!=null)
            throw new Exception("Geometry result lost optional triangle/normal.");
        if(context.HitTest(new(3,2,3),new(0,0,-1),20,[new(11,90)])!=null)throw new Exception("Miss was not null.");
        var boundary=context.HitTest(new(4,2,3),new(0,0,-1),20,[new(11,90)])!.Value;
        if(boundary.Region!=new string('a',64) || region.Region!="head")throw new Exception("Region name was not independently owned or bounded.");
        ++state.Count;
    }
}
[GameModule("poima-test-hit-constructor")]
public sealed class HitRegionConstructorProbe : Game<ProbeState>,IHitRegionsGame
{
    public HitRegionConstructorProbe(){throw new Exception("HIT CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){throw new Exception("HIT INITIALIZE REACHED");}
    public override void Tick(ref ProbeState state,GameContext context){throw new Exception("HIT TICK REACHED");}
}
[GameModule("poima-test-hit-all-features")]
public sealed class HitRegionAllFeaturesProbe : Game<ProbeState>,IHitRegionsGame,IInputActionsGame,IPlayerPreferencesGame,
    IHierarchicalInstancesGame,INavigationGame,ICharacterInputGame,IMaskedAnimationGame
{
    public override void Initialize(ref ProbeState state){state.Count=1300;}
    public override void Tick(ref ProbeState state,GameContext context){ }
}
[GameModule("poima-test-hit-unmarked")]
public sealed class HitRegionUnmarkedProbe : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state){state.Count=1400;}
    public override void Tick(ref ProbeState state,GameContext context){_=context.HitTest(default,new(0,0,-1),20);++state.Count;}
}
static unsafe partial class Program
{
    static int hitRegionCalls,hitRegionReplyMode;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ReadHitRegion(void* context,HitRay* ray,RegionReply* output,void* error)
    {
        ++hitRegionCalls;
        if((nint)context!=0x1234 || ray->Origin.Y!=2 || ray->Origin.Z!=3 || ray->Direction!=new Vector3d(0,0,-1) ||
            ray->Distance!=20 || ray->IgnoreCount!=1 || ray->Ignore==null || ray->Ignore[0].High!=11 || ray->Ignore[0].Low!=90 || ray->Reserved!=0 ||
            output->Version!=1 || output->Bytes!=192 || output->Reserved!=0)badPayload=true;
        if(hitRegionReplyMode==1)return LifecycleError(error);
        *output=new(){Version=1,Bytes=192};
        if(ray->Origin.X!=3)
        {
            output->Hit=new(){Entity=new(){High=11,Low=ray->Origin.X==2?42u:40u},Hit=1,Fraction=ray->Origin.X==2?.5:.25,
                Distance=ray->Origin.X==2?10:5,Position=new(ray->Origin.X,2,-2)};
            if(ray->Origin.X==2){output->TrianglePresent=1;output->Triangle=7;}
            else
            {
                output->Hit.NormalValid=1;output->Hit.Normal=new(0,1,0);output->RegionEntity=new(){High=11,Low=41};
                string name=ray->Origin.X==4?new('a',64):"head";output->RegionNameBytes=(uint)name.Length;
                for(int i=0;i<name.Length;++i)output->RegionName[i]=(byte)name[i];
            }
        }
        switch(hitRegionReplyMode)
        {
            case 2:output->Version=2;break;
            case 3:output->Bytes=191;break;
            case 4:output->Bytes=200;break;
            case 5:output->Reserved=1;break;
            case 6:output->Hit.Hit=2;break;
            case 7:output->Hit.NormalValid=2;break;
            case 8:output->TrianglePresent=2;break;
            case 9:output->RegionNameBytes=65;break;
            case 10:output->Hit.Fraction=double.NaN;break;
            case 11:output->Hit.Fraction=1.1;break;
            case 12:output->Hit.Distance=21;break;
            case 13:output->Hit.Position=new(double.PositiveInfinity,0,0);break;
            case 14:output->Hit.Normal=new(double.NaN,0,0);break;
            case 15:output->RegionEntity=default;break;
            case 16:output->RegionName[0]=(byte)'A';break;
            case 17:output->RegionName[0]=0x80;break;
            case 18:output->RegionName[4]=1;break;
            case 19:output->Hit.NormalValid=0;break;
            case 20:output->Triangle=7;break;
            case 21:output->TrianglePresent=1;break;
            case 22:output->Hit.Entity=default;break;
            case 23:output->Hit.Hit=0;break;
            case 24:output->RegionNameBytes=0;break;
            case 25:output->Hit.Distance=-1;break;
            case 26:output->Hit.Normal=default;break;
        }
        return 0;
    }
    static JsonElement HitRegionHost(uint bytes=272,bool regions=true,uint epoch=7,bool all=false)
    {
        var names=new List<string>{"baseline_v7"};if(regions)names.Add("hit_regions_v1");
        if(all)names.AddRange(["animation_inertial_v1","animation_layers_v1","character_input_v1","navigation_query_v1","hierarchical_instances_v1","player_preferences_v1","input_actions_v1"]);
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void HitRegionsContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(HitRay)==72 && sizeof(HitGeometry)==88 && sizeof(RegionReply)==192 && sizeof(RegionServices)==272 &&
            Marshal.OffsetOf<RegionServices>(nameof(RegionServices.HitTest)).ToInt64()==264 &&
            Marshal.OffsetOf<RegionReply>(nameof(RegionReply.Hit)).ToInt64()==8 &&
            Marshal.OffsetOf<RegionReply>(nameof(RegionReply.RegionEntity)).ToInt64()==96 &&
            Marshal.OffsetOf<RegionReply>(nameof(RegionReply.RegionNameBytes)).ToInt64()==112 &&
            Marshal.OffsetOf<RegionReply>(nameof(RegionReply.RegionName)).ToInt64()==128,"Independent hit region layout mismatch.");
        Check(typeof(ControlContext).GetMethod("HitTest")==null,"Tick-only hit test leaked into Control.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,HitRegionHost(176),HitRegionHost(264),HitRegionHost(271),HitRegionHost(regions:false),HitRegionHost(epoch:8),
            ExtensionJson("{\"call_version\":1,\"call_bytes\":80,\"services_version\":7,\"services_bytes\":272,\"features\":[\"baseline_v7\",\"hit_regions_v1\",\"hit_regions_v1\"]}")})
        {Check(LoadExtension(&call,typeof(HitRegionConstructorProbe),host)!=0 && !Output(output).Contains("HIT CONSTRUCTOR REACHED"),"Invalid hit host reached constructor.");NoExtensionModules(&call);}
        foreach(uint bytes in new[]{272u,280u})
        {Check(LoadExtension(&call,typeof(HitRegionConstructorProbe),HitRegionHost(bytes))!=0 && Output(output).Contains("HIT CONSTRUCTOR REACHED"),"Valid hit host failed constructor sentinel.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(HitRegionAllFeaturesProbe),HitRegionHost())!=0,"Hit feature implicitly granted intermediate services.");NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(HitRegionAllFeaturesProbe),HitRegionHost(all:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var declaration=doc.RootElement.GetProperty("requirements");
            Check(declaration.GetProperty("features").GetArrayLength()==9 && declaration.GetProperty("services_bytes").GetUInt32()==272,"All-feature requirement lost ninth name.");
            var type=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!;
            var parser=type.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;
            Check(Convert.ToUInt32(parser.Invoke(null,[declaration.GetRawText()]))==255,"Generated requirement parser rejected all nine names.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(HitRegionProbeGame),HitRegionHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var requirements=doc.RootElement.GetProperty("requirements");
            Check(requirements.GetProperty("features").EnumerateArray().Select(v=>v.GetString()).SequenceEqual(new[]{"baseline_v7","hit_regions_v1"}),"Hit-only requirements granted other features.");
        }
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        RegionServices services=new(){Actions=new(){Preferences=new(){Instances=new(){Navigation=new(){Character=new(){Animation=new(){Animation=new(){Baseline=good}}}}}}},HitTest=&ReadHitRegion};
        services.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline.Bytes=272;
        new Span<byte>((byte*)&services+176,88).Fill(0xa5);hitRegionCalls=hitRegionReplyMode=0;call.Operation=3;
        foreach(int bytes in new[]{176,192,208,216,224,232,256,264,271})
        {
            using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==1200 && hitRegionCalls==0,"Short protected hit prefix touched the tail/game.");
        }
        var missing=services;missing.HitTest=null;call.Services=&missing.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && hitRegionCalls==0,"Absent hit callback entered game.");
        call.Services=&services.Actions.Preferences.Instances.Navigation.Character.Animation.Animation.Baseline;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==1201 && hitRegionCalls==4 && !badPayload,Output(output));
        for(int mode=1;mode<=26;++mode)
        {
            hitRegionReplyMode=mode;int before=hitRegionCalls;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==1201 && hitRegionCalls==before+1,"Malformed/error hit result entered successful state: "+mode);
            if(mode==1)Check(Output(output).Contains("lifecycle rejected"),"Native hit error message lost.");
        }
        hitRegionReplyMode=0;call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(HitRegionUnmarkedProbe),null)==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(176))
        {
            *(Services*)guard.Header=good;call.Services=(Services*)guard.Header;call.Operation=3;int before=hitRegionCalls;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare IHitRegionsGame") && ((ProbeState*)state)->Count==1400 && hitRegionCalls==before,"Unmarked hit test touched protected tail.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("hit regions independent272/owned192 POD: negotiation precedes construction, nine-feature parser, protected short/undeclared tail guards, poisoned intermediate callbacks, optional geometry/region/triangle/miss, owned ASCII names and malformed/error transfer; Tick-only API");
    }
}
