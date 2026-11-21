# Mantle and vault in the FPS package

The compiled FPS package plans an upright capsule's route from the actor's
current foot position. A mantle ends on the detected ledge. A vault clears the
obstacle and lands on a lower surface at an authored distance from its face.
The game chooses the action, when it may begin, and which collider represents
the actor. The package reports the planned phases and the collider on refusal.

## Plan and execute a move

Reference `Poima.Fps` or compile `packages/fps/Traversal.cs` into a game that
declares `ICapsuleCastGame`. The built-in query adapter runs inside `Tick` and
uses the native ray and capsule services. A caller can also supply
`ITraversalQueries` for a frozen geometry test. Use committed player feet,
camera heading, and the actor ID as an ignored collider.

```csharp
var profile = new TraversalProfile(Radius: .3, Height: 1.6, Reach: 1.1,
    MinimumRise: .2, MaximumRise: 1.2, VaultDepth: .9, MaximumDrop: .4,
    MaximumSlopeDegrees: 35, DurationTicks: 32);
var attempt = Traversal.Plan(in profile, TraversalKind.Mantle,
    feet, forward, context, [actor]);
```

An eligible static face has a walkable top. The mantle checks for a landing on
that collider. For a vault, `VaultDepth` determines where to look for landing
behind the face. The landing center and four points inside the footprint must
hit its static support, within slope and height tolerances. The planner then
sweeps an upright capsule through lift, cross and landing segments. Any contact
along the checked path refuses the traversal. The bounded footprint samples
can miss smaller gaps between the rays.

`Traversal.Sample` gives feet and phase for a fixed tick. Each segment eases
in and out, with zero velocity where the direction changes. Segment lengths
weight the time budget. The planner refuses a duration whose eased motion
could exceed the native kinematic movement limit. `Traversal.Advance` sweeps
the next step from the observed feet position. It stops if the actor has
drifted more than 3 cm or if a collider blocks the step. The final step also
rechecks the landing footprint. A refusal leaves the movement policy to the
game, which can hold position or return control to normal movement.

The fixture applies each accepted position using `MoveKinematic` over one
tick. Its upright box collider is contained in the query capsule. In a game
using this approach, apply the same feet-to-body offset and ensure the
actual collider fits inside the swept query envelope. Capsule checks position
the bottom 2.5 cm above the nominal feet to account for resting contact.
Use the actual committed actor transform for the next assessment.

A plan uses at most eight rays and three capsule casts. Ordinary movement
ticks use one cast, and the final step can make five more rays.
The profile accepts radius 0.05–1 m, height between the diameter and 4 m,
reach 0.1–4 m, ledge rise 0.15–3 m, vault depth up to 5 m, drop up to 3 m,
slope 0–60 degrees and 6–180 ticks at 60 Hz. Queries inspect a committed world.
The game must account for physics that changes the world within the same tick.

The native `CharacterController` currently exposes input intent, stance and
planar root motion. Applying the planned vertical path to that controller
requires a movement-ownership hook. The included implementation uses an
authored kinematic body. Moving ledges, animation and camera feel each need
their own integration and playtest.

## Save or replay an active plan

Keep the elapsed ticks, traversal kind, start and landing positions, ledge
top, obstacle and support IDs and the same profile revision in game-owned
state. Store vector components as separate scalar fields in `Game<TState>`.
`Traversal.Restore` reconstructs the same trajectory from these operands.
On the next `Tick`, use `Advance` against the restored world as usual.

## Verify the compiled path

Publish `tests/fps_traversal_gameplay/FpsTraversalGame.csproj` as Native AOT
using the installed authoring pack. Select game type
`Poima.TraversalFixture.Game`. Run with a simulation-enabled host that
advertises `capsule_cast_v1` and native gameplay.

```sh
python tests/fps_traversal_gameplay_contract.py \
  --binary /absolute/runtime/bin/poima \
  --descriptor /absolute/artifact/native-gameplay.json \
  --output /absolute/new-traversal-check
```

The test checks actual actor positions for mantle and vault, refusal for a
missing face, narrow landing, moving ledge and ceiling, interruption by a
moving blocker, and the same saved continuation in a fresh native process.
