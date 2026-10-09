// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using System.Runtime.InteropServices;
namespace Poima;

/// <summary>Opts into instance-local node resolution, independently of animation, input and navigation.</summary>
public interface IHierarchicalInstancesGame { }

/// <summary>A frozen template-local node identity, distinct from a live entity. Not a persisted state field kind.</summary>
[StructLayout(LayoutKind.Sequential)]
public readonly record struct TemplateNodeId(ulong High,ulong Low)
{
    public static TemplateNodeId Parse(string value)
    {
        ArgumentNullException.ThrowIfNull(value);
        if(value.Length!=32)throw new ArgumentException("Template node IDs require 32 lowercase hexadecimal digits.",nameof(value));
        foreach(char c in value)if(c is not (>= '0' and <= '9') and not (>= 'a' and <= 'f'))
            throw new ArgumentException("Template node IDs require 32 lowercase hexadecimal digits.",nameof(value));
        var id=new TemplateNodeId(ulong.Parse(value.AsSpan(0,16),NumberStyles.HexNumber,CultureInfo.InvariantCulture),
            ulong.Parse(value.AsSpan(16),NumberStyles.HexNumber,CultureInfo.InvariantCulture));
        if(id==default)throw new ArgumentException("Template node ID cannot be zero.",nameof(value));
        return id;
    }
    public override string ToString()=>$"{High:x16}{Low:x16}";
}
[StructLayout(LayoutKind.Sequential)] internal unsafe struct NativeInstanceServicesV1
{
    public NativeNavigationServicesV1 Navigation;
    public delegate* unmanaged[Cdecl]<void*,EntityId*,EntityId*,EntityId*,NativeError*,int> InstanceNode;
}
internal static unsafe class InstanceServiceAbi
{
    internal const uint RequiredBytes=232;
    internal static bool LayoutValid()
    {
        NativeInstanceServicesV1 services=default;
        return NavigationServiceAbi.LayoutValid() && sizeof(NativeInstanceServicesV1)==232 && sizeof(TemplateNodeId)==16 &&
            (byte*)&services.Navigation-(byte*)&services==0 && (byte*)&services.InstanceNode-(byte*)&services==224;
    }
    internal static NativeInstanceServicesV1* Validate(NativeServices* services)
    {
        if(services==null || services->Version!=ServiceAbi.Epoch || services->Bytes<RequiredBytes)
            throw new ArgumentException("Instance service ABI mismatch: hierarchical_instances_v1 requires epoch 7 with at least 232 bytes. Declare IHierarchicalInstancesGame.");
        var extended=(NativeInstanceServicesV1*)services;
        if(extended->InstanceNode==null)throw new ArgumentException("Versioned instance resolver callback is absent.");
        return extended;
    }
}
public readonly unsafe ref partial struct GameContext
{
    /// <summary>Resolves a template-local node on a committed instance or a noncanceled birth reserved in this Tick.
    /// Resolution does not publish the node: Get, IsAlive and component queries still read committed state. Unknown roots/nodes throw.</summary>
    public EntityId ResolveNode(EntityId instanceRoot,TemplateNodeId node)
    {
        RequireFeature(GameplayRequiredFeatures.HierarchicalInstances,nameof(IHierarchicalInstancesGame));
        if(instanceRoot==default)throw new ArgumentException("Instance root cannot be zero.",nameof(instanceRoot));
        if(node==default)throw new ArgumentException("Template node ID cannot be zero.",nameof(node));
        var extended=InstanceServiceAbi.Validate(services);
        EntityId local=new(node.High,node.Low),result=default;NativeError error=default;
        Check(extended->InstanceNode(services->Context,&instanceRoot,&local,&result,&error),&error);
        if(result==default)throw new InvalidOperationException("Native instance resolver returned a zero entity ID.");
        return result;
    }
}
public readonly unsafe ref partial struct ControlContext
{
    /// <summary>Resolves a node on a committed instance at unchanged simulation time. Unknown roots/nodes throw.</summary>
    public EntityId ResolveNode(EntityId instanceRoot,TemplateNodeId node)=>context.ResolveNode(instanceRoot,node);
}
