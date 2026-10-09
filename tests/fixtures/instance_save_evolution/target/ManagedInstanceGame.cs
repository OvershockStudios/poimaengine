// SPDX-License-Identifier: Apache-2.0
using Poima;
using System.Numerics;
using System.Runtime.InteropServices;
namespace Poima.Tests;

#if INSTANCE_EVOLUTION_SHRINK
[GameplayBuffer(typeof(EntityId),1)] public partial struct InstanceMembers { }
#else
[GameplayBuffer(typeof(EntityId),4)] public partial struct InstanceMembers { }
#endif
[GameplayBuffer(typeof(int),2)] public partial struct EvolutionNumbers { }
[GameplayComponent("eeeeeeeeeeeeeeeeeeeeeeeeeeee7810")]
public partial struct InstanceLink
{
    [GameplayField("00000000000000000000000000000006")] public EvolutionNumbers Numbers;
    [GameplayField("00000000000000000000000000000004")] public InstanceMembers RetainedMembers;
    [GameplayField("00000000000000000000000000000005",Default="37")] public int Bonus;
    [GameplayField("00000000000000000000000000000003")] public EntityId RetainedRig;
    [GameplayField("00000000000000000000000000000001",Default="999")] public int RetainedValue;
}

[GameModule("poima.test.managed-instances")]
public sealed class ManagedInstanceGame : Game<ManagedInstanceState>,IHierarchicalInstancesGame,ICharacterInputGame
{
    static readonly TemplateId Actor=TemplateId.Parse("eeeeeeeeeeeeeeeeeeeeeeeeeeee7801");
    static readonly TemplateNodeId Root=TemplateNodeId.Parse("eeeeeeeeeeeeeeeeeeeeeeeeeeee7802");
    static readonly TemplateNodeId Rig=TemplateNodeId.Parse("eeeeeeeeeeeeeeeeeeeeeeeeeeee7803");
    static void Check(bool condition,string message) {if(!condition)throw new InvalidOperationException(message);}
    public override void Initialize(ref ManagedInstanceState state)
    {
        if(Environment.GetEnvironmentVariable("POIMA_INSTANCE_UPGRADE_FORBID_INITIALIZE")=="1")
            throw new InvalidOperationException("Initialize forbidden during instance save upgrade.");
        state=default;state.Drive=1;
    }
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
    static EntityId Birth(GameContext context,out EntityId rig)
    {
        var transform=new SpawnTransform(new(-4,0,0),Quaternion.Identity,new(1,1,1));
        var root=context.Spawn(Actor,in transform);rig=context.ResolveNode(root,Rig);
        Check(context.ResolveNode(root,Root)==root && !context.IsAlive(root),"Reserved evolved graph differs.");
        context.SetAnimation(rig,0,speed:1,loop:true,playing:true);
        context.SetCharacterInput(root,0,.35f);
        InstanceLink link=new(){RetainedValue=11,RetainedRig=rig,Bonus=37};
        Check(link.RetainedMembers.TryAdd(root) && link.RetainedMembers.TryAdd(rig) &&
              link.RetainedMembers.TryAdd(root) && link.RetainedMembers.TryAdd(rig),"Evolved buffer capacity differs.");
        context.Set(root,in link);return root;
    }
    static void ValidateAndAdvance(ref ManagedInstanceState state,GameContext context,EntityId root,EntityId rig,int expectedValue,bool first)
    {
        if(root==default)return;
        Check(context.ResolveNode(root,Root)==root && context.ResolveNode(root,Rig)==rig && context.IsAlive(rig),"Preserved live instance map differs.");
        var link=context.Get<InstanceLink>(root);
        Check(link.RetainedRig==rig && link.RetainedValue==expectedValue && link.RetainedMembers.Count>=2 &&
              link.RetainedMembers[0]==root && link.RetainedMembers[1]==rig,"Preserved scalar/array links differ.");
        if(state.EvolutionTicks==0)
        {
            Check(link.RetainedMembers.Count==2 && link.Bonus==37 && link.Numbers.Count==0,
                  "First upgraded Tick lost retained arrays or explicit defaults.");
            Check(link.RetainedMembers.TryAdd(root) && link.RetainedMembers.TryAdd(rig),"Authorized array growth failed.");
        }
        Check(link.RetainedMembers.Count==4 && link.RetainedMembers[2]==root && link.RetainedMembers[3]==rig,
              "Grown entity array differs.");
        if(link.Numbers.Count==0)Check(link.Numbers.TryAdd(10) && link.Numbers.TryAdd(20),"Added empty array is not usable.");
        else {Check(link.Numbers.Count==2,"Added array length differs.");link.Numbers[0]=checked(link.Numbers[0]+1);link.Numbers[1]=checked(link.Numbers[1]+1);}
        ++link.Bonus;context.Set(root,in link);++state.ReferencesChecked;
        var entity=context.Get(root);var playback=context.GetAnimation(rig) ?? throw new InvalidOperationException("Retained rig disappeared.");
        Check(playback.Clip==0 && playback.Loop && playback.Playing && playback.Speed==(first?1:2),"Independent retained playback differs.");
        if(first) {state.FirstX=entity.Transform.Position.X;state.FirstZ=entity.Transform.Position.Z;state.FirstTime=playback.Time;}
        else {state.SecondX=entity.Transform.Position.X;state.SecondZ=entity.Transform.Position.Z;state.SecondTime=playback.Time;}
        if(state.Drive!=0)context.SetCharacterInput(root,first?0:-.2f,first?.35f:0);
    }
    public override void Tick(ref ManagedInstanceState state,GameContext context)
    {
        Reconcile(ref state,context.Saves);
        if(state.Mode==5)
        {
            Check(state.First!=default && state.Second!=default,"Retirement needs two retained instances.");
            context.Despawn(state.First);state.First=default;state.FirstRig=default;state.Mode=0;
        }
        else if(state.Mode==1)
        {
            Check(state.First==default && state.Second!=default,"Rebirth needs one retired instance.");
            state.First=Birth(context,out state.FirstRig);++state.Births;state.Mode=0;
            ++state.SimulationTicks;++state.EvolutionTicks;return;
        }
        else Check(state.Mode==0,"Unsupported evolved fixture mode.");
        ValidateAndAdvance(ref state,context,state.First,state.FirstRig,11,true);
        ValidateAndAdvance(ref state,context,state.Second,state.SecondRig,22,false);
        ++state.SimulationTicks;++state.EvolutionTicks;
    }
    public override void Control(ref ManagedInstanceState state,ControlContext context)
    {
        ++state.ControlCalls;Reconcile(ref state,context.Saves);
        switch(context.Action)
        {
            case "instances.inspect":
                if(state.First!=default)Check(context.ResolveNode(state.First,Rig)==state.FirstRig,"Control first map differs.");
                if(state.Second!=default)Check(context.ResolveNode(state.Second,Rig)==state.SecondRig,"Control second map differs.");
                ++state.ResolvedControls;break;
            case "instances.repair":state.Mode=5;break;
            case "instances.birth":state.Mode=1;break;
            case "instances.pause":context.RequestPause();break;
            case "instances.resume":context.RequestResume();break;
            default:throw new InvalidOperationException("Unsupported evolved fixture control.");
        }
    }
}

[GameplayPersistence(1)]
[StructLayout(LayoutKind.Sequential)]
public struct ManagedInstanceState
{
    [GameplayField("a790000000000000000000000000001c")] public int SimulationTicks;
    [GameplayField("a790000000000000000000000000001b")] public double SecondZ;
    [GameplayField("a790000000000000000000000000001a")] public double SecondX;
    [GameplayField("a7900000000000000000000000000019")] public double SecondTime;
    [GameplayField("a7900000000000000000000000000018")] public EntityId SecondRig;
    [GameplayField("a7900000000000000000000000000017")] public EntityId Second;
    [GameplayField("a7900000000000000000000000000016")] public long SaveSequence;
    [GameplayField("a7900000000000000000000000000015")] public int SavePending;
    [GameplayField("a7900000000000000000000000000014")] public long SaveLow;
    [GameplayField("a7900000000000000000000000000013")] public int SaveKind;
    [GameplayField("a7900000000000000000000000000012")] public long SaveHigh;
    [GameplayField("a7900000000000000000000000000011")] public int ResolvedControls;
    [GameplayField("a7900000000000000000000000000010")] public int Rejected;
    [GameplayField("a790000000000000000000000000000f")] public long ObservedEpochLow;
    [GameplayField("a790000000000000000000000000000e")] public long ObservedEpochHigh;
    [GameplayField("a790000000000000000000000000000d")] public int Mode;
    [GameplayField("a790000000000000000000000000000c")] public int LastSaveState;
    [GameplayField("a790000000000000000000000000000b")] public long LastSaveGeneration;
    [GameplayField("a790000000000000000000000000000a")] public int LastSaveError;
    [GameplayField("a7900000000000000000000000000009")] public double FirstZ;
    [GameplayField("a7900000000000000000000000000008")] public double FirstX;
    [GameplayField("a7900000000000000000000000000007")] public double FirstTime;
    [GameplayField("a7900000000000000000000000000006")] public EntityId FirstRig;
    [GameplayField("a7900000000000000000000000000005")] public EntityId First;
    [GameplayField("a7900000000000000000000000000004")] public int Drive;
    [GameplayField("a7900000000000000000000000000003")] public int ControlCalls;
    [GameplayField("a7900000000000000000000000000002")] public int Canceled;
    [GameplayField("a7900000000000000000000000000001")] public int Births;
    [GameplayField("a7900000000000000000000000000080")] public int EvolutionTicks;
    [GameplayField("a7900000000000000000000000000081")] public int ReferencesChecked;
}
