// SPDX-License-Identifier: Apache-2.0
using System.Runtime.InteropServices;
using Poima;
using Poima.Ai;

namespace Poima.BehaviorTreeExample;

[StructLayout(LayoutKind.Sequential)]
public struct GuardState
{
    public long LayoutId;
    public int NextNode;
    public int ActionRunning;
    public int ActionProgress;
    public int LastDecision;
    public int Interruptions;
    public int SightStatus;
}

public enum GuardAction { Observe = 1, Patrol = 2 }

public static class GuardDecision
{
    private const int CanSeeTarget = 0;
    private static readonly BehaviorTree tree = new([
        BehaviorNode.Selector(2),
        BehaviorNode.Sequence(2),
        BehaviorNode.Condition(CanSeeTarget),
        BehaviorNode.Action((int)GuardAction.Observe),
        BehaviorNode.Action((int)GuardAction.Patrol)
    ], revision: 1);

    public static void Tick(ref GuardState state, SightStatus sight, Span<BehaviorTrace> trace)
    {
        if (state.ActionRunning is < 0 or > 1)
            throw new ArgumentException("Invalid saved guard action flag.", nameof(state));
        var cursor = new BehaviorTreeState(state.LayoutId, state.NextNode, state.ActionRunning == 1);
        tree.ValidateState(cursor);
        bool visible = sight == SightStatus.Visible;
        if (visible && cursor.ActionRunning && tree[cursor.ActiveAction].Operand == (int)GuardAction.Patrol)
        {
            // The game cancels its active patrol before selecting another action.
            cursor = default;
            state.ActionProgress = 0;
            ++state.Interruptions;
        }
        var leaves = new GuardLeaves(ref state, visible);
        BehaviorStep step = tree.Step(cursor, nodeBudget: tree.NodeCount, trace, ref leaves);
        state.LayoutId = step.State.LayoutId;
        state.NextNode = step.State.NextNode;
        state.ActionRunning = step.State.ActionRunning ? 1 : 0;
        state.SightStatus = (int)sight;
    }

    private ref struct GuardLeaves(ref GuardState state, bool visible) : IBehaviorTreeLeaves
    {
        private ref GuardState state = ref state;
        private readonly bool visible = visible;

        public bool TestCondition(int key) => key == CanSeeTarget
            ? visible : throw new ArgumentOutOfRangeException(nameof(key));

        public BehaviorStatus StepAction(int key, int node, bool resuming)
        {
            state.LastDecision = key;
            if (key == (int)GuardAction.Observe)
                return BehaviorStatus.Success;
            if (key != (int)GuardAction.Patrol)
                throw new ArgumentOutOfRangeException(nameof(key));
            state.ActionProgress = resuming ? state.ActionProgress + 1 : 1;
            if (state.ActionProgress < 3) return BehaviorStatus.Running;
            state.ActionProgress = 0;
            return BehaviorStatus.Success;
        }
    }
}

[GameModule("poima.example.guard-behavior")]
public sealed class GuardGame : Game<GuardState>
{
    public override void Initialize(ref GuardState state) => state = default;

    public override void Tick(ref GuardState state, GameContext context)
    {
        var target = new EntityId(0, 7);
        var status = SightStatus.OutOfView;
        if (context.IsAlive(target))
        {
            var observer = new SightObserver(new(0, 1.6, 0), new(0, 0, -1), new(0, 1, 0));
            var settings = SightSettings.Cone(0.70, 0.25, 12);
            Span<SightSample> samples = stackalloc SightSample[1];
            var sight = Sight.Evaluate(observer, settings, [context.Get(target).Transform.Position],
                [target], [], rayBudget: 1, samples, context);
            status = sight.Status;
        }
        GuardDecision.Tick(ref state, status, []);
    }
}
