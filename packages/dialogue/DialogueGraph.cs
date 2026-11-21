// SPDX-License-Identifier: Apache-2.0
namespace Poima.Dialogue;

public readonly record struct DialogueChoice(int Id, int NextNode);

public sealed class DialogueNode
{
    public const int MaxChoices = 8;
    public const uint MaxDelayTicks = 216_000;

    private readonly DialogueChoice[] choices = [];

    public int Id { get; }
    public int NextNode { get; }
    public uint DelayTicks { get; }
    public ReadOnlySpan<DialogueChoice> Choices => choices;

    public DialogueNode(int id, int nextNode, uint delayTicks = 0)
    {
        if (id <= 0)
            throw new ArgumentOutOfRangeException(nameof(id), "Dialogue node IDs must be positive.");
        if (nextNode < DialogueGraph.End)
            throw new ArgumentOutOfRangeException(nameof(nextNode), "Use a positive node ID or DialogueGraph.End.");
        if (delayTicks > MaxDelayTicks)
            throw new ArgumentOutOfRangeException(nameof(delayTicks));

        Id = id;
        NextNode = nextNode;
        DelayTicks = delayTicks;
    }

    public DialogueNode(int id, ReadOnlySpan<DialogueChoice> choices, uint delayTicks = 0)
        : this(id, DialogueGraph.End, delayTicks)
    {
        if (choices.Length is < 1 or > MaxChoices)
            throw new ArgumentOutOfRangeException(nameof(choices), "A branching node requires one to eight choices.");
        for (var i = 0; i < choices.Length; ++i)
        {
            var choice = choices[i];
            if (choice.Id <= 0 || choice.NextNode < DialogueGraph.End)
                throw new ArgumentException("Choice IDs must be positive and destinations must be a node ID or End.", nameof(choices));
            for (var j = 0; j < i; ++j)
                if (choices[j].Id == choice.Id)
                    throw new ArgumentException("Choice IDs must be unique within their node.", nameof(choices));
        }

        this.choices = choices.ToArray();
    }
}

public readonly record struct DialogueState(int Node, int Revision, long ChangedTick);

public enum DialogueStatus
{
    InvalidState = 0,
    InvalidTick = 1,
    StaleRevision = 2,
    AlreadyTransitioned = 3,
    RevisionExhausted = 4,
    AlreadyActive = 5,
    Inactive = 6,
    UnknownNode = 7,
    NotReady = 8,
    ChoiceRequired = 9,
    UnknownChoice = 10,
    Started = 11,
    Advanced = 12,
    Selected = 13,
    Ended = 14,
    Cancelled = 15,
}

public readonly record struct DialogueResult(DialogueStatus Status, DialogueState State)
{
    public bool Accepted => Status is >= DialogueStatus.Started and <= DialogueStatus.Cancelled;
}

public sealed class DialogueGraph
{
    public const int End = 0;
    public const int MaxNodes = 128;

    private enum Command { Start, Advance, Select, Cancel }
    private readonly Dictionary<int, DialogueNode> nodes;

    public DialogueGraph(ReadOnlySpan<DialogueNode> nodes)
    {
        if (nodes.Length is < 1 or > MaxNodes)
            throw new ArgumentOutOfRangeException(nameof(nodes), "A dialogue requires one to 128 nodes.");
        this.nodes = new(nodes.Length);
        foreach (var node in nodes)
            if (node is null || !this.nodes.TryAdd(node.Id, node))
                throw new ArgumentException("Dialogue nodes must be present and have unique IDs.", nameof(nodes));

        foreach (var node in nodes)
        {
            if (node.NextNode != End && FindNode(node.NextNode) is null)
                throw new ArgumentException($"Dialogue node {node.Id} points to missing node {node.NextNode}.", nameof(nodes));
            foreach (var choice in node.Choices)
                if (choice.NextNode != End && FindNode(choice.NextNode) is null)
                    throw new ArgumentException($"Choice {choice.Id} in node {node.Id} points to missing node {choice.NextNode}.", nameof(nodes));
        }
    }

    public DialogueNode? FindNode(int id) => nodes.GetValueOrDefault(id);

    public DialogueResult Start(DialogueState state, int node, ulong tick, int expectedRevision) =>
        Apply(state, tick, expectedRevision, Command.Start, node);

    public DialogueResult Advance(DialogueState state, ulong tick, int expectedRevision) =>
        Apply(state, tick, expectedRevision, Command.Advance, 0);

    public DialogueResult Select(DialogueState state, int choice, ulong tick, int expectedRevision) =>
        Apply(state, tick, expectedRevision, Command.Select, choice);

    public DialogueResult Cancel(DialogueState state, ulong tick, int expectedRevision) =>
        Apply(state, tick, expectedRevision, Command.Cancel, 0);

    private DialogueResult Apply(DialogueState state, ulong tick, int expectedRevision, Command command, int argument)
    {
        var current = FindNode(state.Node);
        if (state.Revision < 0 || state.ChangedTick < 0 || (state.Revision == 0 && state != default)
            || (state.Node != End && current is null))
            return new(DialogueStatus.InvalidState, state);
        if (tick > long.MaxValue || tick < (ulong)state.ChangedTick)
            return new(DialogueStatus.InvalidTick, state);
        if (expectedRevision != state.Revision)
            return new(DialogueStatus.StaleRevision, state);
        if (state.Revision != 0 && tick == (ulong)state.ChangedTick)
            return new(DialogueStatus.AlreadyTransitioned, state);
        if (state.Revision == int.MaxValue)
            return new(DialogueStatus.RevisionExhausted, state);

        if (command == Command.Start)
        {
            if (current is not null)
                return new(DialogueStatus.AlreadyActive, state);
            if (FindNode(argument) is null)
                return new(DialogueStatus.UnknownNode, state);
            return Move(state, tick, argument, DialogueStatus.Started);
        }

        if (current is null)
            return new(DialogueStatus.Inactive, state);
        if (command == Command.Cancel)
            return Move(state, tick, End, DialogueStatus.Cancelled);
        if (tick - (ulong)state.ChangedTick < current.DelayTicks)
            return new(DialogueStatus.NotReady, state);

        if (command == Command.Advance)
            return current.Choices.IsEmpty
                ? Move(state, tick, current.NextNode, DialogueStatus.Advanced)
                : new(DialogueStatus.ChoiceRequired, state);
        foreach (var choice in current.Choices)
            if (choice.Id == argument)
                return Move(state, tick, choice.NextNode, DialogueStatus.Selected);
        return new(DialogueStatus.UnknownChoice, state);
    }

    private static DialogueResult Move(DialogueState state, ulong tick, int destination, DialogueStatus accepted)
    {
        var status = destination == End && accepted != DialogueStatus.Cancelled ? DialogueStatus.Ended : accepted;
        return new(status, new(destination, state.Revision + 1, (long)tick));
    }
}
