// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
[assembly: InternalsVisibleTo("Poima.ManagedBridge")]
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
}
// Authoritative mutable state belongs in TState. Game instances must contain
// no instance fields. The runtime stages initialization/reload and rolls back
// native state plus queued physics commands when an update fails.
public abstract class Game<TState> : IGame where TState : unmanaged
{
    public abstract void Initialize(ref TState state);
    public abstract void Tick(ref TState state, GameContext context);
    Type IGame.StateType => typeof(TState);
    int IGame.StateBytes => Unsafe.SizeOf<TState>();
    void IGame.Initialize(Span<byte> state) => Initialize(ref MemoryMarshal.AsRef<TState>(state));
    void IGame.Tick(Span<byte> state, GameContext context) => Tick(ref MemoryMarshal.AsRef<TState>(state),context);
}

[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeRay { public Vector3d Origin,Direction;public double Distance;public EntityId* Ignore;public uint IgnoreCount,Reserved; }
[StructLayout(LayoutKind.Sequential)] internal struct NativeHit { public EntityId Entity; public double Fraction,Distance;public Vector3d Position,Normal;public uint Hit,NormalValid; }
[StructLayout(LayoutKind.Sequential)] internal struct NativeMotion { public EntityId Entity;public Vector3d Position;public double X,Y,Z,W;public uint Ticks,Reserved; }
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeError { public fixed byte Text[2048]; }
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeServices
{
    public uint Version,Bytes;public void* Context;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,EntitySnapshot*,NativeError*,int> Entity;
    public delegate* unmanaged[Cdecl]<void*,NativeRay*,NativeHit*,NativeError*,int> Raycast;
    public delegate* unmanaged[Cdecl]<void*,NativeMotion*,NativeError*,int> Move;
}
public readonly unsafe ref struct GameContext
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
    public EntitySnapshot Get(EntityId entity)
    { EntitySnapshot result=default;NativeError error=default;Check(services->Entity(services->Context,&entity,&result,&error),&error);return result; }
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
    public void MoveKinematic(EntityId entity,Vector3d position,System.Numerics.Quaternion rotation,uint ticks)
    {
        NativeMotion target=new(){Entity=entity,Position=position,X=rotation.X,Y=rotation.Y,Z=rotation.Z,W=rotation.W,Ticks=ticks};NativeError error=default;
        Check(services->Move(services->Context,&target,&error),&error);
    }
}
