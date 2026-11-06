// SPDX-License-Identifier: Apache-2.0
// Independent wire declarations and synthetic callbacks test ABI/SDK transfer.
// Actual guarded handback, geometry and rollback require native runtime evidence.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;
[StructLayout(LayoutKind.Sequential)] unsafe struct RecoveryStateWire
{
    public uint Version,Bytes,Present,Status;public Id Entity;public ulong Tick,Generation,AssessmentRevision;
    public uint Flags,Stable,Required,LastResult;public ulong LastTick,ReturnedTick;
    public double Linear,Angular,Upright,Distance,Angle;public fixed uint Supports[2];public uint SupportMask,Reserved;
}
[StructLayout(LayoutKind.Sequential)] struct RecoveryCommandWire
{public uint Version,Bytes;public Id Entity;public ulong Reserved;}
[StructLayout(LayoutKind.Sequential)] unsafe struct RecoveryServicesWire
{
    public PhysicalAnimationServicesWire Animation;
    public delegate* unmanaged[Cdecl]<void*,Id*,RecoveryStateWire*,void*,int> Get;
    public delegate* unmanaged[Cdecl]<void*,RecoveryCommandWire*,void*,int> Request;
}
[GameModule("poima-test-recovery")]
public sealed class RecoveryProbe:Game<ProbeState>,IPhysicalRecoveryGame
{
    public override void Initialize(ref ProbeState state){state.Count=6100;}
    public override void Tick(ref ProbeState state,GameContext context)
    {
        EntityId id=new(11,90);var value=context.GetPhysicalRecovery(id)??throw new Exception("Recovery absent.");
        if(value!=new PhysicalRecoveryState(id,123,2,7,PhysicalRecoveryStatus.Stabilizing,true,false,2,4,121,PhysicalRecoveryStatus.Returned,121,.2,.3,.98,.01,.02,0x01000001,0x02000002))
            throw new Exception("Recovery fields differ.");
        if(context.GetPhysicalRecovery(new(11,91))!=null)throw new Exception("Absent recovery has payload.");
        context.RequestPhysicalRecovery(id);
        if(context.GetPhysicalRecovery(id)!=value)throw new Exception("Request changed committed recovery diagnostics.");
        foreach(var (low,sequence) in new[]{(92UL,1u),(93UL,2u),(94UL,255u)})
        {
            var support=context.GetPhysicalRecovery(new(11,low))??throw new Exception("Valid sequence support absent.");
            if(support.FirstSupportBody!=(sequence<<23) || support.SecondSupportBody!=((sequence<<23)|4095u))
                throw new Exception("Legal Jolt sequence or boundary slot was rejected/changed.");
        }

        bool failed=false;try{_=context.GetPhysicalRecovery(default);}catch(ArgumentException){failed=true;}if(!failed)throw new Exception("Absent read entity reached host.");
        failed=false;try{context.RequestPhysicalRecovery(default);}catch(ArgumentException){failed=true;}if(!failed)throw new Exception("Absent request entity reached host.");
        failed=false;try{_=context.GetPhysicalRigDrive(id,"spine");}catch(ArgumentException e){failed=e.Message.Contains("Declare IPhysicalRigDriveGame");}if(!failed)throw new Exception("Recovery granted drive permission.");
        failed=false;try{_=context.GetPhysicalRigAnimation(id);}catch(ArgumentException e){failed=e.Message.Contains("Declare IPhysicalRigAnimationGame");}if(!failed)throw new Exception("Recovery granted target-player permission.");
        ++state.Count;
    }
}
[GameModule("poima-test-recovery-constructor")]
public sealed class RecoveryConstructorProbe:Game<ProbeState>,IPhysicalRecoveryGame
{public RecoveryConstructorProbe(){throw new Exception("RECOVERY CONSTRUCTOR REACHED");}public override void Initialize(ref ProbeState state){}public override void Tick(ref ProbeState state,GameContext context){}}
[GameModule("poima-test-recovery-unmarked")]
public sealed class RecoveryUnmarkedProbe:Game<ProbeState>,IPhysicalRigGame
{public override void Initialize(ref ProbeState state){state.Count=6200;}public override void Tick(ref ProbeState state,GameContext context){_=context.GetPhysicalRecovery(new(11,90));++state.Count;}}
static unsafe partial class Program
{
    static int recoveryMode,recoveryReads,recoveryRequests;static bool recoveryBadPayload;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int RecoveryGet(void* context,Id* id,RecoveryStateWire* output,void* error)
    {
        ++recoveryReads;if((nint)context!=0x1234 || output->Version!=1 || output->Bytes!=144 || output->Present!=0 || output->Flags!=0 || output->SupportMask!=0 || output->Reserved!=0)recoveryBadPayload=true;
        if(recoveryMode==90)return LifecycleError(error);
        *output=new(){Version=1,Bytes=144,Entity=*id};
        if(id->Low==90 || id->Low is >=92 and <=94){output->Present=1;output->Status=13;output->Tick=123;output->Generation=2;output->AssessmentRevision=7;output->Flags=13;
            output->Stable=2;output->Required=4;output->LastResult=15;output->LastTick=output->ReturnedTick=121;
            output->Linear=.2;output->Angular=.3;output->Upright=.98;output->Distance=.01;output->Angle=.02;output->Supports[0]=0x01000001;output->Supports[1]=0x02000002;output->SupportMask=3;}
        if(id->Low is >=92 and <=94)
        {
            uint sequence=id->Low==92?1u:id->Low==93?2u:255u;
            output->Supports[0]=sequence<<23;output->Supports[1]=(sequence<<23)|4095u;
        }

        switch(recoveryMode){
            case 1:output->Version=2;break;case 2:output->Bytes=143;break;case 3:output->Present=2;break;case 4:output->Status=17;break;
            case 5:output->Entity.Low=99;break;case 6:output->Tick=122;break;case 7:output->Generation=124;break;case 8:output->AssessmentRevision=9007199254740992;break;
            case 9:output->Flags|=16;break;case 10:output->Required=1;break;case 11:output->Required=61;break;case 12:output->Stable=61;break;
            case 13:output->LastResult=17;break;case 14:output->LastTick=124;break;case 15:output->LastTick=0;break;
            case 16:output->ReturnedTick=124;break;case 17:output->ReturnedTick=122;break;case 18:output->ReturnedTick=120;break;
            case 19:output->Generation=0;break;case 20:output->Flags&=~4u;break;case 21:output->Flags&=~8u;break;
            case 22:output->Linear=double.NaN;break;case 23:output->Angular=double.PositiveInfinity;break;case 24:output->Upright=1.1;break;
            case 25:output->Distance=-.01;break;case 26:output->Angle=3.2;break;case 27:output->SupportMask=4;break;
            case 28:output->Supports[0]=0x80000001;break;case 29:output->Supports[0]=4096;break;case 30:output->SupportMask=0;break;
            case 31:output->Reserved=1;break;case 32:if(id->Low==91)output->Linear=BitConverter.Int64BitsToDouble(long.MinValue);break;
            case 33:if(id->Low==91)output->Status=1;break;case 34:output->Bytes=145;break;
            case 35:output->Stable=5;break;case 36:output->Flags|=2;break;case 37:output->Status=0;break;
            case 38:output->Status=14;break;case 39:output->Stable=0;break;case 40:output->Stable=4;break;
            case 41:output->Flags&=~1u;break;case 42:output->SupportMask=2;output->Supports[0]=0;break;
            case 43:output->Supports[0]=(1u<<23)|4096u;break;
            case 44:output->Supports[0]=(255u<<23)|4096u;break;
            case 45:output->Supports[0]=(255u<<23)|0x80000000u;break;
            case 46:if(id->Low==91)output->Supports[0]=1u<<23;break;


        }
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int RecoveryRequest(void* context,RecoveryCommandWire* command,void* error)
    {
        ++recoveryRequests;if((nint)context!=0x1234 || command->Version!=1 || command->Bytes!=32 || command->Entity.High!=11 || command->Entity.Low!=90 || command->Reserved!=0)recoveryBadPayload=true;
        return recoveryMode==91?LifecycleError(error):0;
    }
    static JsonElement RecoveryHost(uint bytes=512,bool physical=true,bool recovery=true,uint epoch=7)
    {
        var names=new List<string>{"baseline_v7"};if(physical)names.Add("physical_rig_v1");if(recovery)names.Add("physical_recovery_v1");
        return JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=epoch,services_bytes=bytes,features=names});
    }
    static void PhysicalRecoveryContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(RecoveryStateWire)==144 && sizeof(RecoveryCommandWire)==32 && sizeof(RecoveryServicesWire)==512,"Independent recovery wire sizes differ.");
        foreach(var (name,offset) in new[]{("Entity",16),("Tick",32),("Generation",40),("AssessmentRevision",48),("Flags",56),("Stable",60),("Required",64),("LastResult",68),("LastTick",72),("ReturnedTick",80),("Linear",88),("Angular",96),("Upright",104),("Distance",112),("Angle",120),("Supports",128),("SupportMask",136),("Reserved",140)})
            Check(Marshal.OffsetOf<RecoveryStateWire>(name).ToInt64()==offset,"Recovery state offset differs: "+name);
        Check(Marshal.OffsetOf<RecoveryServicesWire>("Get").ToInt64()==496 && Marshal.OffsetOf<RecoveryServicesWire>("Request").ToInt64()==504 && Marshal.OffsetOf<RecoveryCommandWire>("Entity").ToInt64()==8 && Marshal.OffsetOf<RecoveryCommandWire>("Reserved").ToInt64()==24,"Recovery callback/command offsets differ.");
        Check(!typeof(ControlContext).GetMethods().Any(m=>m.Name.Contains("PhysicalRecovery")),"Recovery API entered Control context.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,RecoveryHost(176),RecoveryHost(496),RecoveryHost(511),RecoveryHost(physical:false),RecoveryHost(recovery:false),RecoveryHost(epoch:8)})
        {Check(LoadExtension(&call,typeof(RecoveryConstructorProbe),host)!=0&&!Output(output).Contains("RECOVERY CONSTRUCTOR REACHED"),"Invalid recovery host reached constructor.");NoExtensionModules(&call);}
        foreach(uint bytes in new[]{512u,520u})
        {Check(LoadExtension(&call,typeof(RecoveryConstructorProbe),RecoveryHost(bytes))!=0&&Output(output).Contains("RECOVERY CONSTRUCTOR REACHED"),"Valid recovery host denied constructor.");NoExtensionModules(&call);}
        Check(LoadExtension(&call,typeof(RecoveryProbe),RecoveryHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var required=doc.RootElement.GetProperty("requirements");
            Check(required.GetProperty("services_bytes").GetUInt32()==512 && required.GetProperty("features").EnumerateArray().Select(x=>x.GetString()).SequenceEqual(new[]{"baseline_v7","physical_recovery_v1","physical_rig_v1"}),"Recovery grants unrelated requirements.");
            var parser=typeof(Game<>).Assembly.GetType("Poima.GameplayRequirements",true)!.GetMethod("DeclaredFeatures",BindingFlags.Static|BindingFlags.NonPublic)!;
            Check(Convert.ToUInt32(parser.Invoke(null,[required.GetRawText()]))==1179648,"Recovery generated flags differ.");
            foreach(var bad in new[]{RecoveryHost(511),RecoveryHost(physical:false),RecoveryHost(recovery:false)})
            {bool rejected=false;try{_=parser.Invoke(null,[bad.GetRawText()]);}catch(TargetInvocationException e)when(e.InnerException is ArgumentException){rejected=true;}Check(rejected,"Malformed recovery declaration accepted.");}
        }
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        RecoveryServicesWire services=default;var baseline=(Services*)&services;*baseline=good;baseline->Bytes=512;
        // Recovery-only consumers must never need these opaque/unrequested slots.
        new Span<byte>((byte*)&services+176,224).Fill(0xa5);new Span<byte>((byte*)&services+424,72).Fill(0xa5);
        services.Animation.Drive.Physical.Summary=&PhysicalSummary;services.Animation.Drive.Physical.Part=&PhysicalPart;services.Animation.Drive.Physical.Command=&PhysicalCommand;
        services.Get=&RecoveryGet;services.Request=&RecoveryRequest;recoveryMode=recoveryReads=recoveryRequests=0;recoveryBadPayload=false;call.Operation=3;
        foreach(int bytes in new[]{176,424,448,496,503,504,511})
        {using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0&&recoveryReads==0&&((ProbeState*)state)->Count==6100,"Recovery accessed a protected short tail.");}
        call.Services=baseline;
        for(int i=0;i<2;++i){nint* slot=(nint*)((byte*)&services+496+i*8);nint saved=*slot;*slot=0;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0&&recoveryReads==0,"Absent recovery callback accepted.");*slot=saved;}
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        Check(recoveryReads==6&&recoveryRequests==1&&((ProbeState*)state)->Count==6101&&!recoveryBadPayload,"Recovery transfer or input validation differs.");
        foreach(int mode in Enumerable.Range(1,46).Concat(new[]{90,91}))
        {recoveryMode=mode;int committed=((ProbeState*)state)->Count;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0&&((ProbeState*)state)->Count==committed,"Malformed recovery/native error accepted: "+mode);if(mode>=90)Check(Output(output).Contains("lifecycle rejected"),"Recovery native diagnostic lost.");}
        recoveryMode=0;baseline->Bytes=520;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));baseline->Bytes=512;
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(RecoveryUnmarkedProbe),RecoveryHost(424,recovery:false))==0,Output(output));using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(424))
        {
            var old=(PhysicalServicesWire*)guard.Header;*old=services.Animation.Drive.Physical;((Services*)old)->Bytes=424;call.Services=(Services*)old;call.Operation=3;int reads=recoveryReads;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0&&Output(output).Contains("Declare IPhysicalRecoveryGame")&&recoveryReads==reads&&((ProbeState*)state)->Count==6200,"Unmarked physical consumer read recovery tail.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(!recoveryBadPayload,"Recovery callback received malformed SDK input.");
        checks.Add("recovery512/state144/command32 independent wires, marker dependency, protected496/older tails, constructor and exact generated requirement checks");
        checks.Add("synthetic recovery read/request transfer, all diagnostic fields, optional histories/support IDs, legal Jolt sequences1/2/255 at slots0/4095 and48 malformed/native-error cases and rollback of managed state; no native handback claim");
    }
}
