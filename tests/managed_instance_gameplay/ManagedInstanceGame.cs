// SPDX-License-Identifier: Apache-2.0
using System.Numerics;
using System.Runtime.InteropServices;
using Poima;
namespace Poima.Tests;

[GameplayBuffer(typeof(EntityId),2)] public partial struct InstanceMembers { }

[GameplayComponent("eeeeeeeeeeeeeeeeeeeeeeeeeeee7810")]
public partial struct InstanceLink
{
    [GameplayField("00000000000000000000000000000001",Default="7")]
    public int Value;
    [GameplayField("00000000000000000000000000000002")]
    public EntityId Target;
    [GameplayField("00000000000000000000000000000003")]
    public EntityId Rig;
    [GameplayField("00000000000000000000000000000004")]
    public InstanceMembers Members;
}

[StructLayout(LayoutKind.Sequential)]
public struct ManagedInstanceState
{
    public int Mode,Ticks,Drive,Births,Canceled,Rejected,ControlCalls,ResolvedControls,SavePending,SaveKind,LastSaveState,LastSaveError;
    public EntityId First,Second,FirstRig,SecondRig;
    public double FirstX,FirstZ,SecondX,SecondZ,FirstTime,SecondTime;
    public long SaveHigh,SaveLow,SaveSequence,LastSaveGeneration,ObservedEpochHigh,ObservedEpochLow;
}

// A real consumer of imported rig graphs, staged component references and
// independent native controllers. The harness supplies the licensed model and
// the frozen hierarchy recipe; this module contains no model or art data.
[GameModule("poima.test.managed-instances")]
public sealed class ManagedInstanceGame : Game<ManagedInstanceState>,IHierarchicalInstancesGame,ICharacterInputGame
{
    static readonly TemplateId Actor=TemplateId.Parse("eeeeeeeeeeeeeeeeeeeeeeeeeeee7801");
    static readonly TemplateNodeId Root=TemplateNodeId.Parse("eeeeeeeeeeeeeeeeeeeeeeeeeeee7802");
    static readonly TemplateNodeId Rig=TemplateNodeId.Parse("eeeeeeeeeeeeeeeeeeeeeeeeeeee7803");
    static readonly TemplateNodeId Unknown=TemplateNodeId.Parse("eeeeeeeeeeeeeeeeeeeeeeeeeeee78ff");
    const string Slot="managed-instances";
    static void Check(bool condition,string message) {if(!condition)throw new InvalidOperationException(message);}
    public override void Initialize(ref ManagedInstanceState state) {state=default;state.Drive=1;}

    static EntityId Birth(GameContext context,double x,double z,double speed,out EntityId rig,float moveRight=0,float moveForward=0)
    {
        var transform=new SpawnTransform(new(x,0,z),Quaternion.Identity,new(1,1,1));
        var root=context.Spawn(Actor,in transform);
        Check(context.ResolveNode(root,Root)==root,"Reserved recipe root resolves to a different identity.");
        rig=context.ResolveNode(root,Rig);
        Check(rig!=root && rig!=default,"Reserved rig mapping is invalid.");
        Check(!context.IsAlive(root) && !context.IsAlive(rig),"Reserved hierarchy leaked into committed liveness.");
        bool rejected=false;try {_=context.Get(root);}catch(InvalidOperationException){rejected=true;}
        Check(rejected,"Reserved native entity read unexpectedly succeeded.");
        rejected=false;try {_=context.GetAnimation(rig);}catch(InvalidOperationException){rejected=true;}
        Check(rejected,"Reserved rig animation read unexpectedly succeeded.");
        context.SetAnimation(rig,0,speed:speed,loop:true,playing:true);
        context.SetCharacterInput(root,moveRight,moveForward);
        return root;
    }
    static InstanceLink Link(int value,EntityId target,EntityId root,EntityId rig)
    {
        InstanceLink link=new(){Value=value,Target=target,Rig=rig};
        Check(link.Members.TryAdd(root) && link.Members.TryAdd(rig),"Instance reference buffer capacity changed.");
        return link;
    }
    static void ValidateLink(GameContext context,EntityId root,EntityId rig)
    {
        var link=context.Get<InstanceLink>(root);
        Check(link.Rig==rig && link.Members.Count==2 && link.Members[0]==root && link.Members[1]==rig,"Internal scalar/array references are not instance-local.");
    }
    static SaveTicket Ticket(in ManagedInstanceState state)=>new(state.SaveHigh,state.SaveLow,state.SaveSequence);
    static void Reconcile(ref ManagedInstanceState state,SaveCapabilities saves)
    {
        if(state.ObservedEpochHigh==saves.Epoch.High && state.ObservedEpochLow==saves.Epoch.Low)return;
        if(saves.LastRestore is { } restore)
        {
            state.SavePending=0;state.LastSaveState=(int)SaveOperationState.Succeeded;state.LastSaveError=0;
            state.LastSaveGeneration=checked((long)restore.Generation);
        }
        state.ObservedEpochHigh=saves.Epoch.High;state.ObservedEpochLow=saves.Epoch.Low;
    }
    static void Result(ref ManagedInstanceState state,SaveOperationResult result)
    {
        state.LastSaveState=(int)result.State;state.LastSaveError=result.ErrorCode;
        state.LastSaveGeneration=checked((long)result.Generation);
        if(result.IsTerminal)state.SavePending=0;
    }
    public override void Tick(ref ManagedInstanceState state,GameContext context)
    {
        ++state.Ticks;Reconcile(ref state,context.Saves);
        if(state.SavePending!=0)Result(ref state,context.GetSaveResult(Ticket(in state)));
        switch(state.Mode)
        {
            case 1:
                Check(state.First==default && state.Second==default,"Two-birth mode requires no existing instance handles.");
                state.First=Birth(context,-4,0,1,out state.FirstRig);
                state.Second=Birth(context,4,0,2,out state.SecondRig);
                Check(state.First!=state.Second && state.FirstRig!=state.SecondRig,"Instances alias live identities.");
                var first=Link(11,state.Second,state.First,state.FirstRig);
                var second=Link(22,state.First,state.Second,state.SecondRig);
                context.Set(state.First,in first);context.Set(state.Second,in second);
                state.Births+=2;state.Mode=0;return;
            case 2:
                var failed=Birth(context,0,3,3,out var failedRig,.25f,.5f);
                var staged=Link(33,default,failed,failedRig);context.Set(failed,in staged);
                throw new InvalidOperationException("Intentional hierarchical birth rollback.");
            case 3:
                var canceled=Birth(context,0,3,3,out var canceledRig,.25f,.5f);
                var canceledLink=Link(44,default,canceled,canceledRig);context.Set(canceled,in canceledLink);
                context.Despawn(canceled);
                bool denied=false;try {_=context.ResolveNode(canceled,Rig);}catch(InvalidOperationException){denied=true;}
                Check(denied,"Canceled birth still resolves a node.");
                ++state.Canceled;state.Mode=0;return;
            case 4:
                // Both state handles and the survivor's Target still reference
                // this instance. Native candidate validation must reject commit.
                context.Despawn(state.First);state.Mode=0;return;
            case 5:
                Check(context.IsAlive(state.First) && context.IsAlive(state.Second),"Reference repair needs both published instances.");
                var survivor=context.Get<InstanceLink>(state.Second);survivor.Target=default;
                context.Set(state.Second,in survivor);context.Despawn(state.First);
                Check(context.IsAlive(state.First),"Queued removal leaked into committed reads.");
                state.First=default;state.FirstRig=default;state.Mode=0;return;
            case 6:
                context.Despawn(state.Second);state.Second=default;state.SecondRig=default;state.Mode=0;return;
            case 7:
                Check(state.First!=default,"Unknown-node check requires a committed instance.");
                bool unknown=false;try {_=context.ResolveNode(state.First,Unknown);}catch(InvalidOperationException){unknown=true;}
                Check(unknown,"Unknown template-local node unexpectedly resolved.");
                ++state.Rejected;state.Mode=0;return;
            case 0:break;
            default:throw new InvalidOperationException("Unknown instance fixture mode.");
        }
        if(state.First!=default)
        {
            Check(context.ResolveNode(state.First,Root)==state.First && context.ResolveNode(state.First,Rig)==state.FirstRig,"First instance map changed.");
            ValidateLink(context,state.First,state.FirstRig);
            var entity=context.Get(state.First);state.FirstX=entity.Transform.Position.X;state.FirstZ=entity.Transform.Position.Z;
            var playback=context.GetAnimation(state.FirstRig) ?? throw new InvalidOperationException("First rig disappeared.");
            state.FirstTime=playback.Time;
            if(state.Drive!=0)context.SetCharacterInput(state.First,0,.35f);
        }
        if(state.Second!=default)
        {
            Check(context.ResolveNode(state.Second,Root)==state.Second && context.ResolveNode(state.Second,Rig)==state.SecondRig,"Second instance map changed.");
            ValidateLink(context,state.Second,state.SecondRig);
            var entity=context.Get(state.Second);state.SecondX=entity.Transform.Position.X;state.SecondZ=entity.Transform.Position.Z;
            var playback=context.GetAnimation(state.SecondRig) ?? throw new InvalidOperationException("Second rig disappeared.");
            state.SecondTime=playback.Time;
            if(state.Drive!=0)context.SetCharacterInput(state.Second,-.2f,0);
        }
    }
    public override void Control(ref ManagedInstanceState state,ControlContext context)
    {
        ++state.ControlCalls;Reconcile(ref state,context.Saves);
        if(state.SavePending!=0)Result(ref state,context.GetSaveResult(Ticket(in state)));
        switch(context.Action)
        {
            case "instances.inspect":
                if(state.First!=default)Check(context.ResolveNode(state.First,Rig)==state.FirstRig,"Control first rig resolution changed.");
                if(state.Second!=default)Check(context.ResolveNode(state.Second,Rig)==state.SecondRig,"Control second rig resolution changed.");
                ++state.ResolvedControls;break;
            case "instances.birth":state.Mode=1;break;
            case "instances.fail":state.Mode=2;break;
            case "instances.cancel":state.Mode=3;break;
            case "instances.break-references":state.Mode=4;break;
            case "instances.repair":state.Mode=5;break;
            case "instances.despawn":state.Mode=6;break;
            case "instances.unknown":state.Mode=7;break;
            case "instances.pause":context.RequestPause();break;
            case "instances.resume":context.RequestResume();break;
            case "instances.save":case "instances.load":
                Check(state.SavePending==0,"A native checkpoint operation is already pending.");
                var ticket=context.Action=="instances.save" ? context.RequestSave(Slot) : context.RequestLoad(Slot);
                state.SaveHigh=ticket.EpochHigh;state.SaveLow=ticket.EpochLow;state.SaveSequence=ticket.Sequence;
                state.SavePending=1;state.SaveKind=context.Action=="instances.save" ? 1 : 2;
                break;
            default:throw new InvalidOperationException("Unknown instance fixture action.");
        }
    }
}
