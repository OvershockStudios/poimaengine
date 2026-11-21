# Sweep a melee blade

The optional [FPS package](../packages/fps/README.md) gives compiled C#
gameplay a bounded melee contact query. Supply two consecutive world-space
samples of the blade's base and tip. The query returns the first collider it
reaches, including cover. Use the result to decide damage, block reactions
and contact effects.

## Author the attack

Choose the windup, active interval and recovery for each clip. Keep attack
phase, the previous blade sample and whether the attack has consumed its
first contact in game state. At the beginning of the active interval, seed
the previous sample from the current pose. Reset it on a teleport, reparent,
clip seek or other discontinuity. Track the world blade used for contact
independently of the foreground lens, clearance and cosmetic camera effects.
Compare the sampled blade against the intended grip and full-body motion at
the same committed ticks. [Pose comparison](POSE_FRAME_COMPARISON.md)

Declare `ICapsuleCastGame` on the gameplay module. In `Tick`, pass the two
samples and the collider IDs to ignore. The example budget allows up to
eight capsule attempts for this swing interval.

```csharp
var profile = new MeleeSweepProfile(Radius: .04, MaxIntervalError: .01, MaxIntervals: 8);
var previous = new MeleeBladeSample(previousBase, previousTip);
var current = new MeleeBladeSample(currentBase, currentTip);
var contact = Melee.Sweep(profile, previous, current, context, [ownerCollider]);
```

Call during the active interval. Consume a first contact before queuing
effects or damage for that attack. For example, if it hits a wall first,
mark the attack consumed and use the wall point for a scrape. A subsequent
sample cannot register damage through that wall for the same attack when the
game honors the consumed state. Save and restore it with the prior endpoints.

## Interpret the result

- `Hit` contains the native contact with its entity, query and obstacle
  points, optional normal and triangle, and initial overlap flag. The
  fraction and distance in this record refer to the reported capsule attempt.
- `Fraction` maps the contact to the full supplied sample pair. A miss has
  fraction one and no hit. An initial overlap in a later subinterval reports
  that interval's starting fraction.
- `Queries` reports how many attempts ran. The native limit is 32 capsule
  attempts in a tick across all callers that share that budget.
- `MaxIntervalError` reports the geometric radius inflation per interval
  before numeric rounding. Choose this bound and the interval budget
  together for your blade length and motion.

The helper linearly interpolates both endpoints. For each subinterval it
sweeps a capsule in the direction of the blade centre's travel, aligned to
the midpoint blade axis. It grows the radius by a quarter of the subinterval
change in that axis vector to enclose the interpolated blade. A fixed axis
uses a translated capsule or a stationary overlap with no geometric inflation.
Subdivision reduces geometric overreach. The envelope can report contact
before the nominal blade reaches a collider, and the collider positions come
from the native query's committed scene. Sample curved motions closely and
keep their transitions continuous. [Capsule queries](CAPSULE_QUERIES.md)

The earliest reporting subinterval wins. Within it, the native query orders
moving contacts by fraction and initial overlaps by penetration. The native
tie rules apply to equivalent candidates. Only native Jolt query colliders
participate. Use a separate [hit-region ray](HIT_REGIONS.md) when the game
also needs a skeletal damage zone observation.

Endpoints must be finite and within ±1,000 km per axis, with at most 20 m
between them. The radius is .001–10 m. The interval error bound is .000001–1 m.
Set the interval budget from 1 to 32. If no plan satisfies the profile and
native geometry limits, the helper throws before attempting contact.

## Verify an attack

The [compiled fixture](../tests/fixtures/melee-contact/MeleeContactGame.cs)
drives one authored blade trajectory against a flat target, cover, a clear
miss and an initial overlap. The independent Python check compares the
native contact fraction with the expected plane intersection. It also
checks the first-contact marker, a later failed tick, and byte-identical
warm and fresh-process continuation after saving mid-swing.

```sh
python3 tests/managed_melee_contract.py /path/to/poima \
  --hostfxr /path/to/hostfxr --bridge /path/to/Poima.ManagedBridge.dll \
  --assembly /path/to/Poima.MeleeContactFixture.dll --output build/melee-check
```

Use .NET 10 to build
`tests/fixtures/melee-contact/Poima.MeleeContactFixture.csproj` and the
matching `managed/Poima.ManagedBridge` project. Native AOT uses
`--native-descriptor` in place of the three CoreCLR paths. Add
`--windows-interop` when calling a Windows executable from WSL.
