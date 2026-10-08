// SPDX-License-Identifier: Apache-2.0
// Independent wire declarations and adversarial callbacks exercise the actual
// SDK/CoreCLR bridge. These are not Runtime routing or NativeAOT execution tests.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)] struct NavPoint { public float X,Y,Z; }
[StructLayout(LayoutKind.Sequential)] struct NavRequest
{
    public uint Version,Bytes;public Id Agent;public NavPoint Goal,Extents;
    public uint Polygons,Corners,Nodes,Reserved;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct NavReply
{
    public uint Version,Bytes;public fixed ulong Asset[4];public uint Status,Flags,Count,Polygons;
    public NavPoint Requested,Start,End,Reachable;public double StartDistance,EndDistance;public uint Reserved0,Reserved1;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct NavServices
{
    public CharacterServices Character;
    public delegate* unmanaged[Cdecl]<void*,NavRequest*,NavPoint*,uint,NavReply*,void*,int> Path;
}
[GameModule("poima-test-navigation")]
public class NavigationProbeGame : Game<ProbeState>,INavigationGame
{
    public override void Initialize(ref ProbeState state){state.Count=100;}
    internal static void One(GameContext context)
    {
        Span<NavigationPoint> points=stackalloc NavigationPoint[4];points.Fill(new(81,82,83));
        var r=context.FindNavigationPath(new(11,40),new(7.25,1,-8.5),points);
        if(r.Status!=NavigationPathStatus.Complete || r.CornerCount!=2 || !r.Complete || r.Polygons!=2 ||
            r.Asset.ToString()!="0123456789abcdeffedcba987654321000000000000000000000000000000042" ||
            r.RequestedStart!=new NavigationPoint(1,2,3) || r.ProjectedStart!=new NavigationPoint(1,1.75f,3) ||
            r.ProjectedEnd!=new NavigationPoint(7.25f,1,-8.5f) || r.ReachableEnd!=r.ProjectedEnd ||
            r.StartProjectionDistance!=.25 || r.EndProjectionDistance!=0 || points[0]!=r.ProjectedStart || points[1]!=r.ProjectedEnd ||
            points[2]!=new NavigationPoint(81,82,83) || points[3]!=new NavigationPoint(81,82,83))
            throw new Exception("Navigation transfer or untouched corner tail differs.");
    }
    public override void Tick(ref ProbeState state,GameContext context)
    {
        Span<NavigationPoint> points=stackalloc NavigationPoint[4];points.Fill(new(81,82,83));
        // Invalid arguments never reach a callback and never change the route.
        Span<NavigationPoint> invalid=stackalloc NavigationPoint[257];
        for(int test=0;test<17;++test){bool rejected=false;
            try{
                var goal=test switch{0=>new Vector3d(double.NaN,1,2),1=>new(1,double.PositiveInfinity,2),2=>new(1,2,-1000000.01),_=>new Vector3d(7.25,1,-8.5)};
                Vector3d? extents=test switch{3=>new(double.NaN,1,1),4=>new(1,.009,1),5=>new(1,1,100.01),_=>null};
                uint polys=test==6?0u:test==7?257u:256u,nodes=test==8?31u:test==9?4097u:4096u;
                int capacity=test==10?0:test==11?1:test==12?257:4;
                if(test>=13){double value=test==13?double.NegativeInfinity:test==14?-1000000.01:test==15?1000000.01:double.NaN;
                    goal=test==16?new(1,value,1):new(value,1,1);}
                invalid.Fill(new(81,82,83));
                _=context.FindNavigationPath(new(11,40),goal,invalid[..capacity],extents,polys,nodes);
            }catch(ArgumentOutOfRangeException){rejected=true;}
            if(!rejected)throw new Exception("Invalid navigation argument accepted.");
        }
        bool denied=false;try{_=context.GetAnimationExtended(new(11,33));}catch(ArgumentException e){denied=e.Message.Contains("Declare IInertialAnimationGame");}
        if(!denied)throw new Exception("Navigation granted inertial getter.");
        denied=false;try{context.SetAnimation(new(11,22),3,AnimationTransitionMode.Inertial);}catch(ArgumentException e){denied=e.Message.Contains("Declare IInertialAnimationGame");}
        if(!denied)throw new Exception("Navigation granted inertial setter.");
        denied=false;try{_=context.GetAnimationLayer(new(11,31),1);}catch(ArgumentException e){denied=e.Message.Contains("Declare IMaskedAnimationGame");}
        if(!denied)throw new Exception("Navigation granted layer getter.");
        denied=false;try{context.SetAnimationLayer(new(11,31),1,2,.5);}catch(ArgumentException e){denied=e.Message.Contains("Declare IMaskedAnimationGame");}
        if(!denied)throw new Exception("Navigation granted layer setter.");
        denied=false;try{context.SetCharacterInput(new(11,40),0,0);}catch(ArgumentException e){denied=e.Message.Contains("Declare ICharacterInputGame");}
        if(!denied)throw new Exception("Navigation granted character setter.");
        One(context);
        for(ulong kind=41;kind<=45;++kind){points.Fill(new(81,82,83));
            var r=context.FindNavigationPath(new(11,kind),new(7.25,1,-8.5),points,new(.25,.5,.75),9,64);
            if(kind==41){if(!r.Complete || r.CornerCount!=1 || points[0]!=r.ProjectedStart || points[1]!=new NavigationPoint(81,82,83))throw new Exception("Single-corner complete route rejected.");}
            else if(kind==42){if(r.Status!=NavigationPathStatus.Unreachable || r.CornerCount!=0 || r.ProjectedStart!=null || r.StartProjectionDistance!=null || r.ProjectedEnd==null || r.ReachableEnd!=null || points[0]!=new NavigationPoint(81,82,83))throw new Exception("Absent projection canonical form differs.");}
            else if((uint)r.Status!=(kind==43?1u:kind==44?3u:4u) || r.Complete || r.CornerCount!=2 || r.ReachableEnd!=new NavigationPoint(6,1,-7) || points[1]!=new NavigationPoint(4,1,-5))throw new Exception("Partial/budget-limited route falsely complete or lost corridor endpoint.");
        }
        ++state.Count;
    }
    public override void Control(ref ProbeState state,ControlContext context){++state.Count;}
}
[GameModule("poima-test-navigation-invalid-reply")]
public sealed class NavigationInvalidReplyProbe : Game<ProbeState>,INavigationGame
{
    public override void Initialize(ref ProbeState state){state.Count=200;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        Span<NavigationPoint> points=stackalloc NavigationPoint[4];
        for(ulong mutation=0;mutation<26;++mutation){points.Fill(new(81,82,83));bool rejected=false;
            try{_=context.FindNavigationPath(new(11,100+mutation),new(7.25,1,-8.5),points);}catch(InvalidOperationException){rejected=true;}
            if(!rejected || points.ToArray().Any(p=>p!=new NavigationPoint(81,82,83)))throw new Exception("Malformed native result published partial corners.");
        }
        ++state.Count;
    }
}
[GameModule("poima-test-navigation-constructor")]
public sealed class NavigationConstructorProbe : Game<ProbeState>,INavigationGame
{
    public NavigationConstructorProbe(){throw new Exception("NAVIGATION CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){throw new Exception("NAVIGATION INITIALIZE REACHED");}
    public override void Tick(ref ProbeState state,GameContext context){throw new Exception("NAVIGATION TICK REACHED");}
}
[GameModule("poima-test-navigation-unmarked")]
public sealed class UnmarkedNavigationProbe : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state){state.Count=300;}
    public override void Tick(ref ProbeState state,GameContext context){NavigationProbeGame.One(context);++state.Count;}
}
[GameModule("poima-test-navigation-inertial")]
public sealed class NavigationInertialProbe : Game<ProbeState>,INavigationGame,IInertialAnimationGame
{
    public override void Initialize(ref ProbeState state){state.Count=400;}
    public override void Tick(ref ProbeState state,GameContext context){NavigationProbeGame.One(context);_=context.GetAnimationExtended(new(11,33));++state.Count;}
}
[GameModule("poima-test-navigation-masked")]
public sealed class NavigationMaskedProbe : Game<ProbeState>,INavigationGame,IMaskedAnimationGame
{
    public override void Initialize(ref ProbeState state){state.Count=400;}
    public override void Tick(ref ProbeState state,GameContext context){NavigationProbeGame.One(context);_=context.GetAnimationLayer(new(11,31),1);++state.Count;}
}
[GameModule("poima-test-navigation-character")]
public class NavigationCharacterProbe : Game<ProbeState>,INavigationGame,ICharacterInputGame
{
    public override void Initialize(ref ProbeState state){state.Count=400;}
    public override void Tick(ref ProbeState state,GameContext context){NavigationProbeGame.One(context);CharacterProbeGame.Intent(context);++state.Count;}
}
[GameModule("poima-test-navigation-character-inertial")]
public sealed class NavigationCharacterInertialProbe : NavigationCharacterProbe,IInertialAnimationGame
{
    public override void Tick(ref ProbeState state,GameContext context){base.Tick(ref state,context);_=context.GetAnimationExtended(new(11,33));}
}
[GameModule("poima-test-navigation-character-masked")]
public sealed class NavigationCharacterMaskedProbe : NavigationCharacterProbe,IMaskedAnimationGame
{
    public override void Tick(ref ProbeState state,GameContext context){base.Tick(ref state,context);_=context.GetAnimationLayer(new(11,31),1);}
}
static unsafe partial class Program
{
    static int navigationCalls;static bool navigationError;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int NavigationPath(void* context,NavRequest* request,NavPoint* points,uint capacity,NavReply* reply,void* error)
    {
        ++navigationCalls;
        if((nint)context!=0x1234 || request->Version!=1 || request->Bytes!=64 || request->Reserved!=0 || request->Agent.High!=11 ||
            request->Goal.X!=7.25f || request->Goal.Y!=1 || request->Goal.Z!=-8.5f || request->Corners!=4 || capacity!=4 ||
            reply->Version!=1 || reply->Bytes!=128 || reply->Reserved0!=0 || reply->Reserved1!=0)badPayload=true;
        bool custom=request->Agent.Low is >=41 and <=45;
        if(custom ? request->Polygons!=9 || request->Nodes!=64 || request->Extents.X!=.25f || request->Extents.Y!=.5f || request->Extents.Z!=.75f :
            request->Polygons!=256 || request->Nodes!=4096 || request->Extents.X!=2 || request->Extents.Y!=4 || request->Extents.Z!=2)badPayload=true;
        *reply=new(){Version=1,Bytes=128,Status=0,Flags=7,Count=2,Polygons=2,Requested=new(){X=1,Y=2,Z=3},
            Start=new(){X=1,Y=1.75f,Z=3},End=new(){X=7.25f,Y=1,Z=-8.5f},Reachable=new(){X=7.25f,Y=1,Z=-8.5f},StartDistance=.25};
        reply->Asset[0]=0x0123456789abcdef;reply->Asset[1]=0xfedcba9876543210;reply->Asset[2]=0;reply->Asset[3]=0x42;
        points[0]=reply->Start;points[1]=reply->End;
        if(request->Agent.Low==41){reply->Count=1;reply->End=reply->Reachable=reply->Start;points[0]=reply->Start;}
        if(request->Agent.Low==42){reply->Status=2;reply->Flags=2;reply->Count=reply->Polygons=0;reply->Start=reply->Reachable=default;reply->StartDistance=0;}
        if(request->Agent.Low is >=43 and <=45){reply->Status=request->Agent.Low==43?1u:request->Agent.Low==44?3u:4u;reply->Reachable=new(){X=6,Y=1,Z=-7};points[1]=new(){X=4,Y=1,Z=-5};}
        if(request->Agent.Low>=100){switch(request->Agent.Low-100){
            case 0:reply->Version=2;break;case 1:reply->Bytes=127;break;case 2:reply->Bytes=129;break;
            case 3:reply->Reserved0=1;break;case 4:reply->Reserved1=1;break;case 5:reply->Status=5;break;case 6:reply->Flags=8;break;
            case 7:reply->Count=5;break;case 8:reply->Polygons=257;break;case 9:reply->Requested.X=float.NaN;break;
            case 10:reply->Start.Z=float.PositiveInfinity;break;case 11:reply->End.Y=1000001;break;case 12:reply->Reachable.X=float.NaN;break;
            case 13:reply->StartDistance=-1;break;case 14:reply->EndDistance=double.NaN;break;
            case 15:reply->Flags=6;break;case 16:reply->Flags=5;break;case 17:reply->Flags=3;break;
            case 18:reply->Polygons=0;break;case 19:reply->Count=0;break;case 20:reply->Status=2;break;
            case 21:points[0].X=float.NaN;break;case 22:points[1].Y=float.PositiveInfinity;break;
            case 23:points[1].Z=1000001;break;case 24:reply->StartDistance=double.PositiveInfinity;break;
        }}
        if(navigationError || request->Agent.Low==125){Encoding.UTF8.GetBytes("navigation rejected\0",new Span<byte>(error,2048));return -1;}
        return 0;
    }
    static JsonElement NavigationHost(uint bytes=224,bool nav=true,bool character=false,bool inertial=false,bool layers=false)
        =>JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=7,services_bytes=bytes,
            features=new[]{"baseline_v7",nav?"navigation_query_v1":null,character?"character_input_v1":null,inertial?"animation_inertial_v1":null,layers?"animation_layers_v1":null}.Where(v=>v!=null).ToArray()});
    static void NavigationContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(NavServices)==224 && sizeof(NavRequest)==64 && sizeof(NavReply)==128 && sizeof(NavPoint)==12 && sizeof(NavigationPoint)==12 && sizeof(NavigationAssetId)==32 &&
            Marshal.OffsetOf<NavServices>(nameof(NavServices.Path)).ToInt64()==216 && Marshal.OffsetOf<NavRequest>(nameof(NavRequest.Agent)).ToInt64()==8 &&
            Marshal.OffsetOf<NavRequest>(nameof(NavRequest.Goal)).ToInt64()==24 && Marshal.OffsetOf<NavRequest>(nameof(NavRequest.Extents)).ToInt64()==36 &&
            Marshal.OffsetOf<NavRequest>(nameof(NavRequest.Polygons)).ToInt64()==48 && Marshal.OffsetOf<NavRequest>(nameof(NavRequest.Corners)).ToInt64()==52 &&
            Marshal.OffsetOf<NavRequest>(nameof(NavRequest.Nodes)).ToInt64()==56 && Marshal.OffsetOf<NavRequest>(nameof(NavRequest.Reserved)).ToInt64()==60 &&
            Marshal.OffsetOf<NavReply>(nameof(NavReply.Asset)).ToInt64()==8 && Marshal.OffsetOf<NavReply>(nameof(NavReply.Status)).ToInt64()==40 &&
            Marshal.OffsetOf<NavReply>(nameof(NavReply.Flags)).ToInt64()==44 && Marshal.OffsetOf<NavReply>(nameof(NavReply.Count)).ToInt64()==48 &&
            Marshal.OffsetOf<NavReply>(nameof(NavReply.Polygons)).ToInt64()==52 && Marshal.OffsetOf<NavReply>(nameof(NavReply.Requested)).ToInt64()==56 &&
            Marshal.OffsetOf<NavReply>(nameof(NavReply.Start)).ToInt64()==68 && Marshal.OffsetOf<NavReply>(nameof(NavReply.End)).ToInt64()==80 &&
            Marshal.OffsetOf<NavReply>(nameof(NavReply.Reachable)).ToInt64()==92 && Marshal.OffsetOf<NavReply>(nameof(NavReply.StartDistance)).ToInt64()==104 &&
            Marshal.OffsetOf<NavReply>(nameof(NavReply.EndDistance)).ToInt64()==112 && Marshal.OffsetOf<NavReply>(nameof(NavReply.Reserved0)).ToInt64()==120 &&
            Marshal.OffsetOf<NavReply>(nameof(NavReply.Reserved1)).ToInt64()==124,"Independent navigation wire layout mismatch.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,NavigationHost(176),NavigationHost(192),NavigationHost(208),NavigationHost(216),NavigationHost(223),NavigationHost(nav:false),
            ExtensionJson("{\"call_version\":1,\"call_bytes\":80,\"services_version\":8,\"services_bytes\":224,\"features\":[\"baseline_v7\",\"navigation_query_v1\"]}"),
            ExtensionJson("{\"call_version\":1,\"call_bytes\":80,\"services_version\":7,\"services_bytes\":224,\"features\":[\"baseline_v7\",\"navigation_query_v1\",\"navigation_query_v1\"]}")}){
            Check(LoadExtension(&call,typeof(NavigationConstructorProbe),host)!=0 && !Output(output).Contains("NAVIGATION CONSTRUCTOR REACHED"),"Invalid navigation host entered constructor.");NoExtensionModules(&call);
        }
        foreach(uint bytes in new uint[]{224,240}){Check(LoadExtension(&call,typeof(NavigationConstructorProbe),NavigationHost(bytes))!=0 && Output(output).Contains("NAVIGATION CONSTRUCTOR REACHED"),"Valid named navigation host failed positive sentinel.");NoExtensionModules(&call);}
        checks.Add("navigation marker rejects missing, unnamed, short, wrong-epoch and duplicate host features before constructor;224/larger valid hosts reach sentinel");
        Check(LoadExtension(&call,typeof(NavigationProbeGame),NavigationHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output))){call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var r=doc.RootElement.GetProperty("requirements");
            Check(r.GetProperty("services_bytes").GetUInt32()==224 && r.GetProperty("features").EnumerateArray().Select(v=>v.GetString()).SequenceEqual(new[]{"baseline_v7","navigation_query_v1"}),"Navigation-only required profile differs.");}
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        NavServices services=new(){Character=new(){Animation=new(){Animation=new(){Baseline=good}}},Path=&NavigationPath};services.Character.Animation.Animation.Baseline.Bytes=224;
        new Span<byte>((byte*)&services+176,40).Fill(0xa5);
        foreach(uint operation in new uint[]{3,6}){call.Operation=operation;
            foreach(int size in new[]{176,192,208,216}){using var guard=new GuardedHeader(size);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)size;call.Services=(Services*)guard.Header;
                int before=((ProbeState*)state)->Count,calls=navigationCalls;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==before && navigationCalls==calls,"Short table read protected navigation tail or executed game.");}
            var missing=services;missing.Path=null;call.Services=&missing.Character.Animation.Animation.Baseline;int prior=((ProbeState*)state)->Count;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==prior,"Missing navigation callback executed game.");
        }
        checks.Add("actual176/192/208/216 guarded tables and absent own callback reject Tick/Control without reading ungranted tail");
        call.Operation=3;call.Services=&services.Character.Animation.Animation.Baseline;navigationCalls=0;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==101 && navigationCalls==6 && !badPayload,Output(output));
        call.Operation=6;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==102 && navigationCalls==6,Output(output));
        checks.Add("six typed routes include single-corner complete, canonical missing projection, partial and exhausted budgets;17 invalid arguments avoid callback; unrequested40-byte tails poisoned and five helpers deny access");
        navigationError=true;call.Operation=3;int count=((ProbeState*)state)->Count;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output)=="navigation rejected" && ((ProbeState*)state)->Count==count,"Native navigation error changed gameplay state.");
        navigationError=false;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==count+1,Output(output));
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(NavigationInvalidReplyProbe),NavigationHost())==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));call.Operation=3;navigationCalls=0;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && navigationCalls==26 && ((ProbeState*)state)->Count==201 && !badPayload,Output(output));
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("25 malformed result/header/status/flags/count/projection/corner replies plus native failure reject with destination span unchanged; native error/recovery and collectible cleanup pass");
        foreach(var combo in new[]{(typeof(NavigationInertialProbe),false,true,false),(typeof(NavigationMaskedProbe),false,true,true),
            (typeof(NavigationCharacterProbe),true,false,false),(typeof(NavigationCharacterInertialProbe),true,true,false),(typeof(NavigationCharacterMaskedProbe),true,true,true)}){
            Check(LoadExtension(&call,combo.Item1,NavigationHost(character:combo.Item2,inertial:combo.Item3,layers:combo.Item4))==0,Output(output));
            using(var doc=JsonDocument.Parse(Output(output))){call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var required=doc.RootElement.GetProperty("requirements");
                var expected=new[]{"animation_inertial_v1","animation_layers_v1","baseline_v7","character_input_v1","navigation_query_v1"}.Where(n=>n=="baseline_v7" || n=="navigation_query_v1" || n=="character_input_v1" && combo.Item2 || n=="animation_inertial_v1" && combo.Item3 || n=="animation_layers_v1" && combo.Item4).ToArray();
                Check(required.GetProperty("services_bytes").GetUInt32()==224 && required.GetProperty("features").EnumerateArray().Select(v=>v.GetString()).SequenceEqual(expected),"Combined navigation manifest lost/synthesized feature.");}
            call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            services=new(){Character=new(){Animation=new(){Animation=new(){Baseline=good,Get=combo.Item3?&ExtendedGet:null,Set=combo.Item3?&ExtendedSet:null},Get=combo.Item4?&LayerGet:null,Set=combo.Item4?&LayerSet:null},Set=combo.Item2?&CharacterSet:null},Path=&NavigationPath};
            services.Character.Animation.Animation.Baseline.Bytes=224;
            foreach(int callback in Enumerable.Range(0,6)){if(callback==0 && !combo.Item3 || callback==1 && !combo.Item3 || callback==2 && !combo.Item4 || callback==3 && !combo.Item4 || callback==4 && !combo.Item2)continue;
                var missing=services;switch(callback){case 0:missing.Character.Animation.Animation.Get=null;break;case 1:missing.Character.Animation.Animation.Set=null;break;case 2:missing.Character.Animation.Get=null;break;case 3:missing.Character.Animation.Set=null;break;case 4:missing.Character.Set=null;break;case 5:missing.Path=null;break;}
                call.Operation=3;call.Services=&missing.Character.Animation.Animation.Baseline;int before=((ProbeState*)state)->Count,calls=navigationCalls;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==before && navigationCalls==calls,"Combined profile missing requested callback executed game.");}
            call.Services=&services.Character.Animation.Animation.Baseline;call.Operation=3;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==401 && !badPayload,Output(output));
            call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        }
        checks.Add("all five navigation/animation/character combinations retain exact sorted required names; missing each requested callback rejects before game while unrequested callbacks may be null");
        Check(LoadExtension(&call,typeof(UnmarkedNavigationProbe),null)==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(176)){*(Services*)guard.Header=good;call.Services=(Services*)guard.Header;call.Operation=3;int calls=navigationCalls;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare INavigationGame") && ((ProbeState*)state)->Count==300 && navigationCalls==calls,"Unmarked navigation helper read protected tail.");}
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("unmarked navigation helper rejects actual guarded176 table before extension read");
    }
}
