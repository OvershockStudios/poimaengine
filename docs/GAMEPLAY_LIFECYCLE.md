# Spawning and removing props from C#

This guide covers the original **services ABI 7 (176-byte baseline)** lifecycle API: frozen, standalone root-prop templates with optional box physics, static presentation and registered custom components. Call ABI remains 1. [Hierarchical runtime instances](RUNTIME_INSTANCES.md) describes the separate 232-byte extension for complete assemblies, including cameras, characters, animation rigs and audio emitters. Arbitrary authored-entity removal remains unsupported. Rebuild the SDK, managed bridge and game assemblies together when adopting new SDK features; republish Native AOT artifacts.

## Create and initialize

Author a recipe through `world.transact` with `template.set`, as described in [the runtime catalog](RUNTIME.md#standalone-template-catalog-development). Recipes are frozen when a runtime starts. `TemplateId` identifies a recipe; `EntityId` identifies a live or reserved instance. They are separate types with no implicit conversion.

Inside `Game<TState>.Tick`, use the recipe's canonical ID:

```csharp
static readonly TemplateId Crate = TemplateId.Parse("44444444444444444444444444444444");

// Health is a generated component already present in this recipe.
var entity = context.Spawn(Crate);
var health = context.GetTemplate<Health>(Crate);
health.Current = 50;
context.Set(entity, in health);
state.Selected = entity; // Selected is an EntityId field in TState.
```

`Spawn` returns a reserved ID immediately. `Set<T>` queues a complete replacement of a component the recipe already contains. It cannot add an arbitrary new component type. Two births may reference each other, and global gameplay state may reference a birth: the native runtime validates all final references together before publishing membership.

Use `Spawn(template, in SpawnTransform)` for a complete position, normalized XYZW rotation and positive scale override. Other values come from the frozen recipe. `GetTemplate<T>` and `TryGetTemplate<T>` read those recipe values explicitly; instance edits never change them. `TemplateId` is not yet a persisted gameplay/component field kind. Use a compiled constant/static readonly handle until configurable resource-reference fields are supported.

## Visibility and movement

Entity reads, `IsAlive`, component queries, component reads and raycasts observe committed membership during Tick. A reserved birth is absent from these reads until publication; `Get` on that ID reports an unknown entity. Queued component writes also remain invisible to reads. The next Tick sees the published entity and initialized values.

A new kinematic box can receive a `MoveKinematic` command during its creation Tick. Native membership publishes before motion preparation and physics, so movement starts in that physics step. The existing duration, speed and angular-speed bounds apply. Dynamic-body launch velocity and impulses are separate unfinished APIs.

## Remove or cancel

`context.Despawn(entity)` queues removal of a previously spawned root prop. Surviving components and gameplay state must repair or clear references to it in the same Tick. Duplicate removals reject synchronously and may be caught by gameplay. Uncaught callback errors and final-candidate conflicts, including component/motion writes targeting an existing entity removed that Tick, reject the batch. Ongoing movement from an earlier Tick may end through removal.

Removing a birth reserved in the current Tick cancels it before native object creation. Its queued component initializers and kinematic motion are discarded. References from other surviving objects or global state still must be cleared. The ID remains consumed if the batch succeeds, including a Tick whose only structural work was canceled births; snapshots preserve that allocation history. Writing to or moving the canceled ID afterward is invalid.

## Publication and failure

Gameplay reserves IDs before any host-scheduled births in the same Tick. Both sources publish one candidate and increment the structure revision once. Native host results list only the host's requested births. Structural callbacks and host scheduling share a 4,096-command budget per Tick; spawning and then canceling counts as two commands. Component payloads retain the combined 2 MiB per-Tick bound, and live/retained entity, component and physics budgets still apply.

Malformed templates/transforms, unsupported component layouts, duplicate removals and immediate queue limits can reject a callback synchronously. If gameplay catches such an error, a failed spawn has not consumed an ID or left a partial command. Candidate-dependent checks, including final references and physics capacity, can instead fail after Tick returns.

A failure anywhere in an explicit multi-tick batch rolls back the entire batch: entity membership, component/global state, physics, animation, sound, ID allocation and queued save requests. IDs returned during Tick are tentative until the outer batch commits. Arbitrary external C# side effects are outside this rollback contract. Automatic editor playback commits individual ticks, so an error in a later tick preserves earlier successful ticks.

Version 3 runtime snapshots retain live spawn provenance and the generated-ID cursor. Exact supported content/module matching still applies; this ABI upgrade is not a save migration. [Save contract](RUNTIME.md#portable-runtime-snapshot-foundation).

## Qualification

Five native integration groups pass under CoreCLR and Native AOT on Windows and Linux. The agent-service fixture also verifies revision guards, receipt recovery, reference repair/removal, durable restoration and continued gameplay; its CoreCLR path checks compatible reload with spawned objects. These are bounded correctness fixtures, with no new GUI, graphics-performance, console or relocated deployment claim. [Recorded source and artifact evidence](evidence/m2-managed-lifecycle.json).
