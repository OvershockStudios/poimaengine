# Deterministic NPC behavior trees

Use `Poima.Ai.BehaviorTree` to evaluate authored conditions and advance actions
in a fixed order. Reference `Poima.Ai.csproj` or compile `BehaviorTree.cs`
into a trusted C# game project with .NET 10 and C# 14. One immutable tree can
serve multiple NPCs. Each NPC owns its continuation, perception and action
progress.

## Define and step a tree

The rows are preorder. After a composite, place its first child's entire
subtree, then its next child. A composite specifies its number of **direct**
children. Each leaf's nonnegative key identifies an operation in the game's
callback provider.

```csharp
static readonly BehaviorTree Sentry = new([
    BehaviorNode.Selector(2),
    BehaviorNode.Sequence(2),
    BehaviorNode.Condition(CanSeeTarget),
    BehaviorNode.Action(Observe),
    BehaviorNode.Action(Patrol)
], revision: 1);
```

- `Sequence` advances after each child succeeds and fails on the first failure.
- `Selector` advances after each child fails and succeeds on the first success.
- `IBehaviorTreeLeaves.TestCondition` returns true for success.
- `IBehaviorTreeLeaves.StepAction` receives the leaf key, its zero-based row
  index and a flag indicating a resumed action. It returns `Success`, `Failure`
  or `Running`.

Call `Step` from the game's fixed Tick, with the NPC's current cursor, a node
budget, a trace span and its callback provider by reference. The provider can
be a struct or a `ref struct` holding the current `GameContext`. Conditions
can read the current sight and last-seen memory supplied by the game.

```csharp
Span<BehaviorTrace> trace = stackalloc BehaviorTrace[16];
BehaviorStep step = Sentry.Step(cursor, nodeBudget: 5, trace, ref leaves);
cursor = step.State;
ReadOnlySpan<BehaviorTrace> events = trace[..step.TraceWritten];
```

A `Running` action resumes from that row on the next step. Its prior guards
and siblings retain their decisions. If a new stimulus should interrupt the
action, the game can read `cursor.ActiveAction`, cancel the action and its
game-owned effects, then reset the cursor to `default`. At completion the
evaluator returns a fresh cursor for the next Tick.

## Scheduling and traces

Compile one rooted tree with 1–256 rows and at most 32 levels. Each entry or
action resume spends one budget unit. Propagating an outcome through parents
costs no visits and is bounded by the tree depth. With `BudgetExhausted`, the
returned cursor points to the next unentered node. A zero budget preserves the
existing cursor and calls no leaves. Games decide how to split a total budget
among NPCs and how much work each leaf may perform.

`Enter` and `Resume` record the evaluated row. `Success`, `Failure` and
`Running` record outcomes, including parent outcomes in the order they
resolve. Each step produces at most 512 trace entries. A short span keeps the
prefix and `TraceDropped` reports the omitted entries. An empty span captures
no entries. Aggregate step status and cursor are independent of the trace size.
Definition construction copies and validates rows. Stepping performs no
managed allocation in the evaluator. Invalid arguments and callback errors
leave the caller's trace span intact. A callback provider owns its side effects.

## Save or replay an NPC

Store the cursor as three gameplay scalars, `LayoutId` (`long`), `NextNode`
(`int`) and `ActionRunning` (`int`, 0 or 1). `NextNode` is one-based except for
the fresh, all-zero cursor. Store any leaf progress, observations and game-owned
RNG state separately. Restore the scalars and call `ValidateState` before
continuing. `LayoutId` is a nonzero, JSON-safe 53-bit fingerprint of the
ordered node kinds, operands and supplied revision. Change the revision when
leaf behavior changes incompatibly. A changed layout requires an explicit
game decision to discard or migrate the continuation.

The same tree, cursor, budget and leaf replies yield the same ordered calls,
step result and trace. Fixed Tick scheduling and stable game data are part of
that contract. Budgets split evaluation across ticks, so a game may sample
different observations at the next resumed node. Treat scheduling parameters
as replay input whenever they can vary.

## Compiled guard example

[GuardGame.cs](../../tests/ai_behavior_tree/GuardGame.cs) compiles a
`Game<GuardState>` consumer. Its `Tick` reads sight for target entity 7. The
guard observes a visible target and otherwise selects a three-tick patrol
decision. Fresh sight interrupts a running patrol. The example stores action
progress and the cursor in gameplay state. The decisions are observable as
fields, ready for a game to connect to movement and animation.

```sh
dotnet run --project tests/ai_behavior_tree/Poima.BehaviorTree.Tests.csproj -c Release
```

The managed fixture checks the compiled package with supplied leaf and sight
inputs. It covers ordering, traces, bounds, cursor replay, interruptions and
allocation.

For the headless CoreCLR workflow, use a Poima binary built with simulation
and managed gameplay, an existing .NET `libhostfxr.so`, the compiled managed
bridge and the test assembly from the build above. Keep each assembly's
dependencies beside it. Use a new output directory for the replay evidence.

```sh
python3 tests/ai_behavior_tree/native_guard.py <poima> \
  --hostfxr <libhostfxr.so> --bridge <Poima.ManagedBridge.dll> \
  --assembly <Poima.BehaviorTree.Tests.dll> --output <new-output-directory>
```

This fixture moves a native kinematic blocker out of the guard's sight line.
It checks that the guard resumes its concealed patrol after a fresh-owner
load, switches to Observe when the blocker clears, and writes the same full
checkpoint bytes as the uninterrupted run. The
[Perception Yard](../../examples/perception-yard/README.md) covers a larger
set of sight geometry and last-seen memory cases.
