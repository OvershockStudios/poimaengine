// SPDX-License-Identifier: Apache-2.0
// Independent wire PODs: deliberately do not reuse SDK internal declarations.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;
[StructLayout(LayoutKind.Sequential)] unsafe struct CapsuleWireRequest
{
    public uint Version,Bytes;public double X,Y,Z,Qx,Qy,Qz,Qw,Radius,HalfHeight,Dx,Dy,Dz,Distance;
    public Id* Ignore;public uint IgnoreCount,Reserved;
}
[StructLayout(LayoutKind.Sequential)] struct CapsuleWireResult
{
    public uint Version,Bytes,Present,Flags;public Id Entity;public double Fraction,Distance;
    public double Qx,Qy,Qz,Ox,Oy,Oz,Nx,Ny,Nz,Depth;public uint Triangle,Reserved;public ulong ReservedTail;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct CapsuleWireServices
{
    public PoseWireServices Pose;
    public delegate* unmanaged[Cdecl]<void*,CapsuleWireRequest*,CapsuleWireResult*,void*,int> Cast;
}
[GameModule("poima-test-capsule-cast")]
public sealed class CapsuleProbe:Game<ProbeState>,ICapsuleCastGame
{
    public override void Initialize(ref ProbeState state){state.Count=3400;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        EntityId[] ignore=[new(2,3),new(4,5)];
        var hit=context.CapsuleCast(new(1,2,3),Quaterniond.Identity,.25,.5,new(0,0,-2),8,ignore)
            ?? throw new Exception("Capsule hit absent.");
        if(hit.Entity!=new EntityId(7,8) || hit.Fraction!=.25 || hit.Distance!=2 || hit.PointOnQuery!=new Vector3d(1,2,1) ||
            hit.PointOnObstacle!=new Vector3d(1,2,.9) || hit.Normal!=new Vector3d(0,0,1) || hit.PenetrationDepth!=0 || hit.Triangle!=0 || hit.InitialOverlap)
            throw new Exception("Capsule contact transfer differs.");
        var overlap=context.CapsuleCast(new(1,2,3),Quaterniond.Identity,.25,0,new(1,0,0),0)
            ?? throw new Exception("Overlap hit absent.");
        if(!overlap.InitialOverlap || overlap.Fraction!=0 || overlap.Distance!=0 || overlap.PenetrationDepth!=.125 || overlap.Normal!=null || overlap.Triangle!=null)
            throw new Exception("Optional capsule overlap transfer differs.");
        var edge=context.CapsuleCast(new(1e6,2,3),Quaterniond.Identity,.25,.5,new(0,0,-1),8)
            ?? throw new Exception("Boundary-center capsule contact absent.");
        if(edge.PointOnQuery.X!=1e6+.25 || edge.PointOnObstacle.X!=1e6+.125)
            throw new Exception("Valid contact beyond the query-center bounds was lost.");
        if(context.CapsuleCast(new(-1,2,3),Quaterniond.Identity,.25,.5,new(0,0,-1),8)!=null)
            throw new Exception("Capsule miss not absent.");
        foreach(var bad in new (Vector3d Position,Quaterniond Rotation,double Radius,double Height,Vector3d Direction,double Distance)[]{
            (new(double.NaN,0,0),Quaterniond.Identity,.25,0,new(1,0,0),1),
            (new(1000001,0,0),Quaterniond.Identity,.25,0,new(1,0,0),1),
            (default,new(0,0,0,0),.25,0,new(1,0,0),1),
            (default,new(double.PositiveInfinity,0,0,1),.25,0,new(1,0,0),1),
            (default,Quaterniond.Identity,.0009,0,new(1,0,0),1),
            (default,Quaterniond.Identity,10.1,0,new(1,0,0),1),
            (default,Quaterniond.Identity,.25,-1,new(1,0,0),1),
            (default,Quaterniond.Identity,.25,10.1,new(1,0,0),1),
            (default,Quaterniond.Identity,.25,0,default,0),
            (default,Quaterniond.Identity,.25,0,new(1e-13,0,0),0),
            (default,Quaterniond.Identity,.25,0,new(1000001,0,0),1),
            (new(1000000,0,0),Quaterniond.Identity,.25,0,new(1,0,0),1),
            (default,Quaterniond.Identity,.25,0,new(double.NaN,0,0),1),
            (default,Quaterniond.Identity,.25,0,new(1,0,0),10001),
            (default,Quaterniond.Identity,.25,0,new(1,0,0),double.NaN)})
        {
            bool rejected=false;try{_=context.CapsuleCast(bad.Position,bad.Rotation,bad.Radius,bad.Height,bad.Direction,bad.Distance);}catch(ArgumentException){rejected=true;}
            if(!rejected)throw new Exception("Invalid SDK capsule geometry entered native code.");
        }
        foreach(var bad in new EntityId[][]{[default],[new(1,1),new(1,1)],new EntityId[129]})
        {
            bool rejected=false;try{_=context.CapsuleCast(default,Quaterniond.Identity,.25,0,new(1,0,0),1,bad);}catch(ArgumentException){rejected=true;}
            if(!rejected)throw new Exception("Invalid SDK capsule ignore set entered native code.");
        }
        bool ungranted=false;try{_=context.GetCameraPose(new(1,1));}catch(ArgumentException error){ungranted=error.Message.Contains("Declare ICameraPoseGame");}
        if(!ungranted)throw new Exception("Capsule permission granted camera controls.");++state.Count;
    }
}
[GameModule("poima-test-capsule-constructor")]
public sealed class CapsuleConstructorProbe:Game<ProbeState>,ICapsuleCastGame
{
    public CapsuleConstructorProbe(){throw new Exception("CAPSULE CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){}
    public override void Tick(ref ProbeState state,GameContext context){}
}
[GameModule("poima-test-capsule-unmarked")]
public sealed class CapsuleUnmarkedProbe:Game<ProbeState>
{
    public override void Initialize(ref ProbeState state){state.Count=3500;}
    public override void Tick(ref ProbeState state,GameContext context){_=context.CapsuleCast(default,Quaterniond.Identity,.25,0,new(1,0,0),1);++state.Count;}
}
[GameModule("poima-test-capsule-all")]
public sealed class CapsuleAllProbe:Game<ProbeState>,ICapsuleCastGame,ICameraPoseGame,IAnimationRateGame,IGroundingControlGame,
    ICharacterStanceGame,ICameraLensGame,IRootMotionGame,IHitRegionsGame,IInputActionsGame,IPlayerPreferencesGame,IHierarchicalInstancesGame,INavigationGame,ICharacterInputGame,IMaskedAnimationGame
{
    public override void Initialize(ref ProbeState state){}
    public override void Tick(ref ProbeState state,GameContext context){}
}
static unsafe partial class Program
{
    static int capsuleReads,capsuleMode;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int CapsuleCast(void* context,CapsuleWireRequest* q,CapsuleWireResult* r,void* error)
    {
        ++capsuleReads;
        if((nint)context!=0x1234 || q->Version!=1 || q->Bytes!=128 || q->Reserved!=0 ||
            q->Qx!=0 || q->Qy!=0 || q->Qz!=0 || q->Qw!=1 || q->Radius!=.25 ||
            r->Version!=1 || r->Bytes!=144 || r->Flags!=0 || r->Reserved!=0 || r->ReservedTail!=0)badPayload=true;
        if(q->IgnoreCount==2)
        {
            if(q->X!=1 || q->Y!=2 || q->Z!=3 || q->HalfHeight!=.5 || q->Dx!=0 || q->Dy!=0 || q->Dz!=-2 || q->Distance!=8 ||
                q->Ignore==null || q->Ignore[0].High!=2 || q->Ignore[0].Low!=3 || q->Ignore[1].High!=4 || q->Ignore[1].Low!=5)badPayload=true;
        }
        else if(q->IgnoreCount!=0)badPayload=true;
        if(capsuleMode==1)return LifecycleError(error);
        *r=new(){Version=1,Bytes=144};
        if(q->X>=0)
        {
            r->Present=1;r->Entity=new(){High=7,Low=8};r->Qx=r->Ox=1;r->Qy=r->Oy=2;
            if(q->Distance==0){r->Flags=1;r->Depth=.125;}
            else{r->Flags=6;r->Fraction=.25;r->Distance=2;r->Qz=1;r->Oz=.9;r->Nz=1;}
            if(q->X==1e6){r->Qx=1e6+.25;r->Ox=1e6+.125;}
        }
        switch(capsuleMode){
            case 2:r->Version=2;break;case 3:r->Bytes=143;break;case 4:r->Bytes=152;break;
            case 5:r->Present=2;break;case 6:r->Flags=8;break;case 7:r->Entity=default;break;
            case 8:r->Fraction=double.NaN;break;case 9:r->Fraction=1.01;break;case 10:r->Distance=9;break;
            case 11:r->Qx=double.PositiveInfinity;break;case 12:r->Ox=-1000000000001;break;
            case 13:r->Depth=-1;break;case 14:r->Depth=double.NaN;break;case 15:r->Nz=2;break;
            case 16:r->Flags&=~2u;break;case 17:r->Flags&=~4u;r->Triangle=1;break;
            case 18:r->Reserved=1;break;case 19:r->ReservedTail=1;break;
            case 20:r->Flags|=1;break;case 21:r->Depth=.1;break;
            case 22:if(q->X<0)r->Qx=BitConverter.Int64BitsToDouble(long.MinValue);break;
            case 23:if(q->Distance==0)r->Flags=0;break;
        }
        return 0;
    }
    static JsonElement CapsuleHost(uint bytes=400,bool capsule=true,uint epoch=7,bool all=false)
    {
        var names=new List<string>{"baseline_v7"};if(capsule)names.Add("capsule_cast_v1");
        if(all)foreach(var item in PoseHost(all:true).GetProperty("features").EnumerateArray()){string name=item.GetString()!;if(!names.Contains(name))names.Add(name);}
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void CapsuleCastContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(CapsuleWireRequest)==128 && sizeof(CapsuleWireResult)==144 && sizeof(CapsuleWireServices)==400 &&
            Marshal.OffsetOf<CapsuleWireRequest>(nameof(CapsuleWireRequest.X)).ToInt64()==8 && Marshal.OffsetOf<CapsuleWireRequest>(nameof(CapsuleWireRequest.Qx)).ToInt64()==32 &&
            Marshal.OffsetOf<CapsuleWireRequest>(nameof(CapsuleWireRequest.Radius)).ToInt64()==64 && Marshal.OffsetOf<CapsuleWireRequest>(nameof(CapsuleWireRequest.HalfHeight)).ToInt64()==72 &&
            Marshal.OffsetOf<CapsuleWireRequest>(nameof(CapsuleWireRequest.Dx)).ToInt64()==80 && Marshal.OffsetOf<CapsuleWireRequest>(nameof(CapsuleWireRequest.Distance)).ToInt64()==104 &&
            Marshal.OffsetOf<CapsuleWireRequest>(nameof(CapsuleWireRequest.Ignore)).ToInt64()==112 && Marshal.OffsetOf<CapsuleWireRequest>(nameof(CapsuleWireRequest.IgnoreCount)).ToInt64()==120 &&
            Marshal.OffsetOf<CapsuleWireRequest>(nameof(CapsuleWireRequest.Reserved)).ToInt64()==124 &&
            Marshal.OffsetOf<CapsuleWireResult>(nameof(CapsuleWireResult.Entity)).ToInt64()==16 && Marshal.OffsetOf<CapsuleWireResult>(nameof(CapsuleWireResult.Fraction)).ToInt64()==32 &&
            Marshal.OffsetOf<CapsuleWireResult>(nameof(CapsuleWireResult.Distance)).ToInt64()==40 && Marshal.OffsetOf<CapsuleWireResult>(nameof(CapsuleWireResult.Qx)).ToInt64()==48 &&
            Marshal.OffsetOf<CapsuleWireResult>(nameof(CapsuleWireResult.Ox)).ToInt64()==72 && Marshal.OffsetOf<CapsuleWireResult>(nameof(CapsuleWireResult.Nx)).ToInt64()==96 &&
            Marshal.OffsetOf<CapsuleWireResult>(nameof(CapsuleWireResult.Depth)).ToInt64()==120 && Marshal.OffsetOf<CapsuleWireResult>(nameof(CapsuleWireResult.Triangle)).ToInt64()==128 &&
            Marshal.OffsetOf<CapsuleWireResult>(nameof(CapsuleWireResult.Reserved)).ToInt64()==132 && Marshal.OffsetOf<CapsuleWireResult>(nameof(CapsuleWireResult.ReservedTail)).ToInt64()==136 &&
            Marshal.OffsetOf<CapsuleWireServices>(nameof(CapsuleWireServices.Cast)).ToInt64()==392,"Independent capsule wire layout differs.");
        Check(typeof(ControlContext).GetMethod("CapsuleCast")==null,"Capsule query entered Control.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,CapsuleHost(176),CapsuleHost(392),CapsuleHost(399),CapsuleHost(capsule:false),CapsuleHost(epoch:8)})
        {Check(LoadExtension(&call,typeof(CapsuleConstructorProbe),host)!=0 && !Output(output).Contains("CAPSULE CONSTRUCTOR REACHED"),"Invalid capsule host reached constructor.");NoExtensionModules(&call);}
        foreach(uint bytes in new[]{400u,408u}){Check(LoadExtension(&call,typeof(CapsuleConstructorProbe),CapsuleHost(bytes))!=0 && Output(output).Contains("CAPSULE CONSTRUCTOR REACHED"),"Valid capsule host denied constructor.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(CapsuleAllProbe),CapsuleHost(all:true))==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var declaration=doc.RootElement.GetProperty("requirements");
            Check(declaration.GetProperty("features").GetArrayLength()==18 && declaration.GetProperty("services_bytes").GetUInt32()==400,"All18 feature union differs.");
            var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;
            Check(Convert.ToUInt32(parser.Invoke(null,[declaration.GetRawText()]))==131071,"All18 declared flags differ.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(CapsuleProbe),CapsuleHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var req=doc.RootElement.GetProperty("requirements");Check(req.GetProperty("services_bytes").GetUInt32()==400 && req.GetProperty("features").EnumerateArray().Select(x=>x.GetString()).SequenceEqual(new[]{"baseline_v7","capsule_cast_v1"}),"Capsule declaration grants unrelated features.");}
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        CapsuleWireServices services=default;var baseline=(Services*)(&services);*baseline=good;baseline->Bytes=400;services.Cast=&CapsuleCast;
        new Span<byte>((byte*)&services+176,216).Fill(0xa5);capsuleReads=capsuleMode=0;call.Operation=3;
        foreach(int bytes in new[]{176,192,376,392,399})
        {using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && capsuleReads==0 && ((ProbeState*)state)->Count==3400,"Protected short capsule tail accessed.");}
        var absent=services;absent.Cast=null;call.Services=(Services*)(&absent);Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && capsuleReads==0,"Missing capsule callback entered game.");
        call.Services=baseline;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && capsuleReads==4 && ((ProbeState*)state)->Count==3401 && !badPayload,Output(output));
        for(int mode=1;mode<=23;++mode)
        {capsuleMode=mode;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==3401,"Malformed/error capsule accepted: "+mode);if(mode==1)Check(Output(output).Contains("lifecycle rejected"),"Capsule native error lost.");}
        capsuleMode=0;call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(CapsuleUnmarkedProbe),null)==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(176))
        {*(Services*)guard.Header=good;call.Services=(Services*)guard.Header;call.Operation=3;int before=capsuleReads;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare ICapsuleCastGame") && capsuleReads==before && ((ProbeState*)state)->Count==3500,"Unmarked game accessed capsule tail.");}
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("capsule_cast400/REQ128/RESULT144 independent layout,18feature union, pre-construction/protected-tail/independent marker permissions");
        checks.Add("capsule contact/miss/overlap/optional fields, strict geometry+ignore validation and23 malformed/error results preserve compiled state");
    }
}
