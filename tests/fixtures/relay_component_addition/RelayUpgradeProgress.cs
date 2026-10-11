// SPDX-License-Identifier: Apache-2.0
using Poima;
namespace Poima.Examples;

[GameplayComponent("c1120000000000000000000000000001")]
public partial struct RelayUpgradeProgress
{
    [GameplayField("00000000000000000000000000000001",Default="0")] public int Count;
    [GameplayField("00000000000000000000000000000002",Default="0")] public int Collected;
    [GameplayField("00000000000000000000000000000003",Default="0")] public int Won;

    internal static void Observe(GameContext context,in RelayState state)
    {
        Span<EntityId> ids=stackalloc EntityId[3];
        int count=context.Query<RelayUpgradeProgress>(ids);
        if(count!=2 || ids[0]!=RelayYardGame.Player || ids[1]!=RelayYardGame.Actor)
            throw new InvalidOperationException("Upgrade Progress needs exactly the permanent player and actor.");
        for(int index=0;index<count;++index)
        {
            var progress=context.Get<RelayUpgradeProgress>(ids[index]);
            progress.Count=checked(progress.Count+1);
            progress.Collected=state.Collected;progress.Won=state.Won;
            context.Set(ids[index],in progress);
        }
    }
}
