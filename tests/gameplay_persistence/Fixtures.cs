// SPDX-License-Identifier: Apache-2.0
using Poima;
using System.Runtime.InteropServices;
[AttributeUsage(AttributeTargets.Struct|AttributeTargets.Field)]
public sealed class MetadataTrapAttribute : Attribute {public MetadataTrapAttribute()=>throw new Exception("Attribute constructor executed");}
[GameplayPersistence(9),MetadataTrap,StructLayout(LayoutKind.Sequential)]
public struct PersistentState
{
    [GameplayField("00000000000000000000000000000005",Default="-0"),MetadataTrap]public float Rate;
    [GameplayField("00000000000000000000000000000001",Default="-2147483648",Name="Count",Unit="")]public int Count;
    [GameplayField("00000000000000000000000000000003",Default="-9223372036854775808")]public long Total;
    [GameplayField("00000000000000000000000000000002",Default="1.25")]public double Amount;
    [GameplayField("00000000000000000000000000000004")]public EntityId Target;
}
[GameplayPersistence(int.MaxValue),StructLayout(LayoutKind.Sequential)]
public struct ZeroState
{
    [GameplayField("00000000000000000000000000000001")]public int I;
    [GameplayField("00000000000000000000000000000002")]public long L;
    [GameplayField("00000000000000000000000000000003")]public float F;
    [GameplayField("00000000000000000000000000000004",Default="-0.0")]public double D;
    [GameplayField("00000000000000000000000000000005",Default="00000000000000000000000000000000")]public EntityId E;
}
[StructLayout(LayoutKind.Sequential)]public struct LegacyState
{[GameplayField("invalid",Default="not-a-number",Name="Display label",Unit="ignored-without-opt-in")]public int Counter;}
[GameModule("poima.test.persistence")]
public sealed class PersistentGame : Game<PersistentState>
{
    public override void Initialize(ref PersistentState state)=>throw new Exception("Initialize must not execute while extracting metadata");
    public override void Tick(ref PersistentState state,GameContext context) { }
}
[GameModule("poima.test.zero-persistence")]
public sealed class ZeroGame : Game<ZeroState>
{
    public override void Initialize(ref ZeroState state)=>throw new Exception("Initialize must not execute while extracting metadata");
    public override void Tick(ref ZeroState state,GameContext context) { }
}
[GameModule("poima.test.legacy-persistence")]
public sealed class LegacyGame : Game<LegacyState>
{
    public override void Initialize(ref LegacyState state)=>throw new Exception("Initialize must not execute while extracting metadata");
    public override void Tick(ref LegacyState state,GameContext context) { }
}
[GameModule("poima.test.published-persistence")]
public sealed class PublishedPersistentGame : Game<PersistentState>
{
    public override void Initialize(ref PersistentState state) {state=default;state.Count=17;}
    public override void Tick(ref PersistentState state,GameContext context) {++state.Count;}
}
