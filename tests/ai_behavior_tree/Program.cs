// SPDX-License-Identifier: Apache-2.0
using Poima.Ai;
using Poima.BehaviorTreeExample;
using static Poima.Ai.BehaviorStatus;
using static Poima.Ai.BehaviorTraceEvent;

internal static class Program
{
    private static int checks;

    private static void Main()
    {
        LayoutAndValidation();
        OrderedTraversal();
        MemoryAndReplay();
        BudgetsAndTraces();
        GuardExample();
        NoSteadyStateAllocations();
        Console.WriteLine($"Behavior tree passed {checks} managed checks.");
    }

    private static void Require(bool condition, string label)
    {
        ++checks;
        if (!condition) throw new InvalidOperationException(label);
    }

    private static void Reject<TException>(Action action) where TException : Exception
    {
        try { action(); }
        catch (TException) { ++checks; return; }
        throw new InvalidOperationException($"Expected {typeof(TException).Name}.");
    }

    private struct Tape : IBehaviorTreeLeaves
    {
        public bool Visible;
        public BehaviorStatus Start, Resume, Fallback;
        public readonly List<(int Node, int Key, bool Resuming)> Calls = [];

        public Tape(bool visible, BehaviorStatus start, BehaviorStatus resume, BehaviorStatus fallback)
            => (Visible, Start, Resume, Fallback) = (visible, start, resume, fallback);

        public bool TestCondition(int key)
        {
            Calls.Add((-1, key, false));
            return key == 0 && Visible;
        }

        public BehaviorStatus StepAction(int key, int node, bool resuming)
        {
            Calls.Add((node, key, resuming));
            return key == 0 ? (resuming ? Resume : Start) : Fallback;
        }
    }

    private static BehaviorTree GuardTree() => new([
        BehaviorNode.Selector(2), BehaviorNode.Sequence(2), BehaviorNode.Condition(0),
        BehaviorNode.Action(0), BehaviorNode.Action(1)
    ]);

    private static void LayoutAndValidation()
    {
        BehaviorNode[] source = [BehaviorNode.Sequence(2), BehaviorNode.Action(0), BehaviorNode.Condition(0)];
        var tree = new BehaviorTree(source, revision: 42);
        Require(tree.NodeCount == 3, "tree length");
        Require(tree.LayoutId is > 0 and <= 9007199254740991L, "fingerprint is safe for JSON numeric clients");
        Require(tree.LayoutId == new BehaviorTree(source, revision: 42).LayoutId, "stable layout fingerprint");
        Require(tree.LayoutId != new BehaviorTree(source, revision: 43).LayoutId, "revision changes the layout guard");
        source[1] = BehaviorNode.Action(1);
        Require(tree[1] == BehaviorNode.Action(0), "compiled definition is immutable");
        Require(tree.LayoutId != new BehaviorTree(source, revision: 42).LayoutId, "leaf key changes the layout guard");

        Reject<ArgumentException>(() => _ = new BehaviorTree([BehaviorNode.Sequence(1)]));
        Reject<ArgumentException>(() => _ = new BehaviorTree([BehaviorNode.Sequence(0)]));
        Reject<ArgumentException>(() => _ = new BehaviorTree([BehaviorNode.Sequence(int.MaxValue)]));
        Reject<ArgumentException>(() => _ = new BehaviorTree([BehaviorNode.Action(0), BehaviorNode.Condition(0)]));
        Reject<ArgumentException>(() => _ = new BehaviorTree([BehaviorNode.Action(-1)]));
        Reject<ArgumentException>(() => _ = new BehaviorTree([default]));
        Reject<ArgumentOutOfRangeException>(() => _ = new BehaviorTree([]));
        Reject<ArgumentOutOfRangeException>(() => _ = new BehaviorTree(new BehaviorNode[BehaviorTree.MaximumNodes + 1]));
        Reject<ArgumentOutOfRangeException>(() => _ = new BehaviorTree([BehaviorNode.Action(0)], revision: -1));

        var depth = new BehaviorNode[BehaviorTree.MaximumDepth + 1];
        Array.Fill(depth, BehaviorNode.Sequence(1));
        depth[^1] = BehaviorNode.Action(0);
        Reject<ArgumentException>(() => _ = new BehaviorTree(depth));
        depth[^2] = BehaviorNode.Action(0);
        var deepest = new BehaviorTree(depth.AsSpan(0, BehaviorTree.MaximumDepth));
        var provider = new Tape(true, BehaviorStatus.Success, BehaviorStatus.Success, BehaviorStatus.Success);
        var trace = new BehaviorTrace[BehaviorTree.MaximumTraceEntries];
        BehaviorStep result = deepest.Step(default, BehaviorTree.MaximumDepth, trace, ref provider);
        Require(result.NodeVisits == BehaviorTree.MaximumDepth && result.TraceWritten == 2 * BehaviorTree.MaximumDepth,
            "maximum valid depth resolves every composite");

        Reject<ArgumentException>(() => tree.ValidateState(new(tree.LayoutId, 0, false)));
        Reject<ArgumentException>(() => tree.ValidateState(new(tree.LayoutId, 1, false)));
        Reject<ArgumentException>(() => tree.ValidateState(new(tree.LayoutId, 3, true)));
        Reject<ArgumentException>(() => tree.ValidateState(new(tree.LayoutId, 4, false)));
        Reject<ArgumentException>(() => tree.ValidateState(new(0, 2, true)));
        Reject<ArgumentOutOfRangeException>(() => tree.Step(default, -1, [], ref provider));
        Reject<ArgumentOutOfRangeException>(() => tree.Step(default, BehaviorTree.MaximumNodes + 1, [], ref provider));
    }

    private static void OrderedTraversal()
    {
        var tree = GuardTree();
        Span<BehaviorTrace> trace = stackalloc BehaviorTrace[16];
        var visible = new Tape(true, BehaviorStatus.Success, BehaviorStatus.Success, BehaviorStatus.Failure);
        var hidden = new Tape(false, BehaviorStatus.Success, BehaviorStatus.Success, BehaviorStatus.Failure);
        BehaviorStep a = tree.Step(default, 16, trace, ref visible);
        Require(a.Status == BehaviorStatus.Success && a.State == default && a.NodeVisits == 4,
            "a successful branch short-circuits the selector");
        Require(visible.Calls.SequenceEqual([(-1, 0, false), (3, 0, false)]), "first branch condition and action order");
        Require(trace[..a.TraceWritten].SequenceEqual([
            new(0, Enter), new(1, Enter), new(2, Enter), new(2, BehaviorTraceEvent.Success), new(3, Enter),
            new(3, BehaviorTraceEvent.Success), new(1, BehaviorTraceEvent.Success), new(0, BehaviorTraceEvent.Success)
        ]), "complete success trace");
        BehaviorStep b = tree.Step(default, 16, trace, ref hidden);
        Require(b.Status == BehaviorStatus.Failure && b.State == default && b.NodeVisits == 4,
            "exhausted selector follows failure from its last child");
        Require(hidden.Calls.SequenceEqual([(-1, 0, false), (4, 1, false)]), "failed condition skips its subtree");
        Require(trace[..b.TraceWritten].SequenceEqual([
            new(0, Enter), new(1, Enter), new(2, Enter), new(2, BehaviorTraceEvent.Failure),
            new(1, BehaviorTraceEvent.Failure), new(4, Enter), new(4, BehaviorTraceEvent.Failure), new(0, BehaviorTraceEvent.Failure)
        ]), "complete failure trace");
    }

    private static void MemoryAndReplay()
    {
        var tree = GuardTree();
        var a = new Tape(true, BehaviorStatus.Running, BehaviorStatus.Failure, BehaviorStatus.Success);
        var b = new Tape(false, BehaviorStatus.Running, BehaviorStatus.Failure, BehaviorStatus.Running);
        BehaviorStep first = tree.Step(default, 10, [], ref a);
        Require(first.Status == BehaviorStatus.Running && first.State.NextNode == 4 && first.State.ActiveAction == 3,
            "running leaf retained by its row");
        BehaviorStep other = tree.Step(default, 10, [], ref b);
        Require(other.Status == BehaviorStatus.Running && other.State.NextNode == 5 && other.State.ActiveAction == 4,
            "independent NPC cursor with a shared definition");

        // Emulate a save from supported scalar fields, then resume a freshly compiled tree.
        var saved = new BehaviorTreeState(first.State.LayoutId, first.State.NextNode, first.State.ActionRunning);
        var rebuilt = GuardTree();
        rebuilt.ValidateState(saved);
        var replay = new Tape(false, BehaviorStatus.Failure, BehaviorStatus.Failure, BehaviorStatus.Success);
        a.Calls.Clear();
        a.Visible = false;
        var trace = new BehaviorTrace[12];
        var replayTrace = new BehaviorTrace[trace.Length];
        BehaviorStep live = tree.Step(saved, 10, trace, ref a);
        BehaviorStep restored = rebuilt.Step(saved, 10, replayTrace, ref replay);
        Require(live == restored && trace.AsSpan().SequenceEqual(replayTrace), "resumed decision and trace are reproducible");
        Require(live.Status == BehaviorStatus.Success && live.NodeVisits == 2, "failed active child resumes selector fallback");
        Require(a.Calls.SequenceEqual([(3, 0, true), (4, 1, false)]) && a.Calls.SequenceEqual(replay.Calls),
            "ancestors and previous guard do not rerun when an action resumes");
        Require(trace[0] == new BehaviorTrace(3, BehaviorTraceEvent.Resume), "trace identifies a resumed action");
        Require(other.State.ActionRunning && b.Calls.SequenceEqual([(-1, 0, false), (4, 1, false)]),
            "the other NPC's decision remains independent");

        var revised = new BehaviorTree([BehaviorNode.Action(0)]);
        var sentinel = new BehaviorTrace(99, BehaviorTraceEvent.Enter);
        Array.Fill(trace, sentinel);
        a.Calls.Clear();
        Reject<ArgumentException>(() => revised.Step(saved, 10, trace, ref a));
        Require(a.Calls.Count == 0 && trace.All(entry => entry == sentinel), "incompatible layout rejected before callbacks");
        a.Start = BehaviorStatus.BudgetExhausted;
        a.Visible = true;
        Reject<InvalidOperationException>(() => tree.Step(default, 10, trace, ref a));
        Require(trace.All(entry => entry == sentinel), "invalid provider status does not publish a partial trace");
        a.Start = default;
        Reject<InvalidOperationException>(() => tree.Step(default, 10, trace, ref a));

        var root = new BehaviorTree([BehaviorNode.Action(0)]);
        a.Start = BehaviorStatus.Running;
        BehaviorStep rootRunning = root.Step(default, 1, trace, ref a);
        root.ValidateState(rootRunning.State);
        Require(rootRunning.State.ActiveAction == 0 && root.Step(rootRunning.State, 1, trace, ref a).Status == BehaviorStatus.Failure,
            "single-action root can run and resume");
    }

    private static void BudgetsAndTraces()
    {
        var tree = new BehaviorTree([
            BehaviorNode.Sequence(2), BehaviorNode.Condition(0), BehaviorNode.Selector(2),
            BehaviorNode.Condition(1), BehaviorNode.Action(1)
        ]);
        var whole = new Tape(true, BehaviorStatus.Success, BehaviorStatus.Success, BehaviorStatus.Success);
        var uninterruptedTrace = new BehaviorTrace[16];
        BehaviorStep full = tree.Step(default, tree.NodeCount, uninterruptedTrace, ref whole);
        Require(full.Status == BehaviorStatus.Success && full.TraceWritten == 10, "terminal cascade outside visit budget");

        for (int budget = 1; budget <= tree.NodeCount; ++budget)
        {
            var splitTrace = new List<BehaviorTrace>();
            var partial = new Tape(true, BehaviorStatus.Success, BehaviorStatus.Success, BehaviorStatus.Success);
            BehaviorTreeState cursor = default;
            var trace = new BehaviorTrace[16];
            int visits = 0;
            for (int i = 0; i < tree.NodeCount; ++i)
            {
                BehaviorStep next = tree.Step(cursor, budget, trace, ref partial);
                Require(next.NodeVisits <= budget, "budget limits incoming node visits");
                visits += next.NodeVisits;
                splitTrace.AddRange(trace[..next.TraceWritten]);
                cursor = next.State;
                if (next.Status == BehaviorStatus.Success) break;
                Require(next.Status == BehaviorStatus.BudgetExhausted && !cursor.ActionRunning && cursor.NextNode > 1,
                    "budget exhaustion identifies the queued subtree");
                Require(tree.Step(cursor, 0, [], ref partial) == new BehaviorStep(BudgetExhausted, cursor, 0, 0, 0),
                    "zero visits preserve an existing continuation");
            }
            Require(visits == full.NodeVisits && cursor == default, "slicing visits all nodes once");
            Require(splitTrace.SequenceEqual(uninterruptedTrace[..full.TraceWritten]) && partial.Calls.SequenceEqual(whole.Calls),
                "budget split does not change ordered calls and trace events");
        }

        Span<BehaviorTrace> shortTrace = stackalloc BehaviorTrace[3];
        BehaviorStep shortStep = tree.Step(default, 16, shortTrace, ref whole);
        Require(shortStep.Status == full.Status && shortStep.TraceWritten == 3 && shortStep.TraceDropped == 7,
            "a short span reports the omitted suffix");
        Require(shortTrace.SequenceEqual(uninterruptedTrace.AsSpan(0, 3)), "short span receives the exact prefix");
        BehaviorStep noTrace = tree.Step(default, 16, [], ref whole);
        Require(noTrace.TraceWritten == 0 && noTrace.TraceDropped == full.TraceWritten, "empty span still counts omitted events");

        var wide = new BehaviorNode[BehaviorTree.MaximumNodes];
        Array.Fill(wide, BehaviorNode.Condition(0));
        wide[0] = BehaviorNode.Sequence(wide.Length - 1);
        var maximum = new BehaviorTree(wide);
        var maximumTrace = new BehaviorTrace[BehaviorTree.MaximumTraceEntries];
        BehaviorStep all = maximum.Step(default, BehaviorTree.MaximumNodes, maximumTrace, ref whole);
        Require(all.Status == BehaviorStatus.Success && all.NodeVisits == maximum.NodeCount
            && all.TraceWritten == maximumTrace.Length && all.TraceDropped == 0, "maximum complete trace bound");
    }

    private static void GuardExample()
    {
        GuardState state = default;
        Span<BehaviorTrace> trace = stackalloc BehaviorTrace[10];
        GuardDecision.Tick(ref state, SightStatus.OutOfView, trace);
        Require(state.LastDecision == (int)GuardAction.Patrol && state.ActionRunning == 1 && state.ActionProgress == 1,
            "a guard without sight begins patrol");
        GuardState restored = state;
        GuardDecision.Tick(ref state, SightStatus.Occluded, trace);
        GuardDecision.Tick(ref restored, SightStatus.Occluded, trace);
        Require(state.Equals(restored) && state.ActionProgress == 2, "saved guard progress resumes on the next tick");
        GuardDecision.Tick(ref state, SightStatus.Visible, trace);
        Require(state.LastDecision == (int)GuardAction.Observe && state.ActionRunning == 0 && state.ActionProgress == 0
            && state.Interruptions == 1, "new sight explicitly cancels patrol and selects Observe");
        Require(state.SightStatus == (int)SightStatus.Visible, "the current observation is available for diagnostics");
    }

    private struct QuietLeaves : IBehaviorTreeLeaves
    {
        public int Calls;
        public bool TestCondition(int key) => true;
        public BehaviorStatus StepAction(int key, int node, bool resuming)
        {
            ++Calls;
            return resuming ? BehaviorStatus.Success : BehaviorStatus.Running;
        }
    }

    private static void NoSteadyStateAllocations()
    {
        var tree = GuardTree();
        var leaves = new QuietLeaves();
        Span<BehaviorTrace> trace = stackalloc BehaviorTrace[8];
        BehaviorTreeState cursor = default;
        for (int i = 0; i < 500; ++i)
            cursor = tree.Step(cursor, 8, trace, ref leaves).State;
        long before = GC.GetAllocatedBytesForCurrentThread();
        for (int i = 0; i < 1000; ++i)
            cursor = tree.Step(cursor, 8, trace, ref leaves).State;
        long bytes = GC.GetAllocatedBytesForCurrentThread() - before;
        Require(bytes == 0 && leaves.Calls == 1500 && cursor == default, "steady-state steps allocate zero bytes");
    }
}
