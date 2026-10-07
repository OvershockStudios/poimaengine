// SPDX-License-Identifier: Apache-2.0
using Poima;
using System.Runtime.InteropServices;
namespace Poima.Tests;
[GameplayBuffer(typeof(EntityId),4)] public partial struct ItemSlots {}
[GameplayBuffer(typeof(int),4)] public partial struct QuantitySlots {}
[GameplayComponent("66666666666666666666666666666666")]
public partial struct Inventory
{
    [GameplayField("00000000000000000000000000000001")] public ItemSlots Items;
    [GameplayField("00000000000000000000000000000002")] public QuantitySlots Quantities;
    [GameplayField("00000000000000000000000000000003")] public int Changes;
}
[StructLayout(LayoutKind.Sequential)]
public struct CollectionGameplayState
{
    public int Mode,Ticks,Started,ObservedCount,FullRejected,PublishedRead,Inventories;
    public EntityId Spawned,FirstItem,SecondItem,ThirdItem,LastItem;
}
[GameModule("poima.test.collection-gameplay")]
public sealed class CollectionGameplay:Game<CollectionGameplayState>
{
    static readonly EntityId Owner=new(0,1);
    static readonly TemplateId Bag=new(0,200),Item=new(0,201);
    static void Check(bool ok,string message){if(!ok)throw new InvalidOperationException(message);}
    public override void Initialize(ref CollectionGameplayState state)
    {
        if(Environment.GetEnvironmentVariable("POIMA_COLLECTION_FORBID_INITIALIZE")=="1")
            throw new InvalidOperationException("Initialize forbidden during collection exact restore.");
        state=default;
    }
    public override void Tick(ref CollectionGameplayState state,GameContext context)
    {
        ++state.Ticks;
        if(state.Started==0)
        {
            state.FirstItem=context.Spawn(Item);state.SecondItem=context.Spawn(Item);
            state.ThirdItem=context.Spawn(Item);state.LastItem=context.Spawn(Item);
            state.Started=1;return;
        }
        Span<EntityId> page=stackalloc EntityId[8];int count=context.Query<Inventory>(page);state.Inventories=count;
        for(int i=0;i<count;++i)
        {
            var inventory=context.Get<Inventory>(page[i]);Check(inventory.Items.Count==inventory.Quantities.Count,"Inventory parallel cardinality differs.");
            for(int j=0;j<inventory.Items.Count;++j)Check(context.IsAlive(inventory.Items[j]),"Inventory reference is not alive.");
        }
        var value=context.Get<Inventory>(Owner);state.ObservedCount=value.Items.Count;
        switch(state.Mode)
        {
            case 0:return;
            case 1:
                int previous=value.Items.Count;var item=(previous%4) switch {0=>state.FirstItem,1=>state.SecondItem,2=>state.ThirdItem,_=>state.LastItem};
                bool added=value.Items.TryAdd(item);Check(value.Quantities.TryAdd(10+previous)==added,"Capacity outcomes differ.");
                if(!added) {++state.FullRejected;Check(value.Items.Count==previous,"Full insertion changed length.");}
                ++value.Changes;context.Set(Owner,in value);
                state.PublishedRead=context.Get<Inventory>(Owner).Items.Count==previous?1:0;
                Check(state.PublishedRead==1,"Queued collection write leaked into published read.");return;
            case 2:
                Check(value.Items.Count>0,"Dangling fixture needs an item.");value.Items[0]=new(0,999);context.Set(Owner,in value);return;
            case 3:
                value.Items.Clear();value.Quantities.Clear();context.Set(Owner,in value);throw new InvalidOperationException("Intentional collection rollback.");
            case 4:
                value.Items.RemoveAt(value.Items.Count);return; // Invalid index must not alter native or global state.
            case 5:
                state.Spawned=context.Spawn(Bag);
                var initial=context.GetTemplate<Inventory>(Bag);Check(initial.Items.Count==0 && initial.Quantities.Count==2 && initial.Quantities[0]==2 && initial.Quantities[1]==5,"Template collection contents differ.");
                Check(initial.Items.TryAdd(state.FirstItem) && initial.Items.TryAdd(state.LastItem),"Cannot fill pending bag references.");
                context.Set(state.Spawned,in initial);state.Mode=0;return;
            case 6:
                for(int i=0;i<count;++i)
                {
                    var repaired=context.Get<Inventory>(page[i]);
                    for(int j=repaired.Items.Count-1;j>=0;--j)if(repaired.Items[j]==state.FirstItem || repaired.Items[j]==state.LastItem){repaired.Items.RemoveAt(j);repaired.Quantities.RemoveAt(j);}
                    context.Set(page[i],in repaired);
                }
                context.Despawn(state.FirstItem);context.Despawn(state.LastItem);state.FirstItem=default;state.LastItem=default;state.Mode=0;return;
            case 7:
                context.Despawn(state.FirstItem);state.FirstItem=default;return; // Referenced removal must reject the whole tick.
            case 10:
                context.Despawn(state.LastItem);state.LastItem=default;return; // Last collection element must be validated too.
            case 8:
                value.Items.Clear();value.Quantities.Clear();context.Set(Owner,in value);state.Mode=0;return;
            case 9:
                ++value.Changes;context.Set(Owner,in value);
                if(context.Tick%2==1)throw new InvalidOperationException("Later tick collection rollback.");return;
            default:throw new InvalidOperationException("Unknown collection fixture mode.");
        }
    }
}
