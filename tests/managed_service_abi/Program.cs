// SPDX-License-Identifier: Apache-2.0
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Poima;
using Poima.ManagedBridge;
using Poima.Tests;
using System.Buffers.Binary;

// Independent ABI declarations: do not link production internal POD types.
[StructLayout(LayoutKind.Sequential)] unsafe struct Call
{
    public uint Version, Operation; public ulong Handle; public byte* Text, State;
    public uint StateBytes, InputCount; public Services* Services; public void* Inputs;
    public ulong Tick; public byte* Output; public uint OutputCapacity, Reserved;
}
[StructLayout(LayoutKind.Sequential)] unsafe struct Services
{
    public uint Version, Bytes; public void* Context;
    public nint Entity, Raycast, Move, Sound;
    public delegate* unmanaged[Cdecl]<void*, Id*, Animation*, void*, int> Get;
    public delegate* unmanaged[Cdecl]<void*, Command*, void*, int> Set;
    public delegate* unmanaged[Cdecl]<void*, SaveInfo*, void*, int> SaveInfo;
    public delegate* unmanaged[Cdecl]<void*, SaveRequest*, SaveEnqueue*, void*, int> SaveRequest;
    public delegate* unmanaged[Cdecl]<void*, Ticket*, SaveResult*, void*, int> SaveResult;
    public delegate* unmanaged[Cdecl]<void*, ComponentType*, Id*, Id*, uint, uint*, void*, int> Query;
    public delegate* unmanaged[Cdecl]<void*, ComponentType*, Id*, void*, uint, uint*, void*, int> ComponentGet;
    public delegate* unmanaged[Cdecl]<void*, ComponentType*, Id*, void*, uint, void*, int> ComponentSet;
    public delegate* unmanaged[Cdecl]<void*, Id*, uint*, void*, int> Alive;
    public delegate* unmanaged[Cdecl]<void*, RecipeId*, Transform*, Id*, void*, int> Spawn;
    public delegate* unmanaged[Cdecl]<void*, Id*, void*, int> Despawn;
    public delegate* unmanaged[Cdecl]<void*, ComponentType*, RecipeId*, void*, uint, uint*, void*, int> TemplateGet;
    public delegate* unmanaged[Cdecl]<void*,Id*,UiState*,byte*,uint,uint*,void*,int> UiGet;
    public delegate* unmanaged[Cdecl]<void*,UiPatch*,uint,UiModal*,void*,int> UiEdit;
    public delegate* unmanaged[Cdecl]<void*,UiEvent*,void*,int> ControlInfo;
    public delegate* unmanaged[Cdecl]<void*,uint,void*,int> ControlRequest;
}
[StructLayout(LayoutKind.Sequential)] struct UiState { public ulong Revision;public uint Kind,Visible,Enabled,EffectiveVisible,EffectiveEnabled,Eligible,TextBytes,Reserved; }
[StructLayout(LayoutKind.Sequential)] unsafe struct UiPatch { public Id Id;public byte* Text;public uint TextBytes,Mask,Visible,Enabled; }
[StructLayout(LayoutKind.Sequential)] struct UiModal { public Id Id;public uint Change,Reserved; }
[StructLayout(LayoutKind.Sequential)] unsafe struct UiEvent { public Id Element;public ulong Sequence;public uint ActionBytes,Reserved;public fixed byte Action[128]; }
[StructLayout(LayoutKind.Sequential)] struct RecipeId { public ulong High,Low; }
[StructLayout(LayoutKind.Sequential)] unsafe struct Transform { public fixed double Position[3];public fixed double Rotation[4];public fixed double Scale[3]; }
[StructLayout(LayoutKind.Sequential)] struct ComponentType { public Id Id;public ulong A,B,C,D;public uint Bytes,Reserved; }
[StructLayout(LayoutKind.Sequential)] struct Ticket { public ulong High,Low,Sequence; }
[StructLayout(LayoutKind.Sequential)] unsafe struct SaveRequest { public uint Kind,SlotBytes;public byte* Slot;public ulong Expected;public uint HasExpected,Recovery; }
[StructLayout(LayoutKind.Sequential)] struct SaveEnqueue { public Ticket Ticket;public uint Rejection,Reserved; }
[StructLayout(LayoutKind.Sequential)] unsafe struct SaveResult
{ public Ticket Ticket;public uint Kind,State;public ulong Requested,Committed,Generation;public uint Recovered;public int Error;public ulong High,Low,Tick;public fixed byte Diagnostic[256]; }
[StructLayout(LayoutKind.Sequential)] struct SaveInfo
{ public ulong High,Low,Configuration;public uint Enabled,RestorePresent;public Ticket Ticket;public ulong DestinationHigh,DestinationLow,SourceTick,RestoredTick,Generation;public uint Recovered,Reserved; }
[StructLayout(LayoutKind.Sequential)] struct Id { public ulong High, Low; }
[StructLayout(LayoutKind.Sequential)] struct Command
{ public Id Entity; public double Time, Speed; public int Clip; public uint Loop, Playing, BlendTicks; }
[StructLayout(LayoutKind.Sequential)] struct Transition
{
    public ulong Start; public double Weight, Time, Speed; public uint Duration, Elapsed;
    public int Clip; public uint Frozen, Loop, Playing;
}
[StructLayout(LayoutKind.Sequential)] struct Animation
{
    public Id Entity; public double Time, Speed, Duration; public int Clip;
    public uint Present, Loop, Playing, TransitionPresent, Reserved; public Transition Transition;
}
[StructLayout(LayoutKind.Sequential)] public struct ProbeState { public int Count; }
[StructLayout(LayoutKind.Sequential)] public struct UnsupportedTemplateState { public TemplateId Recipe; }
[GameModule("poima-test-template-state-rejected")]
public sealed class UnsupportedTemplateGame : Game<UnsupportedTemplateState>
{
    public override void Initialize(ref UnsupportedTemplateState state) { }
    public override void Tick(ref UnsupportedTemplateState state,GameContext context) { }
}
[GameModule("poima-test-ui-services-v7")]
public sealed class UiProbeGame : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state) {state.Count=10;}
    public override void Tick(ref ProbeState state,GameContext context) { context.SetUi(new(1,2),"New é",true,false); }
    public override void Control(ref ProbeState state,ControlContext context)
    {
        if(context.Tick!=123 || context.Sequence!=42 || context.Element!=new UiId(1,3) || context.Action!="save")throw new Exception("Control event mismatch.");
        var value=context.GetUi(new(1,2));if(value.Revision!=9 || value.Kind!=UiKind.Label || value.Text!="Old" || !value.Visible || value.Eligible)throw new Exception("UI snapshot mismatch.");
        context.SetUi(new(1,2),"New é",true,false);context.SetModal(null);context.RequestResume();context.RequestPause();++state.Count;
    }
}
[GameModule("poima-test-services-v7")]
public sealed class ProbeGame : Game<ProbeState>
{
    public override void Initialize(ref ProbeState state) { state.Count=7; }
    public override void Tick(ref ProbeState state, GameContext context)
    {
        ++state.Count;
        if(context.Tick!=123)throw new Exception("Tick ABI mismatch.");
        var a=context.GetAnimation(new(11,22)) ?? throw new Exception("Missing rig.");
        if(a.Entity!=new EntityId(11,22) || a.Clip!=3 || a.Time!=.125 || a.Speed!=2 || a.Duration!=4 || !a.Loop || a.Playing)
            throw new Exception("Animation state ABI mismatch.");
        var t=a.Transition ?? throw new Exception("Missing transition.");
        if(t.StartTick!=99 || t.DurationTicks!=32 || t.ElapsedTicks!=24 || t.Weight!=.75 || t.SourceFrozen ||
            t.SourceClip!=2 || t.SourceTime!=.5 || t.SourceSpeed!=.25 || t.SourceLoop!=false || t.SourcePlaying!=true)
            throw new Exception("Transition ABI mismatch.");
        var frozen=context.GetAnimation(new(11,23))!.Value.Transition!.Value;
        if(!frozen.SourceFrozen || frozen.SourceClip!=null || frozen.SourceTime!=null || frozen.SourceSpeed!=null || frozen.SourceLoop!=null || frozen.SourcePlaying!=null)
            throw new Exception("Frozen source must hide clock metadata.");
        if(context.GetAnimation(new(11,24))!=null)throw new Exception("Non-rig must be null.");
        context.SetAnimation(new(11,22),null,.375,1.5,false,true,17);
        var saves=context.Saves;
        if(!saves.Enabled || saves.Epoch!=new SaveEpoch(-1,22) || saves.ConfigurationGeneration!=19 || saves.LastRestore is not {} restore ||
            restore.InitiatingTicket!=new SaveTicket(long.MinValue,21,6) || restore.DestinationEpoch!=new SaveEpoch(-1,22) ||
            restore.CommittedSourceTick!=80 || restore.RestoredTick!=14 || restore.Generation!=3 || !restore.Recovered)
            throw new Exception("Save capability/restore ABI mismatch.");
        var complete=context.GetSaveResult(new(-1,22,7));
        if(complete.Ticket!=new SaveTicket(-1,22,7) || complete.Kind!=SaveKind.Load || complete.State!=SaveOperationState.Succeeded ||
            complete.RequestedTick!=22 || complete.CommittedTick!=27 || complete.Generation!=5 || !complete.Recovered || complete.ErrorCode!=0 ||
            complete.RestoredEpoch!=new SaveEpoch(long.MinValue,33) || complete.RestoredTick!=12 || complete.Diagnostic!="restored" || !complete.IsTerminal)
            throw new Exception("Save result ABI mismatch.");
        var uncertain=context.GetSaveResult(new(-1,22,8));
        if(uncertain.State!=SaveOperationState.Resolving || uncertain.ErrorCode!=-32070 || uncertain.Diagnostic!="uncertain" || uncertain.IsTerminal || uncertain.RestoredEpoch!=null || uncertain.RestoredTick!=null)
            throw new Exception("Uncertain save became terminal.");
        if(context.GetSaveResult(default).State!=SaveOperationState.Expired)throw new Exception("Unknown save token did not expire.");
        if(context.RequestSave("quick",42)!=new SaveTicket(-1,22,101))throw new Exception("Save enqueue mismatch.");
        var queued=context.TryRequestLoad("chapter-1",null,true);
        if(!queued.Accepted || queued.Ticket!=new SaveTicket(-1,22,102))throw new Exception("Load enqueue mismatch.");
        if(context.TryRequestSave("bad/path").Rejection!=SaveRequestRejection.Invalid || context.TryRequestLoad("quick",-1).Accepted)
            throw new Exception("Invalid request escaped SDK bounds.");
        bool rejected=false;
        try { context.RequestLoad("busy"); }catch(SaveRequestException error) { rejected=error.Rejection==SaveRequestRejection.Busy; }
        if(!rejected)throw new Exception("Rejected request lost its typed reason.");
        Span<EntityId> ids=stackalloc EntityId[2];
        if(context.Query<Health>(ids)!=2 || ids[0]!=new EntityId(11,22) || ids[1]!=new EntityId(11,23) ||
            context.Query<Health>(ids,ids[1])!=0)throw new Exception("Component query ABI mismatch.");
        var health=context.Get<Health>(new(11,22));
        if(health.Current!=12.5f || health.Maximum!=42 || health.Score!=long.MinValue)throw new Exception("Health wire decode mismatch.");
        if(context.TryGet<Health>(new(11,24),out _))throw new Exception("Missing component was present.");
        var interaction=context.Get<Interaction>(new(11,22));
        if(interaction.Target!=new EntityId(ulong.MaxValue,1UL<<63) || interaction.Weight!=.125)throw new Exception("Interaction wire decode mismatch.");
        health.Current=-0.0f;health.Maximum=-9;health.Score=long.MaxValue;context.Set(new(11,22),in health);
        interaction.Weight=-0.0;context.Set(new(11,22),in interaction);
        bool finiteRejected=false;health.Current=float.NaN;
        try { context.Set(new(11,22),in health); }catch(ArgumentException){finiteRejected=true;}
        if(!finiteRejected)throw new Exception("Nonfinite generated encode accepted.");
        if(!context.IsAlive(new(11,22)) || context.IsAlive(default))throw new Exception("Liveness ABI mismatch.");
        var template=new TemplateId(0xfedcba9876543210,22);
        var defaults=context.GetTemplate<Health>(template);
        if(defaults.Current!=99 || defaults.Maximum!=100 || defaults.Score!=long.MinValue)throw new Exception("Frozen template payload mismatch.");
        if(context.TryGetTemplate<Health>(new(template.High,24),out _))throw new Exception("Absent template component was present.");
        var first=context.Spawn(template);
        var second=context.Spawn(template,new SpawnTransform(new(1.25,-2.5,3.75),System.Numerics.Quaternion.Identity,new(2,3,4)));
        if(first!=new EntityId(ulong.MaxValue,1001) || second!=new EntityId(ulong.MaxValue,1002) || context.IsAlive(first))throw new Exception("Reserved spawn ID transfer/publication mismatch.");
        defaults.Current=8;context.Set(first,in defaults);
        if(context.GetTemplate<Health>(template).Current!=99)throw new Exception("Template read observed instance initialization.");
        context.Despawn(first);context.Despawn(second);
        bool unknown=false;try {context.GetTemplate<Health>(new(template.High,999));}catch(InvalidOperationException e){unknown=e.Message=="lifecycle rejected";}
        if(!unknown)throw new Exception("Unknown template error was not transferred.");
        bool presence=false;try {context.TryGetTemplate<Health>(new(template.High,25),out _);}catch(InvalidOperationException){presence=true;}
        if(!presence)throw new Exception("Invalid template presence flag accepted.");
        bool zero=false;try {context.Spawn(new(template.High,26));}catch(InvalidOperationException){zero=true;}
        if(!zero)throw new Exception("Zero spawn result accepted.");
        bool spawnError=false;try {context.Spawn(new(template.High,999));}catch(InvalidOperationException e){spawnError=e.Message=="lifecycle rejected";}
        if(!spawnError)throw new Exception("Spawn error was not transferred.");
        bool despawnError=false;try {context.Despawn(new(ulong.MaxValue,999));}catch(InvalidOperationException e){despawnError=e.Message=="lifecycle rejected";}
        if(!despawnError)throw new Exception("Despawn error was not transferred.");
    }
}
static unsafe partial class Program
{
    static int gets, sets,infos,requests,results,queries,componentGets,componentSets,aliveCalls,spawns,despawns,templateGets;
    static bool badPayload;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Unused() => -1;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Get(void* context, Id* id, Animation* output, void* error)
    {
        ++gets; if((nint)context!=0x1234 || id->High!=11)badPayload=true;
        *output=new Animation {Entity=*id,Present=id->Low==24 ? 0u:1u,Clip=3,Time=.125,Speed=2,Duration=4,Loop=1,Playing=0,TransitionPresent=1,
            Transition=new Transition {Start=99,Duration=32,Elapsed=24,Weight=.75,Clip=2,Time=.5,Speed=.25,Frozen=id->Low==23 ? 1u:0u,Loop=0,Playing=1}};
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Set(void* context, Command* value, void* error)
    {
        ++sets;
        if((nint)context!=0x1234 || value->Entity.High!=11 || value->Entity.Low!=22 || value->Clip!=-1 ||
            value->Time!=.375 || value->Speed!=1.5 || value->Loop!=0 || value->Playing!=1 || value->BlendTicks!=17)badPayload=true;
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Info(void* context,SaveInfo* output,void* error)
    {
        ++infos;if((nint)context!=0x1234)badPayload=true;
        *output=new(){High=ulong.MaxValue,Low=22,Configuration=19,Enabled=1,RestorePresent=1,
            Ticket=new(){High=1UL<<63,Low=21,Sequence=6},DestinationHigh=ulong.MaxValue,DestinationLow=22,SourceTick=80,RestoredTick=14,Generation=3,Recovered=1};
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Request(void* context,SaveRequest* request,SaveEnqueue* output,void* error)
    {
        ++requests;if((nint)context!=0x1234 || request->SlotBytes>64) {badPayload=true;return -1;}
        string slot=Encoding.UTF8.GetString(new ReadOnlySpan<byte>(request->Slot,(int)request->SlotBytes));
        if(slot=="quick") {
            if(request->Kind!=1 || request->Expected!=42 || request->HasExpected!=1 || request->Recovery!=0)badPayload=true;
            *output=new(){Ticket=new(){High=ulong.MaxValue,Low=22,Sequence=101}};
        } else if(slot=="chapter-1") {
            if(request->Kind!=2 || request->Expected!=0 || request->HasExpected!=0 || request->Recovery!=1)badPayload=true;
            *output=new(){Ticket=new(){High=ulong.MaxValue,Low=22,Sequence=102}};
        } else if(slot=="busy")*output=new(){Rejection=2};
        else {badPayload=true;return -1;}
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Result(void* context,Ticket* ticket,SaveResult* output,void* error)
    {
        ++results;if((nint)context!=0x1234)badPayload=true;
        *output=new(){Ticket=*ticket};
        if(ticket->Sequence==0)return 0;
        if(ticket->High!=ulong.MaxValue || ticket->Low!=22)badPayload=true;
        if(ticket->Sequence==7) {
            output->Kind=2;output->State=3;output->Requested=22;output->Committed=27;output->Generation=5;output->Recovered=1;output->High=1UL<<63;output->Low=33;output->Tick=12;
            Encoding.UTF8.GetBytes("restored",new Span<byte>(output->Diagnostic,256));
        }else if(ticket->Sequence==8) {
            output->Kind=1;output->State=2;output->Error=-32070;
            Encoding.UTF8.GetBytes("uncertain",new Span<byte>(output->Diagnostic,256));
        }else badPayload=true;
        return 0;
    }
    static bool Descriptor(ComponentType* d)
    {
        string hash=d->Id.High==0x1111111111111111UL
            ?"119b6ed352389e9c18c1d60f29176870ac92f7b3cdaf8f85433a2de16a9718d6"
            :"347a4b0f95256df320ce9a4b30ac6abc44d13a37178eda76b80693d114cb99d9";
        return d->Id.High==d->Id.Low && (d->Id.High==0x1111111111111111UL || d->Id.High==0x2222222222222222UL) &&
            d->Bytes==(d->Id.High==0x1111111111111111UL?48u:32u) && d->Reserved==0 &&
            $"{d->A:x16}{d->B:x16}{d->C:x16}{d->D:x16}"==hash;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Query(void* context,ComponentType* descriptor,Id* after,Id* ids,uint capacity,uint* written,void* error)
    {
        ++queries;
        if((nint)context!=0x1234 || !Descriptor(descriptor) || capacity!=2)badPayload=true;
        *written=0;
        if(after->High==0 && after->Low==0) { ids[0]=new(){High=11,Low=22};ids[1]=new(){High=11,Low=23};*written=2; }
        else if(after->High!=11 || after->Low!=23)badPayload=true;
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ComponentGet(void* context,ComponentType* descriptor,Id* id,void* output,uint bytes,uint* present,void* error)
    {
        ++componentGets;
        if((nint)context!=0x1234 || !Descriptor(descriptor) || id->High!=11 || bytes!=descriptor->Bytes)badPayload=true;
        *present=id->Low==24?0u:1u;if(*present==0)return 0;
        var wire=new Span<byte>(output,(int)bytes);wire.Clear();
        if(bytes==48) {
            BinaryPrimitives.WriteSingleLittleEndian(wire,12.5f);BinaryPrimitives.WriteInt32LittleEndian(wire[16..],42);
            BinaryPrimitives.WriteInt64LittleEndian(wire[32..],long.MinValue);
        } else {
            BinaryPrimitives.WriteUInt64LittleEndian(wire,ulong.MaxValue);BinaryPrimitives.WriteUInt64LittleEndian(wire[8..],1UL<<63);
            BinaryPrimitives.WriteDoubleLittleEndian(wire[16..],.125);
        }
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ComponentSet(void* context,ComponentType* descriptor,Id* id,void* input,uint bytes,void* error)
    {
        ++componentSets;
        if(id->High==ulong.MaxValue && id->Low==1001) {
            if((nint)context!=0x1234 || !Descriptor(descriptor) || bytes!=48)badPayload=true;
            var value=new ReadOnlySpan<byte>(input,(int)bytes);
            if(BinaryPrimitives.ReadSingleLittleEndian(value)!=8 || BinaryPrimitives.ReadInt32LittleEndian(value[16..])!=100 || BinaryPrimitives.ReadInt64LittleEndian(value[32..])!=long.MinValue)badPayload=true;
            return 0;
        }
        if((nint)context!=0x1234 || !Descriptor(descriptor) || id->High!=11 || id->Low!=22 || bytes!=descriptor->Bytes)badPayload=true;
        Span<byte> expected=stackalloc byte[(int)bytes];expected.Clear();
        if(bytes==48) { BinaryPrimitives.WriteInt32LittleEndian(expected[16..],-9);BinaryPrimitives.WriteInt64LittleEndian(expected[32..],long.MaxValue); }
        else { BinaryPrimitives.WriteUInt64LittleEndian(expected,ulong.MaxValue);BinaryPrimitives.WriteUInt64LittleEndian(expected[8..],1UL<<63); }
        if(!expected.SequenceEqual(new ReadOnlySpan<byte>(input,(int)bytes)))badPayload=true;return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Alive(void* context,Id* id,uint* alive,void* error)
    {
        ++aliveCalls;if((nint)context!=0x1234)badPayload=true;*alive=id->High==11 && id->Low==22?1u:0u;return 0;
    }
    static int LifecycleError(void* error)
    { var text=new Span<byte>(error,2048);text.Clear();Encoding.UTF8.GetBytes("lifecycle rejected",text);return -1; }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Spawn(void* context,RecipeId* template,Transform* transform,Id* result,void* error)
    {
        ++spawns;if((nint)context!=0x1234 || template->High!=0xfedcba9876543210)badPayload=true;
        if(template->Low==999)return LifecycleError(error);
        if(template->Low==26) {*result=default;return 0;}
        if(template->Low!=22)badPayload=true;
        if(transform!=null && (transform->Position[0]!=1.25 || transform->Position[1]!=-2.5 || transform->Position[2]!=3.75 ||
            transform->Rotation[0]!=0 || transform->Rotation[1]!=0 || transform->Rotation[2]!=0 || transform->Rotation[3]!=1 ||
            transform->Scale[0]!=2 || transform->Scale[1]!=3 || transform->Scale[2]!=4))badPayload=true;
        *result=new(){High=ulong.MaxValue,Low=transform==null?1001u:1002u};return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int Despawn(void* context,Id* id,void* error)
    {
        ++despawns;if((nint)context!=0x1234 || id->High!=ulong.MaxValue)badPayload=true;
        if(id->Low==999)return LifecycleError(error);
        if(id->Low!=1001 && id->Low!=1002)badPayload=true;return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int TemplateGet(void* context,ComponentType* descriptor,RecipeId* template,void* output,uint bytes,uint* present,void* error)
    {
        ++templateGets;if((nint)context!=0x1234 || !Descriptor(descriptor) || bytes!=48 || template->High!=0xfedcba9876543210)badPayload=true;
        if(template->Low==999)return LifecycleError(error);
        *present=template->Low==24?0u:template->Low==25?2u:1u;if(*present!=1)return 0;
        if(template->Low!=22)badPayload=true;
        var wire=new Span<byte>(output,(int)bytes);wire.Clear();BinaryPrimitives.WriteSingleLittleEndian(wire,99);
        BinaryPrimitives.WriteInt32LittleEndian(wire[16..],100);BinaryPrimitives.WriteInt64LittleEndian(wire[32..],long.MinValue);return 0;
    }
    static int uiReads,uiWrites,controlReads,resumeCalls,pauseCalls;
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int UiGet(void* context,Id* id,UiState* result,byte* text,uint capacity,uint* written,void* error)
    {
        ++uiReads;if((nint)context!=0x1234 || id->High!=1 || id->Low!=2 || capacity!=16384)badPayload=true;
        *result=new(){Revision=9,Kind=1,Visible=1,Enabled=1,EffectiveVisible=1,EffectiveEnabled=1,TextBytes=3};*written=3;text[0]=(byte)'O';text[1]=(byte)'l';text[2]=(byte)'d';return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int UiEdit(void* context,UiPatch* patch,uint count,UiModal* modal,void* error)
    {
        ++uiWrites;if((nint)context!=0x1234)badPayload=true;
        if(count==1) {if(patch==null || modal!=null || patch->Id.High!=1 || patch->Id.Low!=2 || patch->Mask!=7 || patch->Visible!=1 || patch->Enabled!=0 || Encoding.UTF8.GetString(new ReadOnlySpan<byte>(patch->Text,(int)patch->TextBytes))!="New é")badPayload=true;}
        else if(count!=0 || patch!=null || modal==null || modal->Change!=1 || modal->Reserved!=0 || modal->Id.High!=0 || modal->Id.Low!=0)badPayload=true;
        return 0;
    }
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ControlInfo(void* context,UiEvent* e,void* error)
    {++controlReads;*e=new(){Element=new(){High=1,Low=3},Sequence=42,ActionBytes=4};e->Action[0]=(byte)'s';e->Action[1]=(byte)'a';e->Action[2]=(byte)'v';e->Action[3]=(byte)'e';return 0;}
    [UnmanagedCallersOnly(CallConvs=[typeof(CallConvCdecl)])]
    static int ControlRequest(void* context,uint intent,void* error)
    {if(intent==1)++resumeCalls;else if(intent==2)++pauseCalls;else badPayload=true;return 0;}
    static void Check(bool condition,string message) { if(!condition)throw new Exception(message); }
    static string Output(byte* output) => Marshal.PtrToStringUTF8((nint)output)!;
    static void Reject(Call* call,List<string> checks,string name,Services candidate,bool nullServices=false)
    {
        call->Services=nullServices ? null:&candidate;
        int before=((ProbeState*)call->State)->Count;
        Check(Entry.Invoke((nint)call,sizeof(Call))!=0,$"{name} unexpectedly accepted.");
        Check(Output(call->Output).Contains("Gameplay service"),$"{name}: wrong error {Output(call->Output)}");
        Check(((ProbeState*)call->State)->Count==before && gets==0 && sets==0 && infos==0 && requests==0 && results==0 && queries==0 && componentGets==0 && componentSets==0 && aliveCalls==0 && spawns==0 && despawns==0 && templateGets==0,$"{name} invoked gameplay or callbacks.");
        checks.Add(name);
    }
    static int InvokePrefix(Call* call,bool native,int bytes=80)
    {
        if(!native)return Entry.Invoke((nint)call,bytes);
        delegate* unmanaged[Cdecl]<void*,int,int> invoke=&Poima.NativeGame.Entry.Invoke;
        return invoke(call,bytes);
    }
    static int UiCallbacks()=>uiReads+uiWrites+controlReads+resumeCalls+pauseCalls;
    static void PrefixContract(Call* call,Services good,List<string> checks,bool native)
    {
        var mode=native ? "linked production NativeGame entry" : "CoreCLR bridge";
        using var guarded=new GuardedHeader();
        byte* extended=stackalloc byte[256];new Span<byte>(extended,256).Fill(0xa5);
        *(Services*)extended=good;
        foreach(uint operation in new uint[]{3,6}) {
            call->Operation=operation;call->InputCount=0;call->Inputs=null;
            foreach(var header in new (uint Epoch,uint Bytes)[]{(7,0),(7,8),(7,175),(6,176),(8,4096)}) {
                guarded.Header[0]=header.Epoch;guarded.Header[1]=header.Bytes;call->Services=(Services*)guarded.Header;
                int before=((ProbeState*)call->State)->Count,callbacks=UiCallbacks();
                Check(InvokePrefix(call,native)!=0 && Output(call->Output).Contains("Gameplay service ABI"),$"{mode}: guarded {header} accepted on {operation}.");
                Check(((ProbeState*)call->State)->Count==before && UiCallbacks()==callbacks,$"{mode}: rejected header invoked game.");
            }
            call->Services=null;Check(InvokePrefix(call,native)!=0,$"{mode}: null services accepted.");
            // Every required slot remains mandatory even when an extension exists.
            for(int slot=0;slot<20;++slot) {
                *(Services*)extended=good;((Services*)extended)->Bytes=256;((nint*)(extended+16))[slot]=0;call->Services=(Services*)extended;
                int before=((ProbeState*)call->State)->Count,callbacks=UiCallbacks();
                Check(InvokePrefix(call,native)!=0 && Output(call->Output).Contains("callback is absent"),$"{mode}: missing slot {slot} accepted.");
                Check(((ProbeState*)call->State)->Count==before && UiCallbacks()==callbacks,$"{mode}: missing callback ran gameplay.");
            }
            *(Services*)extended=good;call->Services=(Services*)extended;
            foreach(var invalid in new (uint Count,nint Inputs)[]{(33,0),(1,0)}) {
                call->InputCount=invalid.Count;call->Inputs=(void*)invalid.Inputs;
                int before=((ProbeState*)call->State)->Count,callbacks=UiCallbacks();
                Check(InvokePrefix(call,native)!=0,$"{mode}: invalid input accepted.");
                Check(((ProbeState*)call->State)->Count==before && UiCallbacks()==callbacks,$"{mode}: invalid input ran gameplay.");
            }
            call->InputCount=0;call->Inputs=null;
            if(operation==6) {
                call->Inputs=(void*)1;
                Check(InvokePrefix(call,native)!=0 && Output(call->Output).Contains("cannot carry physics input"),$"{mode}: Control accepted input pointer.");
                call->Inputs=null;
            }
            foreach(uint size in new uint[]{176,177,192,256}) {
                ((Services*)extended)->Bytes=size;int before=((ProbeState*)call->State)->Count,callbacks=UiCallbacks();
                Check(InvokePrefix(call,native)==0,$"{mode}: prefix {size} operation {operation}: {Output(call->Output)}");
                Check(((ProbeState*)call->State)->Count==before+(native || operation==6 ? 1:0) && UiCallbacks()>callbacks,$"{mode}: extended prefix did not execute correct gameplay.");
                Check(new ReadOnlySpan<byte>(extended+176,80).IndexOfAnyExcept((byte)0xa5)<0,$"{mode}: unknown extension was modified.");
            }
        }
        call->Services=&good;call->Operation=3;
        call->Version=2;Check(InvokePrefix(call,native)!=0,$"{mode}: wrong call epoch accepted.");call->Version=1;
        Check(InvokePrefix(call,native,79)!=0 && InvokePrefix(call,native,81)!=0,$"{mode}: non-80-byte call accepted.");
        call->Services=null;
        checks.Add($"{mode}: Tick and Control accept 176/177/192/256-byte epoch-7 prefixes with poisoned opaque tails; guarded short headers, null/missing callbacks, wrong epochs and invalid input/call layouts rejected");
    }
    static void NativePrefixContract(byte* output,byte* state,Services good,List<string> checks)
    {
        var request=Encoding.UTF8.GetBytes("{\"type\":\"PrefixProbe\"}\0");
        Call call=new(){Version=1,Operation=1,Output=output,OutputCapacity=65536,State=state,StateBytes=4,Tick=123};
        fixed(byte* text=request){call.Text=text;Check(InvokePrefix(&call,true)==0,Output(output));}
        using(var manifest=JsonDocument.Parse(Output(output)))call.Handle=manifest.RootElement.GetProperty("handle").GetUInt64();
        call.Operation=2;Check(InvokePrefix(&call,true)==0,Output(output));
        PrefixContract(&call,good,checks,true);
        call.Operation=4;Check(InvokePrefix(&call,true)==0,Output(output));
    }
    static int Main(string[] args)
    {
        var checks=new List<string>();
        try
        {
            Check(sizeof(Call)==80 && sizeof(Services)==176 && sizeof(ComponentType)==56 && sizeof(Command)==48 && sizeof(Transition)==56 && sizeof(Animation)==120 &&
                sizeof(Ticket)==24 && sizeof(SaveRequest)==32 && sizeof(SaveEnqueue)==32 && sizeof(SaveResult)==344 && sizeof(SaveInfo)==104 && sizeof(RecipeId)==16 && sizeof(Transform)==80,"Independent ABI sizes.");
            Check(Marshal.OffsetOf<Services>(nameof(Services.Spawn)).ToInt64()==120 && Marshal.OffsetOf<Services>(nameof(Services.Despawn)).ToInt64()==128 &&
                Marshal.OffsetOf<Services>(nameof(Services.TemplateGet)).ToInt64()==136 && Marshal.OffsetOf<Transform>(nameof(Transform.Rotation)).ToInt64()==24 &&
                Marshal.OffsetOf<Transform>(nameof(Transform.Scale)).ToInt64()==56,"Independent lifecycle offsets.");
            Check(sizeof(UiState)==40 && sizeof(UiPatch)==40 && sizeof(UiModal)==24 && sizeof(UiEvent)==160 &&
                Marshal.OffsetOf<Services>(nameof(Services.UiGet)).ToInt64()==144 && Marshal.OffsetOf<Services>(nameof(Services.UiEdit)).ToInt64()==152 &&
                Marshal.OffsetOf<Services>(nameof(Services.ControlInfo)).ToInt64()==160 && Marshal.OffsetOf<Services>(nameof(Services.ControlRequest)).ToInt64()==168 &&
                Marshal.OffsetOf<UiState>(nameof(UiState.TextBytes)).ToInt64()==32 && Marshal.OffsetOf<UiPatch>(nameof(UiPatch.Mask)).ToInt64()==28 &&
                Marshal.OffsetOf<UiEvent>(nameof(UiEvent.Action)).ToInt64()==32,"Independent UI ABI offsets/sizes.");
            Check(TemplateId.Parse("fedcba98765432100000000000000016")==new TemplateId(0xfedcba9876543210,22),"Template ID parsing.");
            foreach(var invalid in new[]{new string('0',32),new string('A',32),"template"}) {
                bool failed=false;try {TemplateId.Parse(invalid);}catch(ArgumentException){failed=true;}Check(failed,"Invalid template identity accepted.");
            }
            byte* output=stackalloc byte[65536]; byte* state=stackalloc byte[sizeof(ProbeState)];
            var request=Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new {assembly=Assembly.GetExecutingAssembly().Location,type=typeof(ProbeGame).FullName})+"\0");
            Call call=new(){Version=1,Operation=1,Output=output,OutputCapacity=65536};
            fixed(byte* text=request)
            {
                call.Text=text; Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            }
            using var manifest=JsonDocument.Parse(Output(output)); call.Handle=manifest.RootElement.GetProperty("handle").GetUInt64();
            call.State=state;call.StateBytes=(uint)sizeof(ProbeState);call.Operation=2;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            Check(((ProbeState*)state)->Count==7,"Initialize state.");
            nint unused=(nint)(delegate* unmanaged[Cdecl]<int>)&Unused;
            Services good=new(){Version=7,Bytes=176,Context=(void*)0x1234,Entity=unused,Raycast=unused,Move=unused,Sound=unused,Get=&Get,Set=&Set,SaveInfo=&Info,SaveRequest=&Request,SaveResult=&Result,Query=&Query,ComponentGet=&ComponentGet,ComponentSet=&ComponentSet,Alive=&Alive,Spawn=&Spawn,Despawn=&Despawn,TemplateGet=&TemplateGet,UiGet=&UiGet,UiEdit=&UiEdit,ControlInfo=&ControlInfo,ControlRequest=&ControlRequest};
            call.Operation=3;call.Tick=123;
            Reject(&call,checks,"null services rejected before Tick",good,true);
            foreach(var pair in new (uint Version,uint Bytes)[]{(2,48),(3,64),(3,88),(4,64),(4,87),(4,88),(4,120),(5,88),(5,119),(5,120),(5,121),(6,120),(6,136),(6,143),(6,144),(6,145),(7,144),(7,175),(8,176),(6,0)})
            { var candidate=good;candidate.Version=pair.Version;candidate.Bytes=pair.Bytes;Reject(&call,checks,$"services {pair.Version}/{pair.Bytes} rejected before Tick",candidate); }
            var missing=good;missing.Get=null;Reject(&call,checks,"null animation get rejected",missing);
            missing=good;missing.Set=null;Reject(&call,checks,"null animation set rejected",missing);
            missing=good;missing.Sound=0;Reject(&call,checks,"null prefix callback rejected",missing);
            missing=good;missing.Entity=0;Reject(&call,checks,"null entity callback rejected",missing);
            missing=good;missing.Raycast=0;Reject(&call,checks,"null ray callback rejected",missing);
            missing=good;missing.Move=0;Reject(&call,checks,"null motion callback rejected",missing);
            missing=good;missing.SaveInfo=null;Reject(&call,checks,"null save info rejected",missing);
            missing=good;missing.SaveRequest=null;Reject(&call,checks,"null save request rejected",missing);
            missing=good;missing.SaveResult=null;Reject(&call,checks,"null save result rejected",missing);
            missing=good;missing.Query=null;Reject(&call,checks,"null component query rejected",missing);
            missing=good;missing.ComponentGet=null;Reject(&call,checks,"null component get rejected",missing);
            missing=good;missing.ComponentSet=null;Reject(&call,checks,"null component set rejected",missing);
            missing=good;missing.Alive=null;Reject(&call,checks,"null entity alive rejected",missing);
            missing=good;missing.Spawn=null;Reject(&call,checks,"null spawn rejected",missing);
            missing=good;missing.Despawn=null;Reject(&call,checks,"null despawn rejected",missing);
            missing=good;missing.TemplateGet=null;Reject(&call,checks,"null template component get rejected",missing);
            missing=good;missing.UiGet=null;Reject(&call,checks,"null UI read rejected",missing);
            missing=good;missing.UiEdit=null;Reject(&call,checks,"null UI edit rejected",missing);
            missing=good;missing.ControlInfo=null;Reject(&call,checks,"null control info rejected",missing);
            missing=good;missing.ControlRequest=null;Reject(&call,checks,"null control request rejected",missing);
            call.Services=&good;
            Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            Check(((ProbeState*)state)->Count==8 && gets==3 && sets==1 && infos==1 && requests==3 && results==3 && queries==2 && componentGets==3 && componentSets==3 && aliveCalls==3 && spawns==4 && despawns==3 && templateGets==5 && !badPayload,"Successful v7 invocation payload/state mismatch.");
            checks.Add("matching v7 transfers animation, saves and generated components: all five scalar kinds, sorted cursor, missing presence, exact fingerprint, zero padding, positive zero and finite validation");
            checks.Add("lifecycle callbacks transfer typed template IDs, optional complete transforms, reserved IDs, cancellation, frozen component defaults, presence and native errors");
            call.Operation=6;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("no UI control handler") && ((ProbeState*)state)->Count==8,"Tick-only game silently accepted control.");
            controlReads=0;
            call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            for(int iteration=0;iteration<100;++iteration)
            {
                call.Operation=1;fixed(byte* text=request) { call.Text=text;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output)); }
                using var next=JsonDocument.Parse(Output(output));call.Handle=next.RootElement.GetProperty("handle").GetUInt64();
                call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
                call.Operation=3;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
                call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            }
            checks.Add("100 additional generated-code module lifetimes invoke and retire without retained Type caches");
            var uiRequest=Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new {assembly=Assembly.GetExecutingAssembly().Location,type=typeof(UiProbeGame).FullName})+"\0");
            call.Operation=1;fixed(byte* text=uiRequest){call.Text=text;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));}
            using(var uiManifest=JsonDocument.Parse(Output(output)))call.Handle=uiManifest.RootElement.GetProperty("handle").GetUInt64();
            call.Operation=2;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            call.Operation=6;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            Check(((ProbeState*)state)->Count==11 && uiReads==1 && uiWrites==2 && controlReads==1 && resumeCalls==1 && pauseCalls==1 && !badPayload,"UI control ABI payload/state mismatch.");
            call.Operation=3;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0 && uiWrites==3,"Tick UI SDK binding failed.");
            PrefixContract(&call,good,checks,false);
            call.Operation=4;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            checks.Add("op6 transfers UI event/read/patch/modal/Resume/Pause without invoking Tick; Tick can write UI too");
            var unsupported=Encoding.UTF8.GetBytes(JsonSerializer.Serialize(new {assembly=Assembly.GetExecutingAssembly().Location,type=typeof(UnsupportedTemplateGame).FullName})+"\0");
            call.Operation=1;fixed(byte* text=unsupported) {
                call.Text=text;Check(Entry.Invoke((nint)(&call),sizeof(Call))!=0 && Output(output).Contains("Unsupported state field"),"TemplateId was silently accepted as persisted EntityId state.");
            }
            checks.Add("TemplateId remains distinct from persisted EntityId state fields");
            call.Operation=5;Check(Entry.Invoke((nint)(&call),sizeof(Call))==0,Output(output));
            using var collected=JsonDocument.Parse(Output(output));
            Check(collected.RootElement.GetProperty("active_modules").GetInt32()==0 && collected.RootElement.GetProperty("retired_alive").GetInt32()==0,"Fixture context retained after release.");
            checks.Add("generated component and animation fixture collectible context released");
            NativePrefixContract(output,state,good,checks);
            AnimationExtensionContract(output,state,good,checks);
            AnimationLayerContract(output,state,good,checks);
            CharacterInputContract(output,state,good,checks);
            NavigationContract(output,state,good,checks);
            var evidence=JsonSerializer.Serialize(new {passed=true,checks,platform=RuntimeInformation.OSDescription,
                bridge_sha256=Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(typeof(Entry).Assembly.Location))),
                sdk_sha256=Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(typeof(Game<>).Assembly.Location)))},new JsonSerializerOptions{WriteIndented=true});
            if(args.Length==1)File.WriteAllText(args[0],evidence+"\n");Console.WriteLine(evidence);return 0;
        }
        catch(Exception error) { Console.Error.WriteLine(error);return 1; }
    }
}
