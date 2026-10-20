# Capsule overlap and sweep queries

Use a finite volume to check a projectile, weapon envelope or other translated
probe against committed physics geometry. A capsule has a cylindrical segment
along local Y, with a sphere at each end. `half_height` measures half the
cylinder length; zero selects a sphere. Rotation stays fixed during the sweep.
These queries do not move bodies or advance the simulation.

## CLI/service

`runtime.capsule_cast` requires an active runtime and exact session/tick guards:

```json
{
  "session_id":"<session>", "tick":12,
  "position":[0,1.5,0], "rotation":[0,0,0,1],
  "radius":0.075, "half_height":0.175,
  "direction":[0,0,-1], "distance":2,
  "ignore":["<avatar entity>"]
}
```

The result contains `session_id`, `tick`, `source: "runtime"` and nullable `hit`.
Set distance to zero for an initial-overlap check. Supply a nonzero direction
even for that check; the same strict request shape applies. An ignore entry
must identify a live entity. It ignores that entity's physics body/controller,
not an arbitrary hierarchy subtree. Refresh guards after a concurrent tick.
The query is available while a shared native player runs, but is outside the
frozen runtime-game-session v1 boundary.

## C#

Declare `ICapsuleCastGame` to negotiate `capsule_cast_v1` independently of camera,
animation and other optional services:

```csharp
var hit = context.CapsuleCast(
    position: origin, rotation: Quaterniond.Identity,
    radius: .075, halfHeight: .175,
    direction: forward, distance: 2,
    ignore: [avatar]);
if (hit is { } contact)
{
    double travel = contact.Distance;
    bool anchorBlocked = contact.InitialOverlap;
}
```

The method is Tick-only and reads committed geometry. Staged motion/spawns are
not predicted. Native capsule queries permit at most 32 attempts per callback;
invalid native attempts count too. SDK argument rejection can occur before the
native callback. The returned record is transient owned contact data. Save the
primitive decisions your game needs through its normal state/component schema.

## Hits and geometry

A hit reports entity, fraction, travelled distance, `point_on_query`,
`point_on_obstacle`, optional normal, penetration depth, optional source triangle
and `initial_overlap`. The two points are contact points, **not capsule centres**.
Normals point from the obstacle toward the query when available. A missing
normal is null; an unknown direction is not reported as a zero normal.

Starting contacts return fraction/distance zero and initial overlap true,
including touching within the narrow-phase tolerance. Initial overlaps select
the deepest contact; moving casts select the earliest fraction. Exact ties use
entity, source triangle and contact-point order. These rules stabilize selection
within the supported backend, not floating-point replay across all platforms.

Only physics colliders participate. HitRegion damage volumes, visual-only
meshes and texture transparency are separate. Static mesh triangles retain
real openings and source triangle ordinals; both sides are queried. Their query
normal follows contact separation, unlike the winding normal of a mesh ray hit.
A mesh remains a triangle shell: an enclosed probe that touches no triangle is
not an overlap with a solid interior. Mesh contact simulation retains its
existing front-face policy. See [mesh collision](MESH_COLLISION.md).

| Bound | Value |
| --- | --- |
| Centre and translated endpoint | Each axis within ±1,000,000 m |
| Radius | 0.001–10 m |
| Cylinder half-height | 0–10 m |
| Travel | 0–10,000 m |
| Direction | Each axis within ±1,000,000; length at least 1e-12 |
| Rotation | XYZW unit quaternion; squared-norm tolerance 1e-5 |
| Ignore list | At most 128 distinct live identities |
| Candidate body visits | 512 across overlap and sweep |
| Contact callbacks | 2,048, including discarded contacts |
| Collision tolerance | 1e-5 m |

Exhaustion is an error meaning clearance is unknown; it never becomes a miss.
Body visits are not distinct-body counts. Input/domain limits do not guarantee
millimetre precision at large coordinates. Returned contact coordinates allow
±1e12 m because valid support contacts can exceed the centre's input bound.

## Native ABI and verification

The call ABI remains 1/80 and services epoch 7. `capsule_cast_v1` appends one
pointer at offset 392, giving a 400-byte allocation while retaining older
negotiated views and independent permissions. Version-1 request/result prefixes
are 128/144 bytes. Larger advertised prefixes preserve opaque tails; only known
bytes are read/written. Errors leave the output unchanged; misses have canonical
zero payload. This is a trusted borrowed-memory ABI, not allocation validation.

`tests/runtime_capsule_cast_native.cpp` covers actual narrow-phase geometry and
limits. `tests/capsule_cast_contract.py` exercises the native service and a
separately compiled capsule-only C# game, including callback quotas and complete
rollback/save comparisons. The managed wire harness separately checks physical
layouts, malformed results and permission/prefix behavior. Their execution and
platform scopes are recorded with the checkpoint evidence.

See the [0.0.208 results and platform scopes](evidence/m2-viewmodel-clearance.json).
