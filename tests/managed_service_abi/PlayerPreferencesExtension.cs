// SPDX-License-Identifier: Apache-2.0
// Independent wire declarations: tests never link the SDK's internal PODs.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

[StructLayout(LayoutKind.Sequential)] struct PreferenceValues
{ public double Fov,X,Y,Ui,Gain;public uint InvertX,InvertY,Samples,Frames; }
[StructLayout(LayoutKind.Sequential)] unsafe struct PreferenceSnapshot
{
    public uint Version,Bytes,Available,Replay;public Id Owner;public ulong Revision;
    public uint Overrides,ValueMask;public PreferenceValues Values;public fixed uint Sources[9];
    public uint NextSamples,NextFrames,ObservationMask,Audio,Reserved;
    public ulong Observed,Applied,Presented;public double Fov,Ui,RequestedGain,SinkGain;
}
[StructLayout(LayoutKind.Sequential)] struct PreferencePatch
{ public uint Version,Bytes;public Id Owner;public ulong Revision;public uint Set,Reset;public PreferenceValues Values;public ulong Reserved; }
[StructLayout(LayoutKind.Sequential)] struct PreferenceEnqueue
{ public uint Version,Bytes;public Ticket Ticket;public uint Rejection,Reserved; }
[StructLayout(LayoutKind.Sequential)] struct PreferenceResult
{ public uint Version,Bytes;public Ticket Ticket;public uint State,Rejection;public ulong Revision;public int Error;public uint Reserved; }
[StructLayout(LayoutKind.Sequential)] unsafe struct PreferenceServices
{
    public InstanceServices Instances;
    public delegate* unmanaged[Cdecl]<void*,PreferenceSnapshot*,void*,int> Snapshot;
    public delegate* unmanaged[Cdecl]<void*,PreferencePatch*,PreferenceEnqueue*,void*,int> Patch;
    public delegate* unmanaged[Cdecl]<void*,Ticket*,PreferenceResult*,void*,int> Result;
}
[GameModule("poima-test-preference-extension")]
public sealed class PreferenceProbeGame : Game<ProbeState>,IPlayerPreferencesGame
{
    public override void Initialize(ref ProbeState state){state.Count=800;}
    internal static void One(GameContext context)
    {
        var snapshot=context.GetPlayerPreferences();
        if(!snapshot.Available || snapshot.Owner!=new PlayerPreferenceOwner(11,90) || snapshot.Revision!=7 ||
            snapshot.Values.VerticalFov is not null || snapshot.Values.UiScale!=16 || snapshot.Observation.EffectiveUiScale!=16 ||
            snapshot.Values.Samples!=1 || snapshot.NextSamples!=4 || snapshot.Sources.UiScale!=PlayerPreferenceSource.WindowDensity)
            throw new Exception("Preference snapshot lost optional inheritance, actual density or frozen/next graphics distinction.");
        var staged=context.TryStagePlayerPreferences(new(snapshot.Owner,7,new(VerticalFov:75,InvertY:true,UiScale:2,MasterGain:.5),PlayerPreferenceFields.SensitivityX));
        if(!staged.Staged || staged.Ticket!=new PlayerPreferenceTicket(11,90,9))throw new Exception("Preference enqueue lost ticket.");
        var result=context.GetPlayerPreferenceResult(staged.Ticket!.Value);
        if(result.State!=PlayerPreferenceResultState.Accepted || result.AcceptedRevision!=8)throw new Exception("Preference query lost accepted configuration receipt.");
    }
    public override void Tick(ref ProbeState state,GameContext context)
    {
        bool denied=false;try{_=context.ResolveNode(new(11,80),new(12,81));}catch(ArgumentException e){denied=e.Message.Contains("Declare IHierarchicalInstancesGame");}
        if(!denied)throw new Exception("Preference prefix granted instances.");
        denied=false;try{context.SetCharacterInput(new(11,40),0,0);}catch(ArgumentException e){denied=e.Message.Contains("Declare ICharacterInputGame");}
        if(!denied)throw new Exception("Preference prefix granted character input.");
        Span<NavigationPoint> corners=stackalloc NavigationPoint[2];
        denied=false;try{_=context.FindNavigationPath(new(11,40),new(1,2,3),corners);}catch(ArgumentException e){denied=e.Message.Contains("Declare INavigationGame");}
        if(!denied)throw new Exception("Preference prefix granted navigation.");
        denied=false;try{_=context.GetAnimationExtended(new(11,33));}catch(ArgumentException e){denied=e.Message.Contains("Declare IInertialAnimationGame");}
        if(!denied)throw new Exception("Preference prefix granted inertial animation.");
        denied=false;try{_=context.GetAnimationLayer(new(11,31),1);}catch(ArgumentException e){denied=e.Message.Contains("Declare IMaskedAnimationGame");}
        if(!denied)throw new Exception("Preference prefix granted animation layers.");
        foreach(var patch in new PlayerPreferencePatch[]{new(default,7,default),
            new(new(11,90),9007199254740992,default),new(new(11,90),7,new(VerticalFov:double.NaN)),
            new(new(11,90),7,new(SensitivityX:11)),new(new(11,90),7,new(UiScale:16)),new(new(11,90),7,new(Samples:2)),
            new(new(11,90),7,new(FramesInFlight:3)),new(new(11,90),7,new(InvertX:true),PlayerPreferenceFields.InvertX),
            new(new(11,90),7,default,(PlayerPreferenceFields)512)})
        {
            denied=false;try{_=context.TryStagePlayerPreferences(patch);}catch(ArgumentException){denied=true;}
            if(!denied)throw new Exception("Malformed local preference patch reached host.");
        }
        foreach(var ticket in new PlayerPreferenceTicket[]{default,new(11,90,0),new(0,0,1),new(11,90,9007199254740992)})
        {
            denied=false;try{_=context.GetPlayerPreferenceResult(ticket);}catch(ArgumentException){denied=true;}
            if(!denied)throw new Exception("Malformed local preference ticket reached host.");
        }
        One(context);++state.Count;
    }
    public override void Control(ref ProbeState state,ControlContext context)
    {
        var snapshot=context.GetPlayerPreferences();
        var staged=context.TryStagePlayerPreferences(new(snapshot.Owner,7,new(VerticalFov:75,InvertY:true,UiScale:2,MasterGain:.5),PlayerPreferenceFields.SensitivityX));
        if(staged.Ticket is not { } ticket || context.GetPlayerPreferenceResult(ticket).AcceptedRevision!=8)
            throw new Exception("Control preference extension lost negotiated feature.");
        ++state.Count;
    }
}
[GameModule("poima-test-preference-constructor")]
public sealed class PreferenceConstructorProbe : Game<ProbeState>,IPlayerPreferencesGame
{
    public PreferenceConstructorProbe(){throw new Exception("PREFERENCE CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){throw new Exception("PREFERENCE INITIALIZE REACHED");}
    public override void Tick(ref ProbeState state,GameContext context){throw new Exception("PREFERENCE TICK REACHED");}
}
[GameModule("poima-test-preference-unmarked")]
public sealed class PreferenceUnmarkedProbe : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state){state.Count=900;}
    public override void Tick(ref ProbeState state,GameContext context){_=context.GetPlayerPreferences();++state.Count;}
    public override void Control(ref ProbeState state,ControlContext context){_=context.GetPlayerPreferences();++state.Count;}
}
static unsafe partial class Program
{
    static int preferenceCalls,preferenceReplyMode;
    static bool PreferenceContext(void* context)=>((nint)context)==0x1234;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PreferenceRead(void* context,PreferenceSnapshot* output,void* error)
    {
        ++preferenceCalls;
        if(!PreferenceContext(context) || output->Version!=1 || output->Bytes!=216 || output->Reserved!=0)badPayload=true;
        if(preferenceReplyMode==1)return LifecycleError(error);
        *output=new(){Version=1,Bytes=216,Available=1,Owner=new(){High=11,Low=90},Revision=7,ValueMask=510,
            Values=new(){X=.1,Y=.2,Ui=16,Gain=1,Samples=1,Frames=1},NextSamples=4,NextFrames=2,
            ObservationMask=17,Observed=7,Ui=16,RequestedGain=1};
        output->Sources[5]=(uint)PlayerPreferenceSource.WindowDensity;
        switch(preferenceReplyMode)
        {
            case 2:output->Version=2;break;
            case 3:output->Bytes=224;break;
            case 4:output->Reserved=1;break;
            case 5:output->Available=0;break;
            case 6:output->Values.X=double.NaN;break;
            case 7:output->Values.Fov=60;break;
            case 8:output->Applied=1;break;
            case 9:output->Values.InvertY=2;break;
            case 10:output->ValueMask|=512;break;
            case 11:output->Sources[0]=9;break;
            case 12:output->Overrides=32;break; // Configured UI scale16 is invalid; inherited density16 is valid.
            case 13:output->Ui=double.PositiveInfinity;break;
            case 14:output->Revision=9007199254740992;break;
        }
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PreferenceStage(void* context,PreferencePatch* input,PreferenceEnqueue* output,void* error)
    {
        ++preferenceCalls;
        if(!PreferenceContext(context) || input->Version!=1 || input->Bytes!=104 || input->Owner.High!=11 || input->Owner.Low!=90 || input->Revision!=7 ||
            input->Set!=305 || input->Reset!=2 || input->Values.Fov!=75 || input->Values.Ui!=2 || input->Values.Gain!=.5 || input->Values.InvertY!=1 ||
            input->Values.X!=0 || input->Values.Y!=0 || input->Values.InvertX!=0 || input->Values.Samples!=0 || input->Values.Frames!=0 || input->Reserved!=0 ||
            output->Version!=1 || output->Bytes!=40 || output->Reserved!=0)badPayload=true;
        *output=new(){Version=1,Bytes=40,Ticket=new(){High=11,Low=90,Sequence=9}};
        switch(preferenceReplyMode)
        {
            case 101:output->Version=2;break;
            case 102:output->Bytes=48;break;
            case 103:output->Reserved=1;break;
            case 104:output->Ticket.Sequence=0;break;
            case 105:output->Ticket.High=12;break;
            case 106:output->Rejection=1;break;
            case 107:output->Rejection=8;break;
        }
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int PreferenceQuery(void* context,Ticket* input,PreferenceResult* output,void* error)
    {
        ++preferenceCalls;
        if(!PreferenceContext(context) || input->High!=11 || input->Low!=90 || input->Sequence!=9 || output->Version!=1 || output->Bytes!=56 || output->Reserved!=0)badPayload=true;
        *output=new(){Version=1,Bytes=56,Ticket=*input,State=2,Revision=8};
        switch(preferenceReplyMode)
        {
            case 201:output->Version=2;break;
            case 202:output->Bytes=64;break;
            case 203:output->Reserved=1;break;
            case 204:output->Ticket.Sequence=10;break;
            case 205:output->State=3;break;
            case 206:output->Rejection=1;break;
            case 207:output->Revision=0;break;
            case 208:output->Error=1;break;
            case 209:output->Revision=9007199254740992;break;
        }
        return 0;
    }
    static JsonElement PreferenceHost(uint bytes=256,bool preferences=true)
        =>JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=7,services_bytes=bytes,
            features=preferences?new[]{"baseline_v7","player_preferences_v1"}:new[]{"baseline_v7"}});
    static void PlayerPreferencesContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(PreferenceValues)==56 && sizeof(PreferenceSnapshot)==216 && sizeof(PreferencePatch)==104 && sizeof(PreferenceEnqueue)==40 && sizeof(PreferenceResult)==56 &&
            sizeof(PreferenceServices)==256 && Marshal.OffsetOf<PreferenceServices>(nameof(PreferenceServices.Snapshot)).ToInt64()==232 &&
            Marshal.OffsetOf<PreferenceServices>(nameof(PreferenceServices.Patch)).ToInt64()==240 && Marshal.OffsetOf<PreferenceServices>(nameof(PreferenceServices.Result)).ToInt64()==248,
            "Independent preference wire layout mismatch.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,PreferenceHost(176),PreferenceHost(192),PreferenceHost(208),PreferenceHost(216),PreferenceHost(224),PreferenceHost(232),PreferenceHost(255),PreferenceHost(preferences:false)})
        {
            Check(LoadExtension(&call,typeof(PreferenceConstructorProbe),host)!=0 && !Output(output).Contains("PREFERENCE CONSTRUCTOR REACHED"),"Invalid preference host entered constructor.");NoExtensionModules(&call);
        }
        foreach(uint bytes in new[]{256u,272u})
        { Check(LoadExtension(&call,typeof(PreferenceConstructorProbe),PreferenceHost(bytes))!=0 && Output(output).Contains("PREFERENCE CONSTRUCTOR REACHED"),"Valid preference host failed constructor sentinel.");NoExtensionModules(&call); }
        Check(LoadExtension(&call,typeof(PreferenceProbeGame),PreferenceHost())==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))
        {
            call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();var requirements=doc.RootElement.GetProperty("requirements");
            Check(requirements.GetProperty("services_bytes").GetUInt32()==256 && requirements.GetProperty("features").EnumerateArray().Select(v=>v.GetString()).SequenceEqual(new[]{"baseline_v7","player_preferences_v1"}),"Preference-only profile granted unrelated services.");
        }
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        PreferenceServices services=new(){Instances=new(){Navigation=new(){Character=new(){Animation=new(){Animation=new(){Baseline=good}}}}},Snapshot=&PreferenceRead,Patch=&PreferenceStage,Result=&PreferenceQuery};
        services.Instances.Navigation.Character.Animation.Animation.Baseline.Bytes=256;
        new Span<byte>((byte*)&services+176,56).Fill(0xa5);preferenceCalls=preferenceReplyMode=0;
        foreach(uint operation in new[]{3u,6u})
        {
            call.Operation=operation;
            foreach(int bytes in new[]{176,192,208,216,224,232,248,255})
            {
                using var guard=new GuardedHeader(bytes);*(Services*)guard.Header=good;((Services*)guard.Header)->Bytes=(uint)bytes;call.Services=(Services*)guard.Header;
                int prior=((ProbeState*)state)->Count,calls=preferenceCalls;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==prior && preferenceCalls==calls,"Short protected preference allocation entered game/read tail.");
            }
            for(int missing=0;missing<3;++missing)
            {
                var absent=services;if(missing==0)absent.Snapshot=null;else if(missing==1)absent.Patch=null;else absent.Result=null;
                call.Services=&absent.Instances.Navigation.Character.Animation.Animation.Baseline;int prior=((ProbeState*)state)->Count;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==prior,"Missing preference callback entered game.");
            }
            call.Services=&services.Instances.Navigation.Character.Animation.Animation.Baseline;int before=((ProbeState*)state)->Count;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==before+1 && !badPayload,Output(output));
            foreach(int mode in Enumerable.Range(1,14).Concat(Enumerable.Range(101,7)).Concat(Enumerable.Range(201,9)))
            {
                preferenceReplyMode=mode;before=((ProbeState*)state)->Count;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==before,"Malformed preference output mutated game state: "+mode);
            }
            preferenceReplyMode=0;
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        Check(LoadExtension(&call,typeof(PreferenceUnmarkedProbe),null)==0,Output(output));
        using(var doc=JsonDocument.Parse(Output(output)))call.Handle=doc.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guard=new GuardedHeader(176))
        {
            *(Services*)guard.Header=good;call.Services=(Services*)guard.Header;
            foreach(uint operation in new[]{3u,6u})
            {
                call.Operation=operation;int calls=preferenceCalls;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare IPlayerPreferencesGame") && ((ProbeState*)state)->Count==900 && preferenceCalls==calls,"Unmarked preference access read protected tail.");
            }
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("preferences independent named256/larger negotiation precedes constructor; protected short allocations/unmarked access and missing own callbacks reject; poisoned unrelated tails are unread; exact sparse patch/ticket/result, nullable inheritance, actual density16 and frozen graphics transfer; malformed outputs roll back Tick/Control state");
    }
}
