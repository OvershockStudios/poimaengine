// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
namespace Poima;

[AttributeUsage(AttributeTargets.Struct,Inherited=false)]
public sealed class GameplayComponentAttribute(string id) : Attribute
{ public string Id { get; }=id;public string? Name { get; set; } }
[AttributeUsage(AttributeTargets.Field,Inherited=false)]
public sealed class GameplayFieldAttribute(string id) : Attribute
{ public string Id { get; }=id;public string? Name { get; set; } public string? Default { get; set; } public string Unit { get; set; }=""; }
// A constant data blob extracted using PEReader; authoring never constructs it.
[AttributeUsage(AttributeTargets.Assembly,AllowMultiple=false)]
public sealed class GameplayComponentManifestAttribute(string json) : Attribute { public string Json { get; }=json; }

[StructLayout(LayoutKind.Sequential)]
public readonly struct GameplayComponentDescriptor(EntityId id,ulong a,ulong b,ulong c,ulong d,uint bytes)
{
    public readonly EntityId Id=id;
    public readonly ulong FingerprintA=a,FingerprintB=b,FingerprintC=c,FingerprintD=d;
    public readonly uint Bytes=bytes,Reserved=0;
}
// Generated in the game assembly. No Type registry or reflection on the Tick path.
public interface IGameplayComponent<T> where T:unmanaged,IGameplayComponent<T>
{
    static abstract GameplayComponentDescriptor Descriptor { get; }
    static abstract void Encode(in T value,Span<byte> bytes);
    static abstract T Decode(ReadOnlySpan<byte> bytes);
}
internal static unsafe class ComponentAbiLayout
{
    internal static bool Valid()
    {
        NativeServices s=default;GameplayComponentDescriptor d=default;
        return sizeof(GameplayComponentDescriptor)==56 && (byte*)&d.FingerprintA-(byte*)&d==16 && (byte*)&d.Bytes-(byte*)&d==48 &&
            (byte*)&s.ComponentQuery-(byte*)&s==88 && (byte*)&s.ComponentGet-(byte*)&s==96 &&
            (byte*)&s.ComponentSet-(byte*)&s==104 && (byte*)&s.EntityAlive-(byte*)&s==112;
    }
}
public readonly unsafe ref partial struct GameContext
{
    private static GameplayComponentDescriptor ComponentDescriptor<T>() where T:unmanaged,IGameplayComponent<T>
    {
        var descriptor=T.Descriptor;
        if(descriptor.Bytes is <16 or >512 || descriptor.Bytes%16!=0 || descriptor.Reserved!=0)
            throw new ArgumentException("Invalid generated component wire descriptor.");
        return descriptor;
    }
    /// <summary>Sorted persistent IDs after an exclusive cursor. No membership changes during this runtime.</summary>
    public int Query<T>(Span<EntityId> destination,EntityId after=default) where T:unmanaged,IGameplayComponent<T>
    {
        if(destination.Length is <1 or >256)throw new ArgumentOutOfRangeException(nameof(destination),"Query pages hold 1..256 IDs.");
        var descriptor=ComponentDescriptor<T>();uint written=0;NativeError error=default;
        fixed(EntityId* output=destination)
            Check(services->ComponentQuery(services->Context,&descriptor,&after,output,(uint)destination.Length,&written,&error),&error);
        if(written>destination.Length)throw new InvalidOperationException("Native component query exceeded its page.");
        return (int)written;
    }
    /// <summary>False only for an existing entity without this component. An unknown entity is an error.</summary>
    public bool TryGet<T>(EntityId entity,out T value) where T:unmanaged,IGameplayComponent<T>
    {
        var descriptor=ComponentDescriptor<T>();Span<byte> buffer=stackalloc byte[512];uint present=0;NativeError error=default;
        fixed(byte* output=buffer)
            Check(services->ComponentGet(services->Context,&descriptor,&entity,output,descriptor.Bytes,&present,&error),&error);
        if(present>1)throw new InvalidOperationException("Invalid native component presence flag.");
        value=present==0?default:T.Decode(buffer[..(int)descriptor.Bytes]);return present!=0;
    }
    public T Get<T>(EntityId entity) where T:unmanaged,IGameplayComponent<T> =>
        TryGet<T>(entity,out var value)?value:throw new InvalidOperationException("Entity does not have the requested component.");
    /// <summary>Queues a full replacement after Tick. Reads do not observe queued writes; duplicates reject.</summary>
    public void Set<T>(EntityId entity,in T value) where T:unmanaged,IGameplayComponent<T>
    {
        var descriptor=ComponentDescriptor<T>();Span<byte> buffer=stackalloc byte[512];var payload=buffer[..(int)descriptor.Bytes];payload.Clear();
        T.Encode(in value,payload);NativeError error=default;
        fixed(byte* input=payload)Check(services->ComponentSet(services->Context,&descriptor,&entity,input,descriptor.Bytes,&error),&error);
    }
    public bool IsAlive(EntityId entity)
    {
        uint alive=0;NativeError error=default;Check(services->EntityAlive(services->Context,&entity,&alive,&error),&error);
        if(alive>1)throw new InvalidOperationException("Invalid native entity-liveness flag.");return alive!=0;
    }
}
