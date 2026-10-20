# Finite-speed projectiles

The optional [FPS package](../packages/fps/README.md) supplies constant-acceleration
sphere flight with swept collision. Games own launch rules, ammunition, damage
and the persistent flight state. A projectile is not a native rigid body: the
helper queries committed physics and returns the next numerical state.

## Launch and advance

Declare `ICapsuleCastGame` on the compiled game to negotiate the native
[capsule query service](CAPSULE_QUERIES.md). Use a zero-height capsule as a sphere:

```csharp
var definition = new ProjectileDefinition(
    Acceleration: new(0, -9.81, 0), Radius: .02, Lifetime: 2,
    MaxChordError: .0001, MaxSegments: 8);
var flight = Projectiles.Launch(origin, velocity);
// On subsequent fixed ticks, reconstruct flight from the game's saved fields.
var step = Projectiles.Step(in definition, in flight, context.DeltaTime,
    context, ignore: [shooter]);
flight = step.State;
if (step.Hit is { } contact)
{
    // Apply this game's damage/impact policy from the original contact.
    // flight.Position is the sphere centre; contact points are surface points.
}
```

`Launch` starts at age zero, including when velocity is zero. `Step` returns
position, incident velocity, age, phase, an optional original contact, query
count, consumed time and maximum submitted curvature bound. `Impacted` and
`Expired` are terminal: later steps validate the inputs but issue no queries,
return no new contact and consume no time. Flying at its lifetime becomes
Expired without querying. A caller may explicitly expire a younger projectile.

Launch projectiles from accepted [weapon shot events](WEAPON_ACTIONS.md), then
advance their owned states on fixed ticks. Use independent aim and visual
recoil profiles; a cosmetic camera or weapon offset need not change flight.
The sample demonstrates one projectile, rather than a weapon inventory or pool.

## Curved motion and collision policy

Free flight uses `p(t) = p0 + v0*t + a*t*t/2` and `v(t) = v0 + a*t`, evaluated
from the start of the step. Acceleration is constant for that step. The helper
caps time at the remaining lifetime and subdivides the trajectory into linear
chords. A quadratic path differs from its chord by at most `|a|*h*h/8` over a
segment lasting `h` seconds. Each sweep inflates the physical sphere by an
outward-rounded bound on that deviation, then rounds its radius upward to
binary32 for the native backend. No separate collision body is created.

`MaxChordError` bounds the computed curvature envelope. Binary32 radius padding,
position rounding and native contact tolerances are separate. An inflated
sweep can stop early; its `InitialOverlap` does not prove the uninflated sphere
was initially overlapping. This is a conservative curved-path approximation,
not exact parabolic time of impact, a formal floating-point nonpenetration
proof or relative CCD against a moving target. A zero-length chord still makes
an overlap query with a nonzero fallback direction.

The first hit stops at the swept chord centre at the provider's fraction.
Elapsed time and incident velocity use that segment's proportional duration,
including preceding clear segments. Surface points are retained unchanged;
they never replace the centre. Contact at the final lifetime endpoint wins
over expiry. A clear step ends at the analytic position and velocity; expiry
sets age to the exact configured lifetime.

Queries see committed geometry. Staged spawns and body movements are not
predicted. Physics colliders determine contact, including real mesh apertures;
texture alpha, visual-only meshes and separate HitRegion volumes do not.
Read [query contact and geometry limits](CAPSULE_QUERIES.md#hits-and-geometry).

## Bounds and failure

| Operand | Supported domain |
| --- | --- |
| Position | Each axis within ±1,000,000 m |
| Velocity and acceleration | Each axis within ±100,000 m/s or m/s² |
| Physical radius | 0.001–9 m |
| Lifetime | Greater than zero, at most 3,600 s |
| Time step | Greater than zero, at most 1 s |
| Curvature error | 0.000001–1 m |
| Segments per projectile step | 1–32 |
| Inflated query radius and chord travel | At most 10 m and 10,000 m |

Before querying, the helper validates the recipe, current state, trajectory
axis extrema, end velocity, required segment budget and reconstructed query
endpoints. Time must advance at the supplied floating-point precision. Invalid
operands throw `ArgumentOutOfRangeException`; unrepresentable motion or an
insufficient budget throws `ArithmeticException`. It never drops sweeps or
increases the error allowance to fit a budget.

Native queries permit **32 attempts total per callback**, across all projectiles
and other capsule calls. `MaxSegments` is not an additional allowance. Games
must budget their aggregate work. Exhaustion and provider exceptions propagate;
they do not become clear-space results. A failing compiled callback rolls back
its native tick batch. A custom `IProjectileQueries` provider owns any external
side effects; the stateless helper cannot undo those.

The generic `Step<TQuery>` supports independent collision providers without
boxing or delegate allocation. Providers must return correctly typed contacts
with finite fractions in [0,1]. The helper validates the fraction and otherwise
preserves the contact; the native SDK separately validates its wire result.

## Saves and scope

Store all persistent position/velocity/age/phase operands and the selected
recipe identity in supported primitive state fields. The helper records are
transient values, not nested serializable field declarations. No global clock,
random generator, mutable hidden state or normal-path heap allocation is used.
Compatible saved recipes remain the game's responsibility; changing gravity,
radius or lifetime does not install a save migration.

[Projectile Range](../examples/projectile-range/README.md) supplies a compiled
game, source preparation and guarded world authoring. Its independent verifier
checks thin-wall contact, sphere grazing, overlap, analytic gravity, lifetime,
complete batch rollback and fresh-process saved-flight continuation.
[Qualification](evidence/m2-projectiles.json) records the exact execution scope.

Penetration, ricochet, drag, wind, damage regions, moving-target relative CCD,
projectile pooling and production-scale scheduling remain further work.

## Reproduce the checks

Build and run the pure tests with the matched SDK:

```sh
dotnet build tests/Projectiles/Poima.Projectiles.Tests.csproj -c Release -o <test-build> -p:PoimaGameplayAssembly=<Poima.Gameplay.dll>
dotnet <test-build>/Poima.Projectiles.Tests.dll
```

After building the sample, run its independent native verifier inside an owned
process tree, supplying a fresh output directory:

```sh
python tests/projectile_gameplay_contract.py <poima> --hostfxr <hostfxr> --bridge <Poima.ManagedBridge.dll> --assembly <sample-build>/Poima.ProjectileRangeGame.dll --output <new-evidence-directory>
```

Use native Windows Python and Windows paths for Windows execution. The verifier
retains literal requests/responses, raw typed fields, original save archives and
native closure records. The selected bounds are 180 seconds, 450 RPCs and 32 MiB
of evidence. These focused checks do not replace the existing release gates.
