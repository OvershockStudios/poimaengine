# Branching dialogue

`Poima.Dialogue` holds authored conversation routes and evaluates commands
deterministically. Reference `Poima.Dialogue.csproj` from compiled C# gameplay.

```csharp
using Poima.Dialogue;

// In the gameplay class.
static readonly DialogueGraph BellKeeper = new([
    new DialogueNode(100, 200),
    new DialogueNode(200, [new(10, 300), new(20, DialogueGraph.End)], delayTicks: 30),
    new DialogueNode(300, DialogueGraph.End)
]);
```

Use positive IDs for nodes and choices. Keep those IDs stable when reordering
or editing the conversation. Choice IDs are unique within each node. A route
to `DialogueGraph.End` completes the conversation. A graph holds up to 128
nodes with up to eight choices each. It copies the definitions and validates
destination IDs at construction. Cycles allow repeatable responses.

Store `Node`, `Revision` and `ChangedTick` as `int`, `int` and `long` fields in
the game's save state. All three start at zero. Reconstruct `DialogueState`
from those fields before applying a command and copy its returned state back
after an accepted change. Use `context.Tick` as the tick when running gameplay.
`FindNode` exposes the current node and ordered choice IDs for display.

Supply the revision captured when the input was offered as `expectedRevision`.
An accepted command increments it and records the supplied tick. At most one
command succeeds per tick. A stale revision, invalid choice, or premature
input returns the original state and a reason. A node's optional
`delayTicks` makes its outgoing advance or choices available after that many
ticks, with no automatic advancement. Cancel can close a waiting node and
still uses the same tick and revision guards. Gameplay owns text, localized
labels, UI, input and any side effects for accepted commands.
The delay limit is 216,000 ticks.
