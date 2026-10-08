// SPDX-License-Identifier: Apache-2.0
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;

// Independent PODs and mock callbacks qualify SDK/real CoreCLR bridge transfer,
// not Runtime callback validation, controller physics, or NativeAOT publication.
[StructLayout(LayoutKind.Sequential)] struct CharacterIntent
{
    public uint Version,Bytes;public Id Entity;
    public float Right,Forward,Yaw,Pitch;public uint Flags,Reserved;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct CharacterServices
{
    public LayerServices Animation;
    public delegate* unmanaged[Cdecl]<void*,CharacterIntent*,void*,int> Set;
}
[GameModule("poima-test-character-services")]
public sealed class CharacterProbeGame : Game<ProbeState>,ICharacterInputGame
{
    public override void Initialize(ref ProbeState state){state.Count=60;}
    public static void Intent(GameContext context)
    {
        if(context.Inputs.Length!=0)throw new Exception("Compiled intent must not fabricate caller Inputs.");
        // Twelve invalid finite/range cases must reject without a native call.
        foreach(float invalid in new[]{float.NaN,float.PositiveInfinity,1.01f}){
            bool rejected=false;try{context.SetCharacterInput(new(11,40),invalid,0);}catch(ArgumentOutOfRangeException){rejected=true;}
            if(!rejected)throw new Exception("Invalid right accepted.");
            rejected=false;try{context.SetCharacterInput(new(11,40),0,-invalid);}catch(ArgumentOutOfRangeException){rejected=true;}
            if(!rejected)throw new Exception("Invalid forward accepted.");
        }
        foreach(float invalid in new[]{float.NaN,float.NegativeInfinity,180.01f}){
            bool rejected=false;try{context.SetCharacterInput(new(11,40),0,0,invalid);}catch(ArgumentOutOfRangeException){rejected=true;}
            if(!rejected)throw new Exception("Invalid yaw accepted.");
            rejected=false;try{context.SetCharacterInput(new(11,40),0,0,0,-invalid);}catch(ArgumentOutOfRangeException){rejected=true;}
            if(!rejected)throw new Exception("Invalid pitch accepted.");
        }
        context.SetCharacterInput(new(11,40),.25f,-.5f,12,-18,true);
        context.SetCharacterInput(new(11,41),0,0);
    }
    public override void Tick(ref ProbeState state,GameContext context)
    {
        // A character-only216 prefix cannot silently grant animation access.
        bool rejected=false;try{_=context.GetAnimationExtended(new(11,33));}catch(ArgumentException e){rejected=e.Message.Contains("Declare IInertialAnimationGame");}
        if(!rejected)throw new Exception("Unrequested inertial getter accepted.");
        rejected=false;try{context.SetAnimation(new(11,22),3,AnimationTransitionMode.Crossfade);}catch(ArgumentException e){rejected=e.Message.Contains("Declare IInertialAnimationGame");}
        if(!rejected)throw new Exception("Unrequested inertial setter accepted.");
        rejected=false;try{_=context.GetAnimationLayer(new(11,31),1);}catch(ArgumentException e){rejected=e.Message.Contains("Declare IMaskedAnimationGame");}
        if(!rejected)throw new Exception("Unrequested layer getter accepted.");
        rejected=false;try{context.SetAnimationLayer(new(11,31),1,2,.5);}catch(ArgumentException e){rejected=e.Message.Contains("Declare IMaskedAnimationGame");}
        if(!rejected)throw new Exception("Unrequested layer setter accepted.");
        Intent(context);++state.Count;
    }
    public override void Control(ref ProbeState state,ControlContext context){++state.Count;}
}
[GameModule("poima-test-character-inertial")]
public sealed class CharacterInertialProbe : Game<ProbeState>,ICharacterInputGame,IInertialAnimationGame
{
    public override void Initialize(ref ProbeState state){state.Count=70;}
    public override void Tick(ref ProbeState state,GameContext context)
    {CharacterProbeGame.Intent(context);_=context.GetAnimationExtended(new(11,33));++state.Count;}
}
[GameModule("poima-test-character-masked")]
public sealed class CharacterMaskedProbe : Game<ProbeState>,ICharacterInputGame,IMaskedAnimationGame
{
    public override void Initialize(ref ProbeState state){state.Count=80;}
    public override void Tick(ref ProbeState state,GameContext context)
    {CharacterProbeGame.Intent(context);_=context.GetAnimationLayer(new(11,31),1);++state.Count;}
}
[GameModule("poima-test-character-constructor")]
public sealed class CharacterConstructorProbe : Game<ProbeState>,ICharacterInputGame
{
    public CharacterConstructorProbe(){throw new Exception("CHARACTER CONSTRUCTOR REACHED");}
    public override void Initialize(ref ProbeState state){throw new Exception("CHARACTER INITIALIZE REACHED");}
    public override void Tick(ref ProbeState state,GameContext context){throw new Exception("CHARACTER TICK REACHED");}
}
[GameModule("poima-test-character-unmarked")]
public sealed class UnmarkedCharacterProbe : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state){state.Count=90;}
    public override void Tick(ref ProbeState state,GameContext context){context.SetCharacterInput(new(11,40),0,0);++state.Count;}
}
static unsafe partial class Program
{
    static int characterSets;static bool characterError;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int CharacterSet(void* context,CharacterIntent* intent,void* error)
    {
        ++characterSets;
        bool first=intent->Entity.Low==40;
        if((nint)context!=0x1234 || intent->Version!=1 || intent->Bytes!=48 || intent->Reserved!=0 || intent->Entity.High!=11 ||
            (first ? intent->Right!=.25f || intent->Forward!=-.5f || intent->Yaw!=12 || intent->Pitch!=-18 || intent->Flags!=1 :
                intent->Entity.Low!=41 || intent->Right!=0 || intent->Forward!=0 || intent->Yaw!=0 || intent->Pitch!=0 || intent->Flags!=0))badPayload=true;
        if(characterError){Encoding.UTF8.GetBytes("character rejected\0",new Span<byte>(error,2048));return -1;}
        return 0;
    }
    static JsonElement CharacterHost(uint bytes=216,bool character=true,bool inertial=false,bool layers=false)
        =>JsonSerializer.SerializeToElement(new{call_version=1,call_bytes=80,services_version=7,services_bytes=bytes,
            features=new[]{"baseline_v7",character?"character_input_v1":null,inertial?"animation_inertial_v1":null,layers?"animation_layers_v1":null}.Where(v=>v!=null).ToArray()});
    static void CharacterInputContract(byte* output,byte* state,Services good,List<string> checks)
    {
        Check(sizeof(CharacterIntent)==48 && sizeof(CharacterServices)==216 && sizeof(Services)==176 && sizeof(AnimationServices)==192 && sizeof(LayerServices)==208 &&
            Marshal.OffsetOf<CharacterServices>(nameof(CharacterServices.Set)).ToInt64()==208 &&
            Marshal.OffsetOf<CharacterIntent>(nameof(CharacterIntent.Entity)).ToInt64()==8 && Marshal.OffsetOf<CharacterIntent>(nameof(CharacterIntent.Right)).ToInt64()==24 &&
            Marshal.OffsetOf<CharacterIntent>(nameof(CharacterIntent.Yaw)).ToInt64()==32 && Marshal.OffsetOf<CharacterIntent>(nameof(CharacterIntent.Flags)).ToInt64()==40 &&
            Marshal.OffsetOf<CharacterIntent>(nameof(CharacterIntent.Reserved)).ToInt64()==44,"Independent character ABI layout mismatch.");
        Call call=new(){Version=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        foreach(var host in new JsonElement?[]{null,CharacterHost(176),CharacterHost(208),CharacterHost(215),CharacterHost(character:false),
            ExtensionJson("{\"call_version\":1,\"call_bytes\":80,\"services_version\":8,\"services_bytes\":216,\"features\":[\"baseline_v7\",\"character_input_v1\"]}"),
            ExtensionJson("{\"call_version\":1,\"call_bytes\":80,\"services_version\":7,\"services_bytes\":216,\"features\":[\"baseline_v7\",\"character_input_v1\",\"character_input_v1\"]}")}){
            Check(LoadExtension(&call,typeof(CharacterConstructorProbe),host)!=0 && !Output(output).Contains("CHARACTER CONSTRUCTOR REACHED"),"Invalid character host entered constructor.");
            NoExtensionModules(&call);
        }
        foreach(uint bytes in new uint[]{216,224}){
            Check(LoadExtension(&call,typeof(CharacterConstructorProbe),CharacterHost(bytes))!=0 && Output(output).Contains("CHARACTER CONSTRUCTOR REACHED"),"Named character negotiation failed positive constructor sentinel.");
            NoExtensionModules(&call);
        }
        checks.Add("character marker rejects absent, short, unnamed, wrong-epoch and duplicate hosts before constructor;216 and larger available prefixes reach positive sentinel");
        Check(LoadExtension(&call,typeof(CharacterProbeGame),CharacterHost())==0,Output(output));
        using(var document=JsonDocument.Parse(Output(output))){var manifest=document.RootElement;call.Handle=manifest.GetProperty("handle").GetUInt64();
            var r=manifest.GetProperty("requirements");Check(r.GetProperty("services_bytes").GetInt32()==216 && r.GetProperty("features").EnumerateArray().Select(v=>v.GetString()).SequenceEqual(new[]{"baseline_v7","character_input_v1"}) &&
                manifest.GetProperty("fields").GetArrayLength()==1 && manifest.GetProperty("bytes").GetInt32()==4,"Character-only manifest/state schema mismatch.");}
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==60,Output(output));
        CharacterServices services=new(){Animation=new(){Animation=new(){Baseline=good}},Set=&CharacterSet};services.Animation.Animation.Baseline.Bytes=216;
        // Poison all unrelated animation callbacks. A character-only feature
        // must neither demand these pointers nor allow its game to call them.
        new Span<byte>((byte*)&services+176,32).Fill(0xa5);
        foreach(uint operation in new uint[]{3,6}){
            call.Operation=operation;
            foreach(int size in new[]{176,192,208}){
                using var guarded=new GuardedHeader(size);*(Services*)guarded.Header=good;((Services*)guarded.Header)->Bytes=(uint)size;call.Services=(Services*)guarded.Header;
                int before=((ProbeState*)state)->Count,writes=characterSets;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==before && writes==characterSets,"Short character table read protected tail or executed game.");
            }
            var missing=services;missing.Set=null;call.Services=&missing.Animation.Animation.Baseline;
            int prior=((ProbeState*)state)->Count,calls=characterSets;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==prior && calls==characterSets,"Missing character callback executed game.");
        }
        checks.Add("character Tick/Control reject actual176/192/208 guarded allocations and missing own callback before execution");
        call.Services=&services.Animation.Animation.Baseline;call.Operation=3;characterSets=0;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==61 && characterSets==2 && !badPayload,Output(output));
        call.Operation=6;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==62 && characterSets==2,Output(output));
        services.Animation.Animation.Baseline.Bytes=224;call.Operation=3;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==63 && characterSets==4 && !badPayload,Output(output));
        checks.Add("independent48-byte character commands transfer explicit and neutral defaults;12 invalid arguments never call native; character-only216/224 tolerates poisoned unrequested animation pointers while all four animation helpers deny access");
        characterError=true;int beforeError=((ProbeState*)state)->Count;
        Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output)=="character rejected" && ((ProbeState*)state)->Count==beforeError,"Native character error transfer differs.");
        characterError=false;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && ((ProbeState*)state)->Count==beforeError+1 && !badPayload,Output(output));
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("mock character callback error preserves diagnostic and gameplay state; subsequent Tick and collectible cleanup succeed");
        foreach(bool masked in new[]{false,true}){
            var type=masked?typeof(CharacterMaskedProbe):typeof(CharacterInertialProbe);
            Check(LoadExtension(&call,type,CharacterHost(inertial:true,layers:masked))==0,Output(output));
            using(var document=JsonDocument.Parse(Output(output))){call.Handle=document.RootElement.GetProperty("handle").GetUInt64();
                var names=document.RootElement.GetProperty("requirements").GetProperty("features").EnumerateArray().Select(v=>v.GetString()).ToArray();
                Check(names.Contains("character_input_v1") && names.Contains("animation_inertial_v1") && names.Contains("animation_layers_v1")==masked,"Combined feature declaration differs.");}
            call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            services=new(){Animation=new(){Animation=new(){Baseline=good,Get=&ExtendedGet,Set=&ExtendedSet},Get=masked?&LayerGet:null,Set=masked?&LayerSet:null},Set=&CharacterSet};
            services.Animation.Animation.Baseline.Bytes=216;
            for(int slot=0;slot<(masked?4:2);++slot){
                var missing=services;
                if(slot==0)missing.Animation.Animation.Get=null;else if(slot==1)missing.Animation.Animation.Set=null;else if(slot==2)missing.Animation.Get=null;else missing.Animation.Set=null;
                call.Services=&missing.Animation.Animation.Baseline;call.Operation=3;int prior=((ProbeState*)state)->Count,writes=characterSets;
                Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && ((ProbeState*)state)->Count==prior && characterSets==writes,"Requested combined feature accepted missing callback.");
            }
            call.Services=&services.Animation.Animation.Baseline;call.Operation=3;int oldCharacter=characterSets,oldAnimation=masked?layerGets:extendedGets;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && characterSets==oldCharacter+2 && (masked?layerGets:extendedGets)==oldAnimation+1 && !badPayload,Output(output));
            call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        }
        checks.Add("character+inertial and character+masked profiles transfer through requested callbacks only; absent requested callbacks reject before game, unrequested layer nulls remain valid");
        Check(LoadExtension(&call,typeof(UnmarkedCharacterProbe),null)==0,Output(output));
        using(var document=JsonDocument.Parse(Output(output)))call.Handle=document.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
        using(var guarded=new GuardedHeader(176)){
            *(Services*)guarded.Header=good;call.Services=(Services*)guarded.Header;call.Operation=3;int writes=characterSets;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Declare ICharacterInputGame") && ((ProbeState*)state)->Count==90 && characterSets==writes,"Unmarked character helper read protected tail.");
        }
        call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));NoExtensionModules(&call);
        checks.Add("unmarked compiled character helper rejects before reading an actual176 guarded baseline tail");
    }
}
