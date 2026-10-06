// SPDX-License-Identifier: Apache-2.0
using Poima;
using System.Runtime.InteropServices;
namespace Poima.Tests;
[GameplayComponent("11111111111111111111111111111111",Name="Evolution cell")]
public partial struct EvolutionCell
{
    [GameplayField("00000000000000000000000000000001",Default="55")] public int Retired;
    [GameplayField("00000000000000000000000000000002",Default="10",Unit="items")] public int Count;
    [GameplayField("00000000000000000000000000000003",Default="-9223372036854775808")] public long Total;
    [GameplayField("00000000000000000000000000000004",Default="1.25",Unit="kg")] public float Weight;
    [GameplayField("00000000000000000000000000000005",Default="4.5",Unit="m")] public double Distance;
    [GameplayField("00000000000000000000000000000006")] public EntityId Link;
}
[GameplayPersistence(1)]
[StructLayout(LayoutKind.Sequential)]
public struct EvolutionState
{
    [GameplayField("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa1")] public int Ticks;
    [GameplayField("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa2")] public EntityId Spawned;
    [GameplayField("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa3")] public int Checked;
    [GameplayField("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa4")] public int BonusObserved;
}
[GameModule("poima.test.save-evolution-components")]
public sealed class SaveEvolutionGame:Game<EvolutionState>
{
    static readonly EntityId Anchor=new(0,2);
    static readonly TemplateId Template=new(0,200);
    public override void Initialize(ref EvolutionState state)
    {
        if(Environment.GetEnvironmentVariable("POIMA_COMPONENT_UPGRADE_FORBID_INITIALIZE")=="1")
            throw new InvalidOperationException("Initialize forbidden during component save upgrade.");
        state=default;
    }
    public override void Tick(ref EvolutionState state,GameContext context)
    {
        Span<EntityId> page=stackalloc EntityId[4];int count=context.Query<EvolutionCell>(page);
        if(count!=(state.Ticks==0?1:2))throw new InvalidOperationException("Component membership changed.");
        state.Checked=0;
        for(int i=0;i<count;++i)
        {
            var cell=context.Get<EvolutionCell>(page[i]);
            if(cell.Link!=Anchor || !context.IsAlive(cell.Link))throw new InvalidOperationException("Retained component reference differs.");

            cell.Count=checked(cell.Count+1);cell.Total=checked(cell.Total+1);cell.Weight+=.25f;cell.Distance+=.5;
            ++cell.Retired;
            context.Set(page[i],in cell);++state.Checked;
        }
        if(state.Ticks==0)
        {
            state.Spawned=context.Spawn(Template);
            var born=context.GetTemplate<EvolutionCell>(Template);
            born.Count=101;born.Total=long.MinValue+1;born.Weight=1.5f;born.Distance=5;born.Link=Anchor;
            context.Set(state.Spawned,in born);
        }
        ++state.Ticks;
    }
}
