// SPDX-License-Identifier: Apache-2.0
using System.Buffers.Binary;
using System.Security.Cryptography;

namespace Poima.Ai;

public enum BehaviorNodeKind : byte { Sequence = 1, Selector, Condition, Action }

/// <summary>One preorder row. Operand is the direct child count for a composite, or a leaf key for game code.</summary>
public readonly record struct BehaviorNode(BehaviorNodeKind Kind, int Operand)
{
    public static BehaviorNode Sequence(int children) => new(BehaviorNodeKind.Sequence, children);
    public static BehaviorNode Selector(int children) => new(BehaviorNodeKind.Selector, children);
    public static BehaviorNode Condition(int key) => new(BehaviorNodeKind.Condition, key);
    public static BehaviorNode Action(int key) => new(BehaviorNodeKind.Action, key);
}

public enum BehaviorStatus : byte { Success = 1, Failure, Running, BudgetExhausted }
public enum BehaviorTraceEvent : byte { Enter = 1, Resume, Success, Failure, Running }

public readonly record struct BehaviorTrace(int Node, BehaviorTraceEvent Event);

/// <summary>One NPC's continuation. NextNode is zero for a fresh tree, otherwise a one-based node index.</summary>
public readonly record struct BehaviorTreeState(long LayoutId, int NextNode, bool ActionRunning)
{
    /// <summary>The zero-based active action row, or −1 if no action is running.</summary>
    public int ActiveAction => ActionRunning ? NextNode - 1 : -1;
}

public readonly record struct BehaviorStep(
    BehaviorStatus Status, BehaviorTreeState State, int NodeVisits, int TraceWritten, int TraceDropped);

/// <summary>Deterministic, game-owned leaves. Conditions should be read-only. Actions may return Success, Failure or Running.</summary>
public interface IBehaviorTreeLeaves
{
    bool TestCondition(int key);
    BehaviorStatus StepAction(int key, int node, bool resuming);
}

/// <summary>Immutable, ordered behavior tree with memory composites and bounded, allocation-free stepping.</summary>
public sealed class BehaviorTree
{
    public const int MaximumNodes = 256;
    public const int MaximumDepth = 32;
    public const int MaximumTraceEntries = MaximumNodes * 2;

    private readonly record struct CompiledNode(BehaviorNode Node, int Parent, int End);
    private readonly CompiledNode[] nodes;

    /// <summary>Compile and copy a single rooted preorder tree. Change revision when leaf meanings change.</summary>
    public BehaviorTree(ReadOnlySpan<BehaviorNode> definition, long revision = 0)
    {
        if (definition.Length is < 1 or > MaximumNodes)
            throw new ArgumentOutOfRangeException(nameof(definition), $"Behavior trees require 1..{MaximumNodes} nodes.");
        if (revision < 0)
            throw new ArgumentOutOfRangeException(nameof(revision));
        nodes = new CompiledNode[definition.Length];
        if (Compile(definition, 0, -1, 1) != definition.Length)
            throw new ArgumentException("The definition must contain exactly one rooted tree.", nameof(definition));
        LayoutId = Fingerprint(nodes, revision);
    }

    public int NodeCount => nodes.Length;
    public long LayoutId { get; }
    public BehaviorNode this[int node] => nodes[node].Node;

    private int Compile(ReadOnlySpan<BehaviorNode> definition, int index, int parent, int depth)
    {
        if (index >= nodes.Length)
            throw new ArgumentException("A composite has fewer children than declared.", nameof(definition));
        if (depth > MaximumDepth)
            throw new ArgumentException($"The tree exceeds {MaximumDepth} levels.", nameof(definition));
        var node = definition[index];
        int end = index + 1;
        switch (node.Kind)
        {
            case BehaviorNodeKind.Sequence:
            case BehaviorNodeKind.Selector:
                if (node.Operand < 1 || node.Operand > nodes.Length - end)
                    throw new ArgumentException($"Invalid child count at row {index}.", nameof(definition));
                for (int child = 0; child < node.Operand; ++child)
                    end = Compile(definition, end, index, depth + 1);
                break;
            case BehaviorNodeKind.Condition:
            case BehaviorNodeKind.Action:
                if (node.Operand < 0)
                    throw new ArgumentException($"Negative leaf key at row {index}.", nameof(definition));
                break;
            default:
                throw new ArgumentException($"Unknown node kind at row {index}.", nameof(definition));
        }
        nodes[index] = new(node, parent, end);
        return end;
    }

    private static long Fingerprint(ReadOnlySpan<CompiledNode> definition, long revision)
    {
        // Canonical layout domain, revision and preorder rows. This identifies a saved cursor's layout.
        Span<byte> bytes = stackalloc byte[12 + definition.Length * 5];
        "BT01"u8.CopyTo(bytes);
        BinaryPrimitives.WriteInt64LittleEndian(bytes[4..], revision);
        int offset = 12;
        foreach (CompiledNode row in definition)
        {
            BehaviorNode node = row.Node;
            bytes[offset++] = (byte)node.Kind;
            BinaryPrimitives.WriteInt32LittleEndian(bytes[offset..], node.Operand);
            offset += sizeof(int);
        }
        Span<byte> digest = stackalloc byte[32];
        SHA256.HashData(bytes, digest);
        long id = BinaryPrimitives.ReadInt64LittleEndian(digest) & ((1L << 53) - 1);
        return id == 0 ? 1 : id;
    }

    /// <summary>Reject a corrupt cursor or one created for another tree before invoking its leaves.</summary>
    public void ValidateState(BehaviorTreeState state)
    {
        if (state == default) return;
        if (state.LayoutId != LayoutId || state.NextNode < 1 || state.NextNode > nodes.Length)
            throw new ArgumentException("The cursor does not belong to this tree layout.", nameof(state));
        if (state.ActionRunning ? nodes[state.NextNode - 1].Node.Kind != BehaviorNodeKind.Action : state.NextNode == 1)
            throw new ArgumentException("The cursor cannot resume at this node.", nameof(state));
    }

    /// <summary>Run at most nodeBudget node entries or resumes. Supply an empty trace span when tracing is disabled.</summary>
    public BehaviorStep Step<TLeaves>(BehaviorTreeState state, int nodeBudget, scoped Span<BehaviorTrace> trace,
        scoped ref TLeaves leaves) where TLeaves : IBehaviorTreeLeaves, allows ref struct
    {
        if (nodeBudget is < 0 or > MaximumNodes)
            throw new ArgumentOutOfRangeException(nameof(nodeBudget));
        ValidateState(state);
        if (nodeBudget == 0) return new(BehaviorStatus.BudgetExhausted, state, 0, 0, 0);

        // Publish the trace only after a completed step. Guards and provider exceptions leave the caller's span intact.
        Span<BehaviorTrace> pending = stackalloc BehaviorTrace[Math.Min(trace.Length, MaximumTraceEntries)];
        int traceCount = 0;
        int next = state.NextNode == 0 ? 0 : state.NextNode - 1;
        bool resuming = state.ActionRunning;
        for (int visits = 1; visits <= nodeBudget; ++visits)
        {
            int index = next;
            var current = nodes[index];
            Record(pending, ref traceCount, index, resuming ? BehaviorTraceEvent.Resume : BehaviorTraceEvent.Enter);
            if (current.Node.Kind is BehaviorNodeKind.Selector or BehaviorNodeKind.Sequence)
            {
                next = index + 1;
                continue;
            }

            var status = current.Node.Kind == BehaviorNodeKind.Condition
                ? (leaves.TestCondition(current.Node.Operand) ? BehaviorStatus.Success : BehaviorStatus.Failure)
                : leaves.StepAction(current.Node.Operand, index, resuming);
            resuming = false;
            if (status == BehaviorStatus.Running)
            {
                Record(pending, ref traceCount, index, BehaviorTraceEvent.Running);
                return Finish(BehaviorStatus.Running, new(LayoutId, index + 1, true), visits, pending, trace, traceCount);
            }
            var outcome = status switch
            {
                BehaviorStatus.Success => BehaviorTraceEvent.Success,
                BehaviorStatus.Failure => BehaviorTraceEvent.Failure,
                _ => throw new InvalidOperationException($"Action node {index} returned an invalid status.")
            };

            while (true)
            {
                Record(pending, ref traceCount, index, outcome);
                if (current.Parent < 0)
                    return Finish(status, default, visits, pending, trace, traceCount);

                var parent = nodes[current.Parent];
                bool tryNext = parent.Node.Kind == BehaviorNodeKind.Sequence
                    ? status == BehaviorStatus.Success
                    : status == BehaviorStatus.Failure;
                if (tryNext && current.End < parent.End)
                {
                    next = current.End;
                    break;
                }
                index = current.Parent;
                current = parent;
            }
        }

        return Finish(BehaviorStatus.BudgetExhausted, new(LayoutId, next + 1, false), nodeBudget, pending, trace, traceCount);
    }

    private static void Record(Span<BehaviorTrace> trace, ref int count, int index, BehaviorTraceEvent outcome)
    {
        if (count < trace.Length) trace[count] = new(index, outcome);
        ++count;
    }

    private static BehaviorStep Finish(BehaviorStatus status, BehaviorTreeState state, int visits,
        ReadOnlySpan<BehaviorTrace> pending, Span<BehaviorTrace> trace, int count)
    {
        int written = Math.Min(count, pending.Length);
        pending[..written].CopyTo(trace);
        return new(status, state, visits, written, count - written);
    }
}
