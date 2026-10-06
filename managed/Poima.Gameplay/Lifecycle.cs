// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Numerics;
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>A frozen recipe identity, distinct from a live entity. Not a supported persisted state field kind.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly record struct TemplateId(ulong High,ulong Low)
{
    public static TemplateId Parse(string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if(value.Length!=32)throw new ArgumentException("Template IDs require 32 lowercase hexadecimal digits.",nameof(value));
        foreach(char c in value)if(c is not (>= '0' and <= '9') and not (>= 'a' and <= 'f'))
            throw new ArgumentException("Template IDs require 32 lowercase hexadecimal digits.",nameof(value));
        var id=new TemplateId(ulong.Parse(value.AsSpan(0,16),NumberStyles.HexNumber,CultureInfo.InvariantCulture),
            ulong.Parse(value.AsSpan(16),NumberStyles.HexNumber,CultureInfo.InvariantCulture));
        if(id==default)throw new ArgumentException("Template ID cannot be zero.",nameof(value));
        return id;
    }
    public override string ToString()=>$"{High:x16}{Low:x16}";
}
/// <summary>A complete root transform override. Position/scale use doubles; rotation is normalized XYZW.</summary>
public readonly record struct SpawnTransform(Vector3d Position,Quaternion Rotation,Vector3d Scale);
[StructLayout(LayoutKind.Sequential)]
internal struct NativeTransform
{
    public Vector3d Position;
    public double X,Y,Z,W;
    public Vector3d Scale;
}
internal static unsafe class LifecycleAbiLayout
{
    internal static bool Valid()
    {
        NativeServices services=default;NativeTransform transform=default;TemplateId id=default;
        return sizeof(TemplateId)==16 && sizeof(NativeTransform)==80 && sizeof(NativeServices)==144 &&
            (byte*)&transform.X-(byte*)&transform==24 && (byte*)&transform.Scale-(byte*)&transform==56 &&
            (byte*)&services.Spawn-(byte*)&services==120 && (byte*)&services.Despawn-(byte*)&services==128 &&
            (byte*)&services.TemplateComponentGet-(byte*)&services==136 && id.High==0 && id.Low==0;
    }
}
public readonly unsafe ref partial struct GameContext
{
    private static void RequireTemplate(TemplateId template)
    { if(template==default)throw new ArgumentException("Template ID cannot be zero.",nameof(template)); }
    private EntityId SpawnCore(TemplateId template,NativeTransform* transform)
    {
        RequireTemplate(template);EntityId entity=default;NativeError error=default;
        Check(services->Spawn(services->Context,&template,transform,&entity,&error),&error);
        if(entity==default)throw new InvalidOperationException("Native spawn returned a zero entity ID.");
        return entity;
    }
    /// <summary>Reserves an ID using the frozen recipe transform. Reads remain published state until Tick ends.
    /// Set may initialize this ID before publication. A failed batch rolls back the reservation.</summary>
    public EntityId Spawn(TemplateId template)=>SpawnCore(template,null);
    /// <summary>Reserves an ID with a complete transform override; other recipe values remain frozen defaults.</summary>
    public EntityId Spawn(TemplateId template,in SpawnTransform transform)
    {
        var value=new NativeTransform {Position=transform.Position,X=transform.Rotation.X,Y=transform.Rotation.Y,
            Z=transform.Rotation.Z,W=transform.Rotation.W,Scale=transform.Scale};
        return SpawnCore(template,&value);
    }
    /// <summary>Queues removal of a spawned root. Same-Tick spawn cancellation consumes its ID on successful batch commit.</summary>
    public void Despawn(EntityId entity)
    {
        if(entity==default)throw new ArgumentException("Despawn target cannot be zero.",nameof(entity));
        NativeError error=default;Check(services->Despawn(services->Context,&entity,&error),&error);
    }
    /// <summary>Reads frozen recipe values. False means the template exists without this component; unknown templates reject.</summary>
    public bool TryGetTemplate<T>(TemplateId template,out T value) where T:unmanaged,IGameplayComponent<T>
    {
        RequireTemplate(template);var descriptor=ComponentDescriptor<T>();Span<byte> buffer=stackalloc byte[512];uint present=0;NativeError error=default;
        fixed(byte* output=buffer)
            Check(services->TemplateComponentGet(services->Context,&descriptor,&template,output,descriptor.Bytes,&present,&error),&error);
        if(present>1)throw new InvalidOperationException("Invalid native template component presence flag.");
        value=present==0?default:T.Decode(buffer[..(int)descriptor.Bytes]);return present!=0;
    }
    /// <summary>Reads a component from the frozen recipe, independently of all spawned instances.</summary>
    public T GetTemplate<T>(TemplateId template) where T:unmanaged,IGameplayComponent<T> =>
        TryGetTemplate<T>(template,out var value)?value:throw new InvalidOperationException("Template does not have the requested component.");
}
