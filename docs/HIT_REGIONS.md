# Character hit regions

`HitRegion` adds an immutable query shape to an entity without creating a
physics body or collision response. Attach shapes to imported `RigNode`
entities to follow their current native animation pose. Use
`runtime.hit_test` or C# `GameContext.HitTest` to test those regions together
with ordinary world geometry. Existing `runtime.raycast` and C# `Raycast`
continue to query physics colliders alone.

## Author a region

Inspect the imported hierarchy and choose an existing attachment entity and
its owner. The owner must be the attachment itself or an ancestor in the
authored graph. A region name is ASCII `[a-z][a-z0-9_.-]{0,63}`; the pair
`(owner, name)` must be unique, including disabled regions.

For example, attach a head sphere to existing bone entity `...00c9`, below
actor `...00c8`, using the observed authored revision:

```json
{
  "request_id":"00000000000000000000000000000301",
  "base_revision":7,
  "ops":[{
    "op":"component.set",
    "id":"000000000000000000000000000000c9",
    "type":"HitRegion",
    "value":{
      "owner":"000000000000000000000000000000c8",
      "name":"head",
      "shape":"sphere",
      "radius":0.12,
      "center":[0,0.08,0],
      "enabled":true
    }
  }]
}
```

Send this object as `world.transact` parameters. Replace the illustrative IDs,
revision and local dimensions with values for your model. The shape is local
to the attachment's complete world transform; it is not inferred from skin
weights or anatomical names.

All shapes require `owner`, `name` and `shape`. Optional `center` defaults to
`[0,0,0]`; optional `enabled` defaults to true. Unknown or irrelevant fields,
non-Boolean enabled values and invalid graph references reject the whole
transaction.

| Shape | Required dimensions | Meaning |
| --- | --- | --- |
| `sphere` | `radius` | Local sphere radius in meters. |
| `box` | `half_extents` | Three local positive half extents. |
| `capsule` | `radius`, `half_height` | Y-axis cylinder segment from `-half_height` to `+half_height`, with hemispherical ends; zero half height is a sphere. |

Radius and each half extent are 0.001–10,000 meters; capsule half height is
0–10,000 meters. Center coordinates are finite within ±10,000 meters. At most
4,096 region components may be live, including disabled regions. These are
admission bounds, not a qualified character count or large-world envelope.

Regions freeze when the runtime starts. Later authored edits mark the source
stale and do not change live hit shapes. Restart with the new authored revision
to adopt them. Hierarchical templates support local owners that remap to each
instance. Legacy root-prop templates do not support HitRegion. Whole-instance
removal removes its regions from queries; retained rollback owners do not
remain queryable. Ordinary saves reconstruct regions from the exact frozen
definition and restore the current animation/state; this adds no mutable save
rows or general content-upgrade permission.

## Query the committed pose

Pass an exact live session and tick to `runtime.hit_test`:

```json
{
  "session_id":"00000000000000000000000000000384",
  "tick":60,
  "origin":[0,1.6,5],
  "direction":[0,0,-1],
  "distance":100,
  "ignore":["00000000000000000000000000000064"]
}
```

The response contains `session_id`, the unchanged `tick` and `hit`, which is
null on a miss. A hit has `entity`, `region_entity`, `region`, `fraction`,
`distance`, `position`, `normal` and `triangle`. For a region, `entity` is the
owner and the two region fields identify its attachment and name; `triangle`
is null. For geometry, the region fields are null and a static triangle mesh
may return a triangle index. Unavailable normals are null.

Queries read the current live native world matrices without advancing time or
resampling animation. Inverse-affine intersections preserve nonuniform scale
and shear for all three shapes; normals use the inverse transpose. Singular
or nonfinite query poses reject. A ray beginning inside or on a region returns
fraction/distance zero and no normal. Tangencies and the segment endpoint are
included.

The ray contract matches existing raycasts: distance is 0.001–10,000 meters,
finite origin and endpoint coordinates stay within ±1,000,000 meters, and the
finite direction is normalized. Each direction coordinate is bounded to
±1,000,000; its squared length must be at least `1e-24`. The explicit ignore
list permits at most 128 unique existing entity IDs.

Enabled regions suppress the exact physics body or character capsule of their
owner in composite queries, even when the region is missed. Descendant bodies
remain occluders. Ignoring an owner suppresses all its regions and body;
ignoring an attachment suppresses that region while its enabled owner's proxy
stays suppressed. Internal proxy owners have their own region budget and do
not consume the caller's 128 ignored IDs. Disabled regions neither intersect
nor suppress an owner proxy.

The closest hit wins. At exactly equal fractions, geometry wins over a region;
region ties sort by owner ID, name and attachment ID. Physics-only ties retain
the existing entity/triangle ordering. Regions are query-only: they do not
change controller movement, collision response or static mesh geometry.

## Use from C#

Declare `IHitRegionsGame` for hit tests. This example also declares the
independent `IInputActionsGame` marker to consume a named Fire action:

```csharp
using Poima;

public struct CombatState
{
    public EntityId Player, Camera;
    public int RegionHits;
}

[GameModule("example.region-query")]
public sealed class CombatGame : Game<CombatState>, IHitRegionsGame, IInputActionsGame
{
    public override void Initialize(ref CombatState state)
    {
        state = default;
        state.Player = new(0, 100);
        state.Camera = new(0, 101);
    }

    public override void Tick(ref CombatState state, GameContext context)
    {
        if (!context.GetInputAction(state.Player, "fire").Pressed) return;
        var view = context.Get(state.Camera).Transform;
        var hit = context.HitTest(view.Position, view.Forward, 100,
                                 new EntityId[] { state.Player });
        if (hit is not null && hit.Value.Region is not null)
            ++state.RegionHits;
        // hit.Value.Entity identifies the damage owner when a hit exists.
        // A nearer wall has Region == null and blocks the character query.
    }
}
```

Replace the player/camera IDs with existing live entities and define the Fire
binding or scripted action. `HitTestHit` is an immutable result with Entity,
nullable RegionEntity/Region, Fraction, Distance, Position, nullable Normal
and nullable Triangle. A miss returns null. Damage rules, ammo and hit effects
belong to your gameplay; persist their resulting state through ordinary native
game fields and components.

Queries are Tick-only and permit at most 1,024 admitted calls per tick. Staged
movement, animation and instance commands remain invisible until publication;
a pending birth is not yet an existing ignored entity or query target.
Callback failures participate in ordinary complete batch rollback. External
side effects in trusted C# code remain outside that rollback boundary.

## Compatibility and qualification

`hit_regions_v1` is independent of named input, preferences, navigation,
instances and animation services. It requires a 272-byte services prefix in
epoch 7, with the callback at byte 264. The version-1 native result is 192
bytes. The matched SDK, bridge and Native AOT generator negotiate this feature
before constructing the game; a short table, missing callback or undeclared
operation rejects. A larger host prefix does not grant other extensions.
Call ABI 1/80, legacy GameInput 40 bytes, the 176-byte baseline and earlier
negotiated prefixes remain unchanged.

Windows qualification passes twelve native suites, seven hit-region groups,
four headless API groups and 78 compiled ABI checks. Real CoreCLR and Native AOT
gameplay checks cover animated bone poses, the 1,024-query limit, callback and
later-tick rollback, save intents and exact fresh-process continuation. CoreCLR
also checks compatible reload. [Results](evidence/m2-hit-regions.json).

These fixtures use synthetic shapes and an analytic imported rig. They do not
qualify game-scale performance, physical aiming, rendered region visualization
or Alpha completion.
