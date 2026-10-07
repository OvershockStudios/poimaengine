// SPDX-License-Identifier: Apache-2.0
namespace Poima;

/// <summary>Generates an unmanaged fixed-capacity buffer for component fields.
/// Apply to an empty public partial struct. Elements are int, long, float,
/// double or EntityId; capacity is 1..31 and the whole component wire payload
/// remains limited to 512 bytes. Buffers initially contain no elements.</summary>
[AttributeUsage(AttributeTargets.Struct,Inherited=false)]
public sealed class GameplayBufferAttribute(Type elementType,int capacity):Attribute
{
    public Type ElementType { get; }=elementType;
    public int Capacity { get; }=capacity;
}
