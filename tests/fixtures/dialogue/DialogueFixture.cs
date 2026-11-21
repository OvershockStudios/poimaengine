// SPDX-License-Identifier: Apache-2.0
using Poima;
using Poima.Dialogue;

namespace Poima.Tests;

public struct DialogueProbeState
{
    public int Node, Revision, LastStatus, FailControl;
    public long ChangedTick, FailTick;
}

[GameModule("poima.test.dialogue")]
public sealed class DialogueProbeGame : Game<DialogueProbeState>
{
    private static readonly UiId Line = new(0, 2);
    private static readonly DialogueGraph Graph = new([
        new DialogueNode(10, [new DialogueChoice(101, 20), new DialogueChoice(102, 30)], delayTicks: 1),
        new DialogueNode(20, [new DialogueChoice(103, 40)], delayTicks: 2),
        new DialogueNode(30, DialogueGraph.End, delayTicks: 3),
        new DialogueNode(40, DialogueGraph.End, delayTicks: 2),
    ]);

    public override void Initialize(ref DialogueProbeState state) => state.FailTick = -1;

    private static DialogueState Read(in DialogueProbeState state) => new(state.Node, state.Revision, state.ChangedTick);

    private static void Apply(ref DialogueProbeState state, DialogueResult result)
    {
        state.LastStatus = (int)result.Status;
        if (!result.Accepted)
            return;
        state.Node = result.State.Node;
        state.Revision = result.State.Revision;
        state.ChangedTick = result.State.ChangedTick;
    }

    private static string Text(int node) => node switch
    {
        10 => "Keeper: Do you want the north path or the ferry?",
        20 => "Keeper: Take this lantern for the north path.",
        30 => "Keeper: The ferry leaves at dawn.",
        40 => "Keeper: Follow the lanterns along the ridge.",
        DialogueGraph.End => "The keeper is available.",
        _ => throw new InvalidOperationException("Unrecognized dialogue branch."),
    };

    public override void Control(ref DialogueProbeState state, ControlContext context)
    {
        var prior = Read(state);
        var result = context.Action switch
        {
            "begin" => Graph.Start(prior, 10, context.Tick, prior.Revision),
            "north" => Graph.Select(prior, 101, context.Tick, prior.Revision),
            "ferry" => Graph.Select(prior, 102, context.Tick, prior.Revision),
            "lantern" => Graph.Select(prior, 103, context.Tick, prior.Revision),
            "unknown" => Graph.Select(prior, 999, context.Tick, prior.Revision),
            "stale" => Graph.Select(prior, 101, context.Tick, prior.Revision - 1),
            "advance" => Graph.Advance(prior, context.Tick, prior.Revision),
            "cancel" => Graph.Cancel(prior, context.Tick, prior.Revision),
            _ => throw new InvalidOperationException("Unrecognized dialogue control."),
        };
        Apply(ref state, result);
        if (!result.Accepted)
            return;
        context.SetUi(Line, text: Text(state.Node));
        if (state.FailControl != 0)
        {
            context.RequestSave("dialogue-rejected-intent", 0);
            throw new InvalidOperationException($"Dialogue control fault after selecting node {state.Node}.");
        }
    }

    public override void Tick(ref DialogueProbeState state, GameContext context)
    {
        ulong completed = context.Tick + 1;
        if (state.Node is 30 or 40)
        {
            var result = Graph.Advance(Read(state), completed, state.Revision);
            if (result.Accepted)
            {
                Apply(ref state, result);
                context.SetUi(Line, text: Text(state.Node));
            }
        }
        if ((long)completed == state.FailTick)
            throw new InvalidOperationException($"Dialogue Tick fault after node {state.Node} revision {state.Revision}.");
    }
}
