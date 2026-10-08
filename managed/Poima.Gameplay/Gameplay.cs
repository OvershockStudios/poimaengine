// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
[assembly: InternalsVisibleTo("Poima.ManagedBridge")]
[assembly: InternalsVisibleTo("Poima.NativeGame")]
[assembly: InternalsVisibleTo("Poima.NativeGame.Generator")]
namespace Poima;

[StructLayout(LayoutKind.Sequential)]
public readonly record struct EntityId(ulong High, ulong Low)
{
    public static EntityId Parse(string value)
    {
        if (value.Length != 32) throw new ArgumentException("Entity IDs contain 32 hex digits.");
        return new(ulong.Parse(value.AsSpan(0,16),NumberStyles.HexNumber,CultureInfo.InvariantCulture),ulong.Parse(value.AsSpan(16),NumberStyles.HexNumber,CultureInfo.InvariantCulture));
    }
    public override string ToString() => $"{High:x16}{Low:x16}";
}
[StructLayout(LayoutKind.Sequential)]
public readonly record struct Vector3d(double X, double Y, double Z);
[InlineArray(16)] internal struct MatrixStorage { private double element; }
[StructLayout(LayoutKind.Sequential)]
public struct WorldTransform
{
    private MatrixStorage matrix;
    public readonly Vector3d Position => new(matrix[12],matrix[13],matrix[14]);
    public readonly Vector3d Forward => new(-matrix[8],-matrix[9],-matrix[10]);
    public readonly double this[int column, int row] => column is >=0 and <4 && row is >=0 and <4 ? matrix[column*4+row] : throw new ArgumentOutOfRangeException();
}
public enum BodyMotion : uint { None, Static, Dynamic, Kinematic, Character }
[StructLayout(LayoutKind.Sequential)]
public struct EntitySnapshot { public WorldTransform Transform; public Vector3d Velocity; public BodyMotion Motion; public uint MotionRemainingTicks; }
public readonly record struct RayHit(EntityId Entity, double Distance, Vector3d Position, Vector3d? Normal);
public readonly record struct AnimationTransition(ulong StartTick,uint DurationTicks,uint ElapsedTicks,double Weight,
    bool SourceFrozen,int? SourceClip,double? SourceTime,double? SourceSpeed,bool? SourceLoop,bool? SourcePlaying);
public readonly record struct AnimationState(EntityId Entity,int? Clip,double Time,double Speed,bool Loop,bool Playing,
    double Duration,AnimationTransition? Transition);
// Epoch words are opaque signed bit patterns, allowing a ticket to be stored
// explicitly in three existing long state fields. SaveTicket itself is not a
// registered nested state-field type and is never an EntityId.
[StructLayout(LayoutKind.Sequential)]
public readonly record struct SaveTicket(long EpochHigh,long EpochLow,long Sequence)
{
    public bool IsValid => (EpochHigh!=0 || EpochLow!=0) && Sequence is >0 and <=9007199254740991L;
}
public readonly record struct SaveEpoch(long High,long Low);
public enum SaveKind : uint { None=0,Save=1,Load=2 }
public enum SaveOperationState : uint { Expired=0,Queued=1,Resolving=2,Succeeded=3,Failed=4 }
public enum SaveRequestRejection : uint { None=0,Disabled=1,Busy=2,Invalid=3,Exhausted=4 }
public readonly record struct SaveRequestResult(SaveTicket Ticket,SaveRequestRejection Rejection)
{ public bool Accepted => Rejection==SaveRequestRejection.None; }
public sealed class SaveRequestException(SaveRequestRejection rejection) : InvalidOperationException($"Gameplay save request rejected: {rejection}.")
{ public SaveRequestRejection Rejection { get; }=rejection; }
public readonly record struct SaveRestoreInfo(SaveTicket? InitiatingTicket,SaveEpoch DestinationEpoch,
    ulong CommittedSourceTick,ulong RestoredTick,ulong Generation,bool Recovered);
public readonly record struct SaveCapabilities(bool Enabled,SaveEpoch Epoch,ulong ConfigurationGeneration,SaveRestoreInfo? LastRestore);
public readonly record struct SaveOperationResult(SaveTicket Ticket,SaveKind Kind,SaveOperationState State,
    ulong RequestedTick,ulong CommittedTick,ulong Generation,bool Recovered,int ErrorCode,string Diagnostic,
    SaveEpoch? RestoredEpoch,ulong? RestoredTick)
{ public bool IsTerminal => State is SaveOperationState.Succeeded or SaveOperationState.Failed or SaveOperationState.Expired; }
public enum GameAction : uint { Jump=1, Use=2 }
[StructLayout(LayoutKind.Sequential)]
public struct GameInput
{
    public EntityId Entity; public float MoveRight,MoveForward,LookYaw,LookPitch; internal uint Buttons,Reserved;
    public readonly bool Pressed(GameAction action) => (Buttons & (uint)action)!=0;
}
[AttributeUsage(AttributeTargets.Class,Inherited=false)]
public sealed class GameModuleAttribute(string identity) : Attribute { public string Identity { get; }=identity; }

internal interface IGame
{
    Type StateType { get; }
    int StateBytes { get; }
    void Initialize(Span<byte> state);
    void Tick(Span<byte> state, GameContext context);
    void Control(Span<byte> state, ControlContext context);
}
// Authoritative mutable state belongs in TState. Game instances must contain
// no instance fields. The runtime stages initialization/reload and rolls back
// native state plus queued physics commands when an update fails.
public abstract class Game<TState> : IGame where TState : unmanaged
{
    public abstract void Initialize(ref TState state);
    public abstract void Tick(ref TState state, GameContext context);
    public virtual void Control(ref TState state, ControlContext context) => throw new NotSupportedException("This game module has no UI control handler.");
    Type IGame.StateType => typeof(TState);
    int IGame.StateBytes => Unsafe.SizeOf<TState>();
    void IGame.Initialize(Span<byte> state) => Initialize(ref MemoryMarshal.AsRef<TState>(state));
    void IGame.Tick(Span<byte> state, GameContext context) => Tick(ref MemoryMarshal.AsRef<TState>(state),context);
    void IGame.Control(Span<byte> state, ControlContext context) => Control(ref MemoryMarshal.AsRef<TState>(state),context);
}

[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeRay { public Vector3d Origin,Direction;public double Distance;public EntityId* Ignore;public uint IgnoreCount,Reserved; }
[StructLayout(LayoutKind.Sequential)] internal struct NativeHit { public EntityId Entity; public double Fraction,Distance;public Vector3d Position,Normal;public uint Hit,NormalValid; }
[StructLayout(LayoutKind.Sequential)] internal struct NativeMotion { public EntityId Entity;public Vector3d Position;public double X,Y,Z,W;public uint Ticks,Reserved; }
[StructLayout(LayoutKind.Sequential)] internal struct NativeSound { public EntityId Emitter;public ulong Voice;public float Gain;public uint Stop; }
[StructLayout(LayoutKind.Sequential)] internal struct NativeAnimationCommand
{ public EntityId Entity;public double Time,Speed;public int Clip;public uint Loop,Playing,BlendTicks; }
[StructLayout(LayoutKind.Sequential)] internal struct NativeAnimationTransition
{
    public ulong StartTick;public double Weight,SourceTime,SourceSpeed;public uint DurationTicks,ElapsedTicks;
    public int SourceClip;public uint SourceFrozen,SourceLoop,SourcePlaying;
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeAnimationState
{
    public EntityId Entity;public double Time,Speed,Duration;public int Clip;
    public uint Present,Loop,Playing,TransitionPresent,Reserved;public NativeAnimationTransition Transition;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeError { public fixed byte Text[2048]; }
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeSaveRequest
{ public uint Kind,SlotBytes;public byte* Slot;public ulong ExpectedGeneration;public uint HasExpectedGeneration,AllowRecovery; }
[StructLayout(LayoutKind.Sequential)] internal struct NativeSaveEnqueue
{ public SaveTicket Ticket;public uint Rejection,Reserved; }
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeSaveResult
{
    public SaveTicket Ticket;public uint Kind,State;public ulong RequestedTick,CommittedTick,Generation;
    public uint Recovered;public int ErrorCode;public ulong RestoredHigh,RestoredLow,RestoredTick;public fixed byte Diagnostic[256];
}
[StructLayout(LayoutKind.Sequential)] internal struct NativeSaveInfo
{
    public ulong High,Low,ConfigurationGeneration;public uint Enabled,RestorePresent;public SaveTicket InitiatingTicket;
    public ulong DestinationHigh,DestinationLow,CommittedSourceTick,RestoredTick,Generation;public uint Recovered,Reserved;
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeServices
{
    public uint Version,Bytes;public void* Context;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,EntitySnapshot*,NativeError*,int> Entity;
    public delegate* unmanaged[Cdecl]<void*,NativeRay*,NativeHit*,NativeError*,int> Raycast;
    public delegate* unmanaged[Cdecl]<void*,NativeMotion*,NativeError*,int> Move;
    public delegate* unmanaged[Cdecl]<void*,NativeSound*,ulong*,NativeError*,int> Sound;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,NativeAnimationState*,NativeError*,int> AnimationGet;
    public delegate* unmanaged[Cdecl]<void*,NativeAnimationCommand*,NativeError*,int> AnimationSet;
    public delegate* unmanaged[Cdecl]<void*,NativeSaveInfo*,NativeError*,int> SaveInfo;
    public delegate* unmanaged[Cdecl]<void*,NativeSaveRequest*,NativeSaveEnqueue*,NativeError*,int> SaveRequest;
    public delegate* unmanaged[Cdecl]<void*,SaveTicket*,NativeSaveResult*,NativeError*,int> SaveResult;
    public delegate* unmanaged[Cdecl]<void*,GameplayComponentDescriptor*,EntityId*,EntityId*,uint,uint*,NativeError*,int> ComponentQuery;
    public delegate* unmanaged[Cdecl]<void*,GameplayComponentDescriptor*,EntityId*,void*,uint,uint*,NativeError*,int> ComponentGet;
    public delegate* unmanaged[Cdecl]<void*,GameplayComponentDescriptor*,EntityId*,void*,uint,NativeError*,int> ComponentSet;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,uint*,NativeError*,int> EntityAlive;
    public delegate* unmanaged[Cdecl]<void*,TemplateId*,NativeTransform*,EntityId*,NativeError*,int> Spawn;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,NativeError*,int> Despawn;
    public delegate* unmanaged[Cdecl]<void*,GameplayComponentDescriptor*,TemplateId*,void*,uint,uint*,NativeError*,int> TemplateComponentGet;
    public delegate* unmanaged[Cdecl]<void*,UiId*,NativeUiState*,byte*,uint,uint*,NativeError*,int> UiGet;
    public delegate* unmanaged[Cdecl]<void*,NativeUiPatch*,uint,NativeUiModalEdit*,NativeError*,int> UiEdit;
    public delegate* unmanaged[Cdecl]<void*,NativeUiControlEvent*,NativeError*,int> ControlInfo;
    public delegate* unmanaged[Cdecl]<void*,uint,NativeError*,int> ControlRequest;
}
internal static unsafe class SaveAbiLayout
{
    // Address differences also work in NativeAOT without reflection metadata.
    internal static bool Valid()
    {
        NativeServices s=default;NativeSaveRequest r=default;NativeSaveEnqueue e=default;NativeSaveResult o=default;NativeSaveInfo i=default;
        return sizeof(SaveTicket)==24 && sizeof(NativeSaveRequest)==32 && sizeof(NativeSaveEnqueue)==32 && sizeof(NativeSaveResult)==344 && sizeof(NativeSaveInfo)==104 &&
            (byte*)&s.SaveInfo-(byte*)&s==64 && (byte*)&s.SaveRequest-(byte*)&s==72 && (byte*)&s.SaveResult-(byte*)&s==80 &&
            (byte*)&r.Slot-(byte*)&r==8 && (byte*)&r.ExpectedGeneration-(byte*)&r==16 && (byte*)&r.AllowRecovery-(byte*)&r==28 &&
            (byte*)&e.Rejection-(byte*)&e==24 && (byte*)&o.RequestedTick-(byte*)&o==32 && (byte*)&o.ErrorCode-(byte*)&o==60 && o.Diagnostic-(byte*)&o==88 &&
            (byte*)&i.InitiatingTicket-(byte*)&i==32 && (byte*)&i.DestinationHigh-(byte*)&i==56 && (byte*)&i.Recovered-(byte*)&i==96;
    }
}
public readonly unsafe ref partial struct GameContext
{
    private readonly NativeServices* services;
    private readonly ReadOnlySpan<GameInput> inputs;
    public ulong Tick { get; }
    public double DeltaTime => 1.0/60.0;
    public ReadOnlySpan<GameInput> Inputs => inputs;
    internal GameContext(NativeServices* services,GameInput* inputs,int count,ulong tick)
    { this.services=services;this.inputs=new(inputs,count);Tick=tick; }
    private static void Check(int code,NativeError* error)
    { if(code!=0)throw new InvalidOperationException(Marshal.PtrToStringUTF8((nint)error->Text) ?? "Native gameplay service failed."); }
    public SaveCapabilities Saves
    {
        get
        {
            NativeSaveInfo info=default;NativeError error=default;
            Check(services->SaveInfo(services->Context,&info,&error),&error);
            SaveRestoreInfo? restored=info.RestorePresent==0 ? null : new(
                info.InitiatingTicket==default ? null : info.InitiatingTicket,
                new(unchecked((long)info.DestinationHigh),unchecked((long)info.DestinationLow)),
                info.CommittedSourceTick,info.RestoredTick,info.Generation,info.Recovered!=0);
            return new(info.Enabled!=0,new(unchecked((long)info.High),unchecked((long)info.Low)),info.ConfigurationGeneration,restored);
        }
    }
    // Requests stage copied intents only. Storage is serviced after the entire
    // native batch commits; a later Tick failure rolls the request back too.
    // Resolving means publication is uncertain, not a failed write to repeat.
    public SaveRequestResult TryRequestSave(string slot,long? expectedGeneration=null) => TryRequest(SaveKind.Save,slot,expectedGeneration,false);
    public SaveRequestResult TryRequestLoad(string slot,long? expectedGeneration=null,bool allowRecovery=false) => TryRequest(SaveKind.Load,slot,expectedGeneration,allowRecovery);
    public SaveTicket RequestSave(string slot,long? expectedGeneration=null) => RequireAccepted(TryRequestSave(slot,expectedGeneration));
    public SaveTicket RequestLoad(string slot,long? expectedGeneration=null,bool allowRecovery=false) => RequireAccepted(TryRequestLoad(slot,expectedGeneration,allowRecovery));
    private static SaveTicket RequireAccepted(SaveRequestResult result) => result.Accepted ? result.Ticket : throw new SaveRequestException(result.Rejection);
    private SaveRequestResult TryRequest(SaveKind kind,string slot,long? expected,bool recovery)
    {
        if(slot is null || slot.Length is <1 or >64 || expected is <0 or >9007199254740991L)
            return new(default,SaveRequestRejection.Invalid);
        byte* bytes=stackalloc byte[64];
        for(int i=0;i<slot.Length;++i)
        {
            char c=slot[i];
            if(!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='-' || c=='_'))return new(default,SaveRequestRejection.Invalid);
            bytes[i]=(byte)c;
        }
        NativeSaveRequest request=new(){Kind=(uint)kind,SlotBytes=(uint)slot.Length,Slot=bytes,
            ExpectedGeneration=(ulong)(expected ?? 0),HasExpectedGeneration=expected.HasValue ? 1u : 0u,AllowRecovery=recovery ? 1u : 0u};
        NativeSaveEnqueue result=default;NativeError error=default;
        Check(services->SaveRequest(services->Context,&request,&result,&error),&error);
        return new(result.Ticket,(SaveRequestRejection)result.Rejection);
    }
    // Memory only; reads neither consume results nor touch save storage. Old
    // process/evicted tickets expire, allowing restored pending flags to clear.
    public SaveOperationResult GetSaveResult(SaveTicket ticket)
    {
        NativeSaveResult result=default;NativeError error=default;
        Check(services->SaveResult(services->Context,&ticket,&result,&error),&error);
        int length=0;while(length<256 && result.Diagnostic[length]!=0)++length;
        string diagnostic=System.Text.Encoding.UTF8.GetString(new ReadOnlySpan<byte>(result.Diagnostic,length));
        bool restored=result.State==(uint)SaveOperationState.Succeeded && result.Kind==(uint)SaveKind.Load;
        return new(result.Ticket,(SaveKind)result.Kind,(SaveOperationState)result.State,result.RequestedTick,result.CommittedTick,
            result.Generation,result.Recovered!=0,result.ErrorCode,diagnostic,
            restored ? new SaveEpoch(unchecked((long)result.RestoredHigh),unchecked((long)result.RestoredLow)) : null,
            restored ? result.RestoredTick : null);
    }
    public EntitySnapshot Get(EntityId entity)
    { EntitySnapshot result=default;NativeError error=default;Check(services->Entity(services->Context,&entity,&result,&error),&error);return result; }
    // Queries observe current native state, including explicit caller commands,
    // but not SetAnimation writes queued by this Tick callback.
    public AnimationState? GetAnimation(EntityId entity)
    {
        NativeAnimationState result=default;NativeError error=default;
        Check(services->AnimationGet(services->Context,&entity,&result,&error),&error);
        return DecodeAnimation(entity,result);
    }
    private static AnimationState? DecodeAnimation(EntityId entity,in NativeAnimationState result)
    {
        if(result.Present==0)return null;
        AnimationTransition? transition=null;
        if(result.TransitionPresent!=0)
        {
            var t=result.Transition;bool frozen=t.SourceFrozen!=0;
            transition=new(t.StartTick,t.DurationTicks,t.ElapsedTicks,t.Weight,frozen,
                !frozen && t.SourceClip>=0 ? t.SourceClip : null,frozen ? null : t.SourceTime,
                frozen ? null : t.SourceSpeed,frozen ? null : t.SourceLoop!=0,frozen ? null : t.SourcePlaying!=0);
        }
        return new(entity,result.Clip>=0 ? result.Clip : null,result.Time,result.Speed,result.Loop!=0,result.Playing!=0,result.Duration,transition);
    }
    // Full replacement, applied after Tick returns. Call on state changes rather
    // than every frame: repeating a command intentionally restarts its clock.
    public void SetAnimation(EntityId entity,int? clip,double time=0,double speed=1,bool loop=true,bool playing=true,uint blendTicks=0)
    {
        if(clip is <0)throw new ArgumentOutOfRangeException(nameof(clip),"Use null for the authored rest pose.");
        NativeAnimationCommand command=new(){Entity=entity,Clip=clip ?? -1,Time=time,Speed=speed,Loop=loop ? 1u : 0u,Playing=playing ? 1u : 0u,BlendTicks=blendTicks};
        NativeError error=default;Check(services->AnimationSet(services->Context,&command,&error),&error);
    }
    public bool Pressed(EntityId entity,GameAction action)
    { foreach(ref readonly var input in inputs)if(input.Entity==entity && input.Pressed(action))return true;return false; }
    public RayHit? Raycast(Vector3d origin,Vector3d direction,double distance,ReadOnlySpan<EntityId> ignore=default)
    {
        fixed(EntityId* ids=ignore)
        {
            NativeRay ray=new(){Origin=origin,Direction=direction,Distance=distance,Ignore=ids,IgnoreCount=(uint)ignore.Length};NativeHit hit=default;NativeError error=default;
            Check(services->Raycast(services->Context,&ray,&hit,&error),&error);
            return hit.Hit==0 ? null : new(hit.Entity,hit.Distance,hit.Position,hit.NormalValid!=0 ? hit.Normal : null);
        }
    }
    public long PlaySound(EntityId emitter,float gain=1)
    {
        NativeSound command=new(){Emitter=emitter,Gain=gain};ulong voice=0;NativeError error=default;
        Check(services->Sound(services->Context,&command,&voice,&error),&error);return checked((long)voice);
    }
    public void StopSound(long voice)
    {
        if(voice<=0)throw new ArgumentOutOfRangeException(nameof(voice));
        NativeSound command=new(){Voice=(ulong)voice,Stop=1};ulong result=0;NativeError error=default;
        Check(services->Sound(services->Context,&command,&result,&error),&error);
    }
    public void MoveKinematic(EntityId entity,Vector3d position,System.Numerics.Quaternion rotation,uint ticks)
    {
        NativeMotion target=new(){Entity=entity,Position=position,X=rotation.X,Y=rotation.Y,Z=rotation.Z,W=rotation.W,Ticks=ticks};NativeError error=default;
        Check(services->Move(services->Context,&target,&error),&error);
    }
}
