// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using System.Runtime.InteropServices;
using Poima;
namespace Poima.Tests;

[GameplayComponent("11111111111111111111111111111111")]
public partial struct LifecycleLink
{
    [GameplayField("00000000000000000000000000000001",Default="7")]
    public int Value;
    [GameplayField("00000000000000000000000000000002")]
    public EntityId Target;
}

[StructLayout(LayoutKind.Sequential)]
public struct ManagedLifecycleState
{
    public int Mode,Ticks,Queried,VisibleFirst,SeenFirst,SeenSecond,CaughtBad,TemplateDefault;
    public EntityId First,Second;
    public long SaveHigh,SaveLow,SaveSequence;
}

[GameModule("poima.test.managed-lifecycle")]
public sealed class ManagedLifecycleGame : Game<ManagedLifecycleState>
{
    static readonly TemplateId Linked=TemplateId.Parse("44444444444444444444444444444444");
    static readonly TemplateId Plain=TemplateId.Parse("33333333333333333333333333333333");
    static readonly TemplateId Missing=TemplateId.Parse("99999999999999999999999999999999");
    static void Check(bool condition,string message)
    {
        if(!condition)throw new InvalidOperationException(message);
    }
    public override void Initialize(ref ManagedLifecycleState state) {state=default;}
    public override void Tick(ref ManagedLifecycleState state,GameContext context)
    {
        ++state.Ticks;
        Span<EntityId> page=stackalloc EntityId[2];EntityId after=default;int count;
        state.Queried=0;
        while((count=context.Query<LifecycleLink>(page,after))!=0)
        {
            state.Queried+=count;after=page[count-1];
        }
        state.VisibleFirst=context.IsAlive(state.First)?1:0;
        if(state.VisibleFirst!=0)state.SeenFirst=context.Get<LifecycleLink>(state.First).Value;
        if(context.IsAlive(state.Second))state.SeenSecond=context.Get<LifecycleLink>(state.Second).Value;
        switch(state.Mode)
        {
            case 0:return;
            case 1:
                var defaults=context.GetTemplate<LifecycleLink>(Linked);
                state.TemplateDefault=defaults.Value;
                Check(defaults.Value==7 && defaults.Target==default,"Frozen template defaults changed.");
                Check(!context.TryGetTemplate<LifecycleLink>(Plain,out _),"Plain template unexpectedly has a component.");
                state.First=context.Spawn(Linked);
                var pose=new SpawnTransform(new(4,2,0),Quaternion.Identity,new(1,1,1));
                state.Second=context.Spawn(Linked,in pose);
                Check(!context.IsAlive(state.First) && context.Query<LifecycleLink>(page)==1,"Pending birth leaked into published liveness/query.");
                bool rejectedPending=false;
                try {context.TryGet<LifecycleLink>(state.First,out _);}
                catch(InvalidOperationException) {rejectedPending=true;}
                Check(rejectedPending,"Pending birth component read did not reject unknown published identity.");
                var first=new LifecycleLink {Value=11,Target=state.Second};
                var second=new LifecycleLink {Value=22,Target=state.First};
                context.Set(state.First,in first);context.Set(state.Second,in second);
                context.MoveKinematic(state.First,new(2,2,0),Quaternion.Identity,60);
                state.Mode=0;return;
            case 3:
                try {context.Spawn(Missing);throw new Exception("Invalid template unexpectedly spawned.");}
                catch(InvalidOperationException) {++state.CaughtBad;}
                var invalidPose=new SpawnTransform(new(0,2,0),Quaternion.Identity,new(0,1,1));
                try {context.Spawn(Linked,in invalidPose);throw new Exception("Invalid transform unexpectedly spawned.");}
                catch(InvalidOperationException) {++state.CaughtBad;}
                var canceled=context.Spawn(Linked);
                var canceledValue=new LifecycleLink {Value=99};context.Set(canceled,in canceledValue);
                context.MoveKinematic(canceled,new(9,2,0),Quaternion.Identity,60);
                context.Despawn(canceled);
                state.First=context.Spawn(Linked);
                context.MoveKinematic(state.First,new(2,2,0),Quaternion.Identity,60);
                state.Mode=0;return;
            case 4:
                Check(context.IsAlive(state.First) && context.IsAlive(state.Second),"Repair needs two published births.");
                var survivor=context.Get<LifecycleLink>(state.Second);survivor.Target=default;
                context.Set(state.Second,in survivor);context.Despawn(state.First);
                Check(context.IsAlive(state.First),"Queued removal leaked into published reads.");
                state.First=default;state.Mode=0;return;
            case 5:
                if(context.Tick%2==1)throw new InvalidOperationException("Later lifecycle fixture rollback.");
                state.First=context.Spawn(Linked);
                var initial=new LifecycleLink {Value=33};context.Set(state.First,in initial);
                context.MoveKinematic(state.First,new(3,2,0),Quaternion.Identity,60);
                var ticket=context.RequestSave("managed-lifecycle",0);
                state.SaveHigh=ticket.EpochHigh;state.SaveLow=ticket.EpochLow;state.SaveSequence=ticket.Sequence;
                return;
            case 6:
                var only=context.Spawn(Linked);
                var discarded=new LifecycleLink {Value=55};context.Set(only,in discarded);
                context.MoveKinematic(only,new(5,2,0),Quaternion.Identity,60);
                context.Despawn(only);state.Mode=0;return;
            case 7:
                state.First=context.Spawn(Linked);context.Despawn(state.First);return;
            case 8:
                for(int i=0;i<2047;++i) {var reserved=context.Spawn(Linked);context.Despawn(reserved);}
                state.First=context.Spawn(Linked);
                try {context.Spawn(Linked);throw new Exception("Combined host/gameplay budget unexpectedly allowed another spawn.");}
                catch(InvalidOperationException) {++state.CaughtBad;}
                state.Mode=0;return;
            default:throw new InvalidOperationException("Unknown lifecycle fixture mode.");
        }
    }
}
