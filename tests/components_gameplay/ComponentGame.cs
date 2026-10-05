// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using Poima;
namespace Poima.Tests;

#if COMPONENT_LABELS
[GameplayComponent("11111111111111111111111111111111",Name="Vitality")]
#else
[GameplayComponent("11111111111111111111111111111111")]
#endif
public partial struct Health
{
#if !COMPONENT_REORDER
    [GameplayField("00000000000000000000000000000003",Default="-9223372036854775808")]
    public long Score;
#endif
#if COMPONENT_INCOMPATIBLE
    [GameplayField("00000000000000000000000000000001",Default="101",Unit="hp")]
#elif COMPONENT_LABELS
    [GameplayField("00000000000000000000000000000001",Name="Hit points",Default="100",Unit="points")]
#else
    [GameplayField("00000000000000000000000000000001",Default="100",Unit="hp")]
#endif
    public float Current;
    [GameplayField("00000000000000000000000000000002",Default="100",Unit="hp")]
    public int Maximum;
#if COMPONENT_REORDER
    [GameplayField("00000000000000000000000000000003",Default="-9223372036854775808")]
    public long Score;
#endif
}
[GameplayComponent("22222222222222222222222222222222")]
public partial struct Interaction
{
    [GameplayField("00000000000000000000000000000002",Default="-0")]
    public double Weight;
    [GameplayField("00000000000000000000000000000001")]
    public EntityId Target;
}
[StructLayout(LayoutKind.Sequential)]
public struct ComponentGameState
{
    public int Ticks,Mode,Queried,Missing,Alive,ReadBeforeWrite;
    public EntityId Selected;
    public float LastHealth;
    public long LastScore;
    public double LastWeight;
    public long SaveHigh,SaveLow,SaveSequence;
    public int SaveState,SaveRequests;
}
[GameModule("poima.test.components")]
public sealed class ComponentGame : Game<ComponentGameState>
{
    public override void Initialize(ref ComponentGameState state) { state.LastScore=long.MinValue; }
    public override void Tick(ref ComponentGameState state,GameContext context)
    {
        ++state.Ticks;
        Span<EntityId> page=stackalloc EntityId[2];EntityId after=default;int count;
        state.Queried=0;
        while((count=context.Query<Health>(page,after))!=0)
        {
            for(int i=0;i<count;++i)
            {
                if(page[i].High<after.High || (page[i].High==after.High && page[i].Low<=after.Low))throw new InvalidOperationException("Component query order changed.");
                after=page[i];++state.Queried;
                if(state.Selected==default)state.Selected=page[i];
            }
        }
        state.Alive=context.IsAlive(state.Selected)?1:0;
        if(context.IsAlive(default) || context.IsAlive(new(ulong.MaxValue,ulong.MaxValue)))throw new InvalidOperationException("Unknown entity reported alive.");
        if(state.Selected==default)return;
        var health=context.Get<Health>(state.Selected);state.LastHealth=health.Current;state.LastScore=health.Score;
        state.Missing=context.TryGet<Interaction>(state.Selected,out var interaction)?0:1;
        if(state.Missing==0)
        {
            state.LastWeight=interaction.Weight;
            if(interaction.Target!=default && !context.IsAlive(interaction.Target))throw new InvalidOperationException("Unresolved native reference.");
        }
        if(state.Mode==0)return;
        health.Current-=1;health.Score=checked(health.Score+1);context.Set(state.Selected,in health);
        state.ReadBeforeWrite=context.Get<Health>(state.Selected).Current==state.LastHealth?1:0;
        if(state.ReadBeforeWrite!=1)throw new InvalidOperationException("Queued component writes leaked into reads.");
        if(state.Mode==2)context.Set(state.Selected,in health); // Duplicate queued write must fail transactionally.
        if(state.Mode==3)throw new InvalidOperationException("Component fixture rollback.");
        if(state.Mode==4 && context.Tick%2==1)throw new InvalidOperationException("Later component fixture rollback.");
        if(state.Mode==5) { health.Current=float.NaN;context.Set(state.Selected,in health); }
        if(state.Mode==6) { interaction.Target=new(ulong.MaxValue,ulong.MaxValue);context.Set(state.Selected,in interaction); }
        if(state.Mode==7)context.Get<Health>(new(ulong.MaxValue,ulong.MaxValue));
        if(state.Mode is 8 or 9)
        {
            if(state.SaveSequence==0)
            {
                var ticket=state.Mode==8?context.RequestSave("components-game",0):context.RequestLoad("components-game",1);
                state.SaveHigh=ticket.EpochHigh;state.SaveLow=ticket.EpochLow;state.SaveSequence=ticket.Sequence;++state.SaveRequests;
            }
            state.SaveState=(int)context.GetSaveResult(new(state.SaveHigh,state.SaveLow,state.SaveSequence)).State;
        }
    }
}
