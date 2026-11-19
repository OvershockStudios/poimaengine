# Transfer a held item between anchors

<!-- SPDX-License-Identifier: Apache-2.0 -->

Use an attachment to move a rigid item between a character's hands, a holster,
a weapon socket or another live entity. Anchors may follow animation or
physics. The item and its ordinary mesh descendants follow the anchor's
committed matrix, so their relative alignment survives subsequent ticks. If
the prop is part of an imported skin and has a single joint owner, extract its
rigid primitives before attaching it. [Rigid mesh extraction](ANIMATION_ASSETS.md#extract-a-rigid-item-from-a-skin)

Use [matched viewmodel FOV](VIEWMODELS.md#configure-a-camera-and-drawables)
when a first-person pickup or release should keep its projection scale as it
changes render passes.
Use [world depth](VIEWMODELS.md#share-depth-with-the-world) when the fingers
and prop should depth-test together throughout that transfer.

## Transfer on a committed tick

Read the active session, tick and structure revision from `runtime.inspect`.
Use live IDs for the item and anchor. The item can be a transform wrapper with
several rigid drawables. `runtime.attachment.apply` accepts up to 64 distinct
items in one transaction.

```json
{
  "session_id": "10000000000000000000000000000001",
  "request_id": "20000000000000000000000000000001",
  "expected_tick": 64,
  "expected_structure_revision": 2,
  "commands": [
    {
      "entity": "30000000000000000000000000000001",
      "mode": "keep_world",
      "anchor": "30000000000000000000000000000002"
    }
  ]
}
```

| Mode | Effect |
| --- | --- |
| `keep_world` | Retains the item's current world matrix and derives its offset from the new anchor. Set `anchor` to null to hold the item in world space. |
| `snap` | Requires an anchor and a complete `offset` with `position` in metres, a normalized XYZW `rotation`, and positive XYZ `scale`. Composes the offset with the anchor. A nonmatching socket produces a deliberate snap. |
| `reset` | Supply only `entity` and `mode`. Restores the active item's authored parent and local transform. |
| `pickup` | Requires a released item, a live receiving anchor and sampled contact. Preserves the item pose and retires the release body in the same transaction. |

All commands take effect together at the requested tick. If a command follows
another item that moves in the transaction, it uses that item's new anchor
frame. A cycle, duplicate target, expired structure revision or unsupported
item rejects the whole operation. Use up to 64 live bindings. A successful
receipt advances the structure revision by one and preserves the tick.
Exact retries in the runtime receipt window return the original outcome.

For hand-to-hand transfers, first make the receiving hand reach the current
item. Measure the contact at the transfer tick, then keep the item in place
as its anchor changes. Use a snapped calibrated offset when an insertion
needs a specific socket pose. [Frame comparisons](POSE_FRAME_COMPARISON.md) ·
[Surface measurements](SURFACE_MEASUREMENTS.md)

## Require grip alignment

A keep-world transfer to a live anchor can include a contact guard. Provide
the calibrated contact frame on the item and the receiving socket frame.
Omit either offset to use that entity's origin and axes. Set both position
and rotation limits for the intended grip.

```json
{
  "entity": "30000000000000000000000000000001",
  "mode": "keep_world",
  "anchor": "30000000000000000000000000000002",
  "contact": {
    "item_offset": {"position": [0.04, 0.01, 0], "rotation": [0, 0, 0, 1]},
    "position_limit_m": 0.015,
    "rotation_limit_deg": 5
  }
}
```

The guard compares the two composed world frames before publishing the new
binding. If another command moves the receiver in the same transaction,
the guard uses its proposed pose. Both limits are inclusive. A miss returns
`-32040` with the item and anchor IDs, position error and limit in metres,
and angular error and limit in degrees. A sheared, reflected or degenerate
basis produces a null angular error and rejects the transfer. The entire
transaction retains its previous state on a miss.

Use authored animation or IK to bring the hand to the grip.

### Require contact at sampled points

Add `surface` to the contact guard to require one or more finger pads to
reach the handle. Select cooked vertices or triangle points from the hand's
drawable and choose the facing surface on the item. For a prop with a
separate blade and handle, select the handle triangles explicitly.

```json
{
  "position_limit_m": 0.015,
  "rotation_limit_deg": 5,
  "surface": [{
    "source": {"entity": "a0000000000000000000000000000012", "vertex": 24},
    "target": {"entity": "30000000000000000000000000000001", "triangles": [12, 13]},
    "distance_limit_m": 0.005
  }]
}
```

The source and target are visible indexed drawables. The target must be
the item or one of its effective descendants before and after the transfer.
The source must stay outside that subtree. The solver evaluates the proposed
world matrices and skin palettes after resolving every command in the batch.
Each distance limit is inclusive. A guard accepts 1–8 probes, with up to 32
probes and 32,768 target triangle checks across the whole batch. Explicit
target selections support up to 4,096 distinct triangle ordinals per probe.
Use [surface measurements](SURFACE_MEASUREMENTS.md) to calibrate the sample
location and distance limit against committed poses.

A distance miss returns `-32040` with the item and anchor IDs, zero-based
`probe_index`, `distance_limit_m`, and a `surface` measurement in the same
shape as a row from `runtime.surface.measure`. The measurement includes the
proposed source and closest target points, distance, and winning triangle.
The proposed affine transforms can change the rounding of a shared-edge
witness. To reproduce the exact witness, read it from a separate replay of
the applied candidate pose.
The item bindings, revision and retry history stay at their pre-request values.
Reuse the request ID after repairing a rejected request. Invalid sample
selectors, subtree violations, and exceeded bounds return `-32602`.

## Transfer during compiled gameplay

Declare `IItemAttachmentGame` on a `Game<TState>` and call these methods from
`Tick`. The game can choose the receiving anchor from live entities or an ID
reserved earlier in the same tick. Reads require a live entity.

```csharp
var current = context.GetItemAttachment(item);
if (current?.Anchor != receivingHand)
    context.TransferItem(item, receivingHand);
```

`TransferItem(item, null)` holds the item at the world pose it reaches this
tick. `SnapItem(item, anchor, offset)` uses a complete `ItemSocketOffset`.
`ResetItemAttachment(item)` returns a bound item to its authored parent and
local transform. `GetItemAttachment` returns null for an unbound item. A
present binding with a null `Anchor` denotes a world-held item.

The read describes the committed state entering `Tick`, even after staging a
write. The engine moves and solves the current anchors first, then commits
all attachment writes together as one structure revision. The next tick sees
the new binding. A cycle, invalid transform or other rejected simulation work
rolls back the entire step batch, including an earlier tick's transfers and
staged save intents. A save requested in a successful handoff tick contains
the committed binding. Reset or transfer any existing binding before
removing its item or anchor on a later tick.

Gameplay can attempt up to 64 writes and 1,024 reads per tick. Each item may
appear once in the write batch. `world.describe` lists the `item_attachment_v1`
feature and the 528-byte epoch-7 prefix under `gameplay_item_attachments`.
Existing compiled modules retain their negotiated prefixes.

For a checked handoff, implement `IItemAttachmentContactGame` and call
`TransferItem(item, anchor, contact)` with an `ItemAttachmentContact`. This
selects `item_attachment_contact_v1` and the 256-byte version 2 command
through the existing service table. The check uses the final item and
receiving anchor after the tick's movement. A failed contact rolls back the
whole step batch and returns the same measurements through `runtime.step`.

For sampled grips, implement `IItemAttachmentSurfaceGame` and pass the
surface probes to `TransferItem`. Use cooked indices and live drawables.

```csharp
var contact = new ItemAttachmentContact(itemFrame, receivingFrame, 0.015, 5);
ReadOnlySpan<ItemSurfaceProbe> probes =
[
    new(new ItemSurfacePoint(handDrawable, fingertipVertex), itemDrawable, 0.005)
];
context.TransferItem(item, receivingHand, contact, probes);
```

Supply optional barycentric weights in `ItemSurfacePoint` to sample a
triangle. Pass `TargetTriangles` in `ItemSurfaceProbe` to restrict the closest
point search. The host copies the selectors during the call. It checks the
final animated pose and proposed item graph using the same bounds as the
immediate service. Compiled gameplay negotiates
`item_attachment_surface_v1` through the existing 528-byte allocation with
a 272-byte version 3 command. A miss returns the structured surface
measurement through `runtime.step` and rolls back the whole step batch,
including earlier transfers and staged saves.

### Continue gameplay after a missed grip

Implement `IItemAttachmentRecoveryGame` and call
`context.AttemptItemTransfer(item, receivingHand, contact, probes)` during
`Tick`. The engine resolves the pose and returns a committed outcome.
A contact miss leaves ownership with the current holder and lets the tick
finish, including animation, other gameplay and any queued save.

At the start of the next `Tick`, call
`context.GetItemTransferOutcome(item)` to read the previous tick's result.
The status is `Transferred`, `FrameMiss`, or `SurfaceMiss`. A frame miss
includes the position and angular measurements. A surface miss includes the
failed probe index, closest triangle, distance and limit. Use the status to
choose a recovery animation or wait for another grip attempt.

The ordinary strict batch runs first. Recoverable attempts then run one at
a time in staging order, with successful transfers available to later
attempts. Each successful attempt advances the structure revision once.
Contact misses consume the attempt and leave the structure revision alone.
An invalid command still rejects the step, including any earlier writes or
outcomes in that batch. All attempts share the per-tick handoff and probe
budgets.

Agents can read the same results using `runtime.attachment.outcomes` with the
session, exact committed tick and optional structure revision. The query
returns results in item ID order. Outcomes describe only the tick that just
committed. Consume them each tick or store them in the game if they need to
survive longer. Saves retain outcomes for the saved tick. Loading restores
them before the first resumed callback.

The feature negotiates `item_attachment_recovery_v1` in the existing
528-byte service table. A version 4 command uses the sampled command layout.
Outcome reads use the version 2, 120-byte response.

## Release to world physics

Use `runtime.attachment.release` on a simulation-enabled host to drop or
throw a rigid prop at the current committed pose. The prop must have an
active binding to a live holder. Author a single-root template with a
Transform and a dynamic BoxCollider. Use the template to set the box
dimensions, mass, friction and restitution. The template has no drawable
or custom gameplay components. The engine uses the released item for the
visual and the temporary spawned body for collision.

```json
{
  "session_id": "10000000000000000000000000000001",
  "request_id": "20000000000000000000000000000001",
  "expected_tick": 64,
  "expected_structure_revision": 2,
  "item": "30000000000000000000000000000001",
  "body_template": "40000000000000000000000000000001",
  "body_in_item": {"position": [0.12, 0, 0], "rotation": [0, 0, 0, 1]},
  "linear_velocity_m_s": [2.5, 0.75, 0],
  "angular_velocity_rad_s": [0, 0, 3]
}
```

`body_in_item` defaults to the item's origin and axes. Move or rotate this
frame to fit the box around the prop. Its position is in metres and its
rotation is a normalized XYZW quaternion. The resulting collision frame
must have positive scale and an unsheared, proper rotation. Both velocities
are required in world coordinates. Linear velocity applies at the box's
centre of mass. Their magnitudes are bounded by 100 m/s and 20 rad/s.
Sample the intended body frame on successive committed ticks if the prop
should inherit motion from the holder. The engine applies the supplied
velocities at release.

Successful release returns the generated `body` ID and advances the
structure revision once at the same tick. The prop retains its effective
world pose and follows the body. The prop and its ordinary attached
descendants enter world rendering, where the world's camera lens applies.
Independently bound child items retain their own draw ownership. A different
foreground lens can produce a visible projection change on release even
when the world pose stays continuous.

At admission, the box must clear the current world within 1 mm of
penetration. Invalid bounds, an obstruction or a stale guard leave the
binding, physics and allocator unchanged. Read the current item and
collision geometry before repairing the request. An obstructed service
request returns `-32602` with `data.obstacle` and `data.penetration_m`
for the deepest overlap. An exact retry of an
accepted request returns the original body within the runtime receipt
window.

`runtime.attachment.inspect` shows `released: true` and the body anchor.
Save and load keep the active release, body identities, contacts and
sleep state together. Use [pickup](#pick-up-a-released-item) to reclaim
the item and retire its proxy. Gameplay save upgrades currently reject
the joined physical snapshot format.

Run the [release fixture](../tests/item_attachment_release.py) with a new
output directory:

```sh
python3 tests/item_attachment_release.py --binary build/linux-game/poima \
  --output build/item-release-check
```

Use `--windows-interop --gpu 1` with the matching Windows Vulkan binary
from WSL and the appropriate adapter number. Compare visible geometry
before and after release, in flight, and after restoring the same launch.
The fixture also restores a separate save with live floor contact and
checks exact continuation to a settled body.

### Release from C# gameplay

Implement `IItemAttachmentReleaseGame` and call
`AttemptItemRelease` from `Tick`. The caller supplies the authored
template ID, velocities and optional collision frame. Here, the held prop
drops from the pose reached on the input tick.

```csharp
if (drop.Pressed && heldItem is { } item)
    context.AttemptItemRelease(item, dropBodyTemplate, new Vector3d(0, 0, 0),
        new Vector3d(0, 0, 0), bodyInItem);
```

Supply the authored template ID from game configuration. Serialized
`EntityId` state fields are validated against live entities.

The engine applies staged handoffs first, then checks releases in staging
order using the final animation and physics of that tick. Each successful
release advances the structure revision. A blocked release advances the
tick and keeps the holder. A malformed command still rejects the entire
step batch, including earlier successful releases and queued saves.
An item may receive one attachment command per tick. Releases share the
64-write and 1,024-read budgets with the other attachment callbacks.

Read `GetItemReleaseOutcome(item)` from the next `Tick`. It returns
`Released` with the generated body as `Anchor`, or `Blocked` with the
unchanged holder, the obstacle and its depth in metres. Use the blocked
result to keep the prop equipped or play a recovery. The outcome is
available on the same committed tick through `runtime.attachment.outcomes`
as `released` or `release_blocked`. A save requested on the attempt tick
captures the binding, physics and outcome before the next callback.

The named feature is `item_attachment_release_v1`. It uses the existing
528-byte epoch-7 table with a 152-byte version 5 command and 80-byte
version 3 result. `GetItemTransferOutcome` reports only transfer results.
Prerequisites and commands are in the
[compiled release fixture](../tests/fixtures/item_attachment_release_gameplay/README.md).

## Pick up a released item

### Aim at a released item

`runtime.attachment.aim` traces a world-space ray through the same colliders
and tie rules as [`runtime.raycast`](PHYSICS_INTERACTIONS.md#query-a-collider).
Supply an exact `session_id`, `tick` and `structure_revision` along with
`origin`, `direction`, `distance` and any explicit collider IDs to `ignore`.
The result reports the nearest `hit`. If that hit is the active physics proxy
of a released item, it also reports the item's entity ID in `item`.
A closer wall, ordinary dynamic body or other collider returns its hit with
`item: null`. A ray with no hit returns both fields as null.

The structure guard distinguishes a hit sampled before a same-tick release
or pickup from the current body membership. Querying leaves the physics
state, tick and saved contents intact. Each ray measures physical proxies.
Use the pickup's separate frame and surface guards to verify actual contact
before attaching the object.

Compiled gameplay implements `IItemAttachmentAimGame` and calls
`context.AimReleasedItem(origin, direction, distance, ignore)` in `Tick`.
It returns nullable item and collider identities, the hit fraction and
optional mesh-triangle ordinal, and the sampled tick and structure revision.
The callback reads committed state at Tick entry and shares the attachment
read budget. The feature is `item_attachment_aim_v1` with a 208-byte
version 4 query in the existing epoch-7 528-byte services prefix.

### Verify contact and pick up

Read the release with `runtime.attachment.inspect`. Use the item ID in
`runtime.attachment.apply` with mode `pickup`. The current binding identifies
the body's ownership. Include the receiving anchor, explicit alignment limits
and 1–8 [surface probes](#require-contact-at-sampled-points) from the reaching
hand or tool to the released prop. The item keeps the world pose it reached on
that tick and returns to its authored draw scope. The proxy retires in the
same structure revision.

```json
{
  "entity": "30000000000000000000000000000001",
  "mode": "pickup",
  "anchor": "30000000000000000000000000000002",
  "contact": {
    "position_limit_m": 0.015,
    "rotation_limit_deg": 5,
    "surface": [{
      "source": {"entity": "a0000000000000000000000000000012", "vertex": 24},
      "target": {"entity": "30000000000000000000000000000001", "triangles": [12, 13]},
      "distance_limit_m": 0.005
    }]
  }
}
```

Use the actual guard measurements to tune the reach. A direct contact miss
returns `-32040` with the failed frame or surface witness. A body that still
has live strong references blocks retirement. Move any dependent attachment
bindings in the same direct transaction and clear any component or gameplay
state references to the old proxy before requesting pickup. The engine keeps
the whole original state when validation fails. An exact successful retry
replays its receipt.

From C# gameplay, implement `IItemAttachmentPickupGame` and call
`context.AttemptItemPickup(item, receivingHand, contact, probes)` during
`Tick`. Use `GetItemTransferOutcome(item)` on the next callback. A contact
miss advances the tick while keeping the prop in the world. Successful pickup
reports `Transferred`. Invalid requests roll back the step batch. Saves
requested on the pickup tick preserve the result and the new holder.

The named feature is `item_attachment_pickup_v1`. It uses version 6 of the
existing 272-byte sampled command, the 120-byte transfer outcome and the
epoch-7 528-byte service table. The
[pickup fixture](../tests/fixtures/item_attachment_pickup_gameplay/README.md)
checks a dynamic drop, contact rejection, collision retirement, and exact
continuation after cold load.

The compiled [reach and pickup example](../examples/reach-pickup/README.md)
uses a released item's grip as an oriented IK target. It supports cancellation
and retries contact as the controller moves into range.
The [Carry Props example](../examples/carry-props/README.md) combines that
reach with a timed grip opening, blocked-release recovery and repeated use
in a standalone game.

### Recheck the arm after movement

For pickups that must account for a moving player or obstacle, declare
`IItemPickupClearanceGame` and supply a manual arm chain with two calibrated
world-space radii:

```csharp
var arm = new ItemPickupClearance(rig, 1, 0.04, 0.03);
context.AttemptItemPickup(item, receivingHand, contact, probes, arm);
```

The engine records the root, elbow and wrist positions at the callback.
After physics and the final hand solve, it validates the sampled frame
and fingertips. Before changing ownership, it also checks the arm between
the recorded and final points against that tick's final colliders. The
receiving anchor must be the selected chain's end node or its descendant.
The released item's own proxy and the rig's first physical ancestor are
excluded. The capsules reserve eight queries from the shared Tick budget.
The [IK probe](TWO_BONE_IK.md#probe-a-reach-for-obstacles) uses the same
four-interval swept geometry.

A blocked pickup advances the tick and leaves the prop with its release
proxy. `runtime.attachment.outcomes` reports `reach_blocked` with the intended
receiving anchor, obstacle, upper/lower segment, interval, fraction and
penetration in metres. In compiled gameplay, use
`GetItemPickupBlocker(item)` on the next Tick to read that witness.
Use `GetItemTransferOutcome(item)` for a successful transfer or a sampled
contact miss. The reach witness is saved with the blocked tick and expires
from the live outcome query after the next step.

This check controls pickup ownership. The hand pose for that tick has
already been solved, so the game can retract or adjust it once the blocker
is observed. The geometric check sweeps the two sampled arm segments
against final colliders. Continuous obstacle motion and finger or skin
deformation clearance are separate.

The named feature `item_attachment_pickup_clearance_v1` requires
`item_attachment_pickup_v1` and uses the existing 528-byte epoch-7 table.
It adds version 7 of the pickup command (312 bytes) and version 5 of the
attachment getter (88 bytes). A checkpoint containing the new result uses
attachment outcome version 2. Previous outcome encodings remain readable.
[Verification](evidence/m2-final-pickup-clearance.json)

### Choose a fallback arm pose on the blocked tick

Declare `IItemPickupPoseGame` and call
`AttemptItemPickupWithPoseFallback(item, hand, contact, probes, arm)`.
When valid contact is refused by final arm clearance, the engine tries the arm
controls from callback entry against the current animation and final colliders.
If those capsules overlap, it tries the current animation with manual IK off.
An accepted fallback updates the committed joint transforms and the cached IK
state before observation or rendering. Other slots and the finger layer keep
their committed controls.

The `reach_blocked` receipt adds a `pose` field. `previous` means the prior
controls passed the endpoint check. `animation` means the current animation
passed with manual IK disabled. `unresolved` means both options overlapped and
the attempted pose was retained. In all three cases the prop stays with its
release proxy. Read the existing pickup blocker and the committed IK state on
the next Tick to start gameplay recovery.

The opt-in requires a dedicated manual slot for the attempt and an animated
rig without physical ownership or recorded procedural grounding. It reserves
12 capsule queries from the shared Tick budget. The two fallback candidates
are checked at their endpoints against the final colliders. This does not
plan a continuous retraction path or test fingertips or skin against obstacles.

`item_attachment_pickup_pose_v1` requires `item_attachment_pickup_clearance_v1`.
It uses pickup command version 8 with the same 312-byte record and 528-byte
service table. Version 7 remains available. Outcome encoding version 3
includes the pose receipt. Earlier outcome encodings remain readable.
[Verification](evidence/m2-pickup-pose-fallback.json)

### Keep wrist orientation during fallback

Declare `IItemPickupBlendGame` and call
`AttemptItemPickupWithBlendedFallback(item, hand, contact, probes, arm)`.
This keeps the same ownership guard and tries the preceding controls first.
If they overlap, the runtime checks zero positional influence and up to
four intermediate positional weights. Each candidate reuses the preceding
wrist goal, when present. The runtime accepts a fallback only when its
endpoint capsules clear the final colliders.

The receipt adds the `blended` pose for an accepted intermediate reach or a
zero-weight positional fallback that retains wrist orientation. Read
`GetTwoBoneIkOrientation` after the block. The Carry Props example eases the
committed wrist weight across subsequent return ticks, independently of the
positional blend. The prop stays released until a valid pickup.

`item_attachment_pickup_blend_v1` requires `item_attachment_pickup_pose_v1`.
Command version 9 uses the same 312-byte record and 528-byte service table
and reserves 20 queries from the shared Tick capsule budget. An archive with
a `blended` receipt uses outcome version 4. Use the same manual-slot and rig
rules as version 8. Four intermediate probes can miss a viable configuration.
The capsule checks cover the published arm segments. Finger and skin
collision and continuous retraction paths require separate checks.
[Verification](evidence/m2-pickup-pose-blend.json)

### Steer a blocked reach away from contact

Declare `IItemPickupSteeringGame` and call
`AttemptItemPickupWithSteeredFallback(item, hand, contact, probes, arm)`.
The runtime tries up to two small wrist target offsets along the contact
normal of the blocked preceding pose. Each candidate keeps the preceding
wrist orientation goal. It passes the same endpoint capsule checks as the
blended candidates. The runtime selects the candidate with the smallest
maximum displacement across the elbow and wrist, measured relative to the
shoulder. A tie keeps the blended fallback.

A selected offset produces the `steered` pose receipt. The example starts
its return from the published IK target and eases the wrist orientation
through recovery. A later pickup still has to pass the sampled grip checks.
Node-relative target slots and contacts without a normal use the prior
fallback search.

`item_attachment_pickup_steering_v1` requires `item_attachment_pickup_blend_v1`.
Command version 10 uses the same 312-byte record and 528-byte service table.
It reserves 24 query attempts from the shared Tick capsule budget. Saves
containing a `steered` result use attachment outcome version 5 and retain
the chosen target and pose. Clearance covers endpoint capsules on that tick.
[Verification](evidence/m2-pickup-pose-steering.json)

## Observe and restore

Query `runtime.attachment.inspect` with `session_id`, `tick`, and optionally
the current `structure_revision` to get all active bindings in item ID order.
The offset and world matrices are column-major. `runtime.entity` keeps the
authored local transform and reports the effective world matrix. Use the
same structure revision in offscreen `runtime.capture` to pin a render to
its side of a same-tick transfer.

Relative offsets store complete affine matrices. Keep-world can therefore
preserve a scaled or sheared frame without decomposition into position,
rotation and scale. Following a changing scaled anchor may change the
item's scale or shear. Use rigid anchors for rigid movement.

Native checkpoints record active bindings and the structure revision.
Restoring a checkpoint revalidates the complete graph and reconstructs
the effective item transforms. A spawned item or anchor with an active
binding can be removed after the binding is reset or moved. An ordinary
save with zero attachment edits uses its prior layout. Resetting all
bindings omits the attachment extension and retains the structure revision.
The cold replay fixture uses animated anchors and verifies that an idle
kinematic body sleeps on the same tick after restore. It exercises following
a spawned kinematic anchor separately. [Kinematic save behavior](GAMEPLAY_SAVES.md)

This path moves ordinary transform and rigid-mesh subtrees. Collision,
hit regions, skinned meshes, emitters, cameras, lights and other independent
native pose owners use their own systems. A world-held item stays fixed until
another command. Ordinary transfer uses the drawable's authored viewmodel
membership. Use explicit [release](#release-to-world-physics) to hand it
off to a dynamic body and world rendering. Use animation or IK to bring the
receiving hand to the anchor before transferring the item.

Stale tick and structure guards return `-32009`. Absent or mismatched sessions
return `-32030`. Invalid payloads or graph changes return `-32602`. Retained
request IDs reused with different parameters return `-32010`. The attachment
methods sit outside runtime-game-session v1.

## Reproduce the handoff

```sh
python3 tests/runtime_attachment.py --binary build/linux-game/poima \
  --output build/attachment-check
```

Use `--windows-interop` for a Windows executable launched from WSL.
On a Windows Vulkan build, add `--gpu` with the adapter index to capture
both sides of the transfer and the resumed item motion. The test verifies
the full checkpoint before and after rejected batches and cold continuation.

The [compiled fixture](../tests/managed_item_attachment_gameplay/README.md)
adds same-tick save requests, late batch failure, and an unmarked consumer
check with actual C# gameplay.

The [contact fixture](../tests/fixtures/item_attachment_contact/README.md)
checks the direct and compiled guards and the optional original CC0 rig case.

Run the sampled guard fixture to compare committed and proposed geometry,
rejected batches, and corrected retries.

```sh
python3 tests/item_attachment_surface.py --binary build/linux-game/poima \
  --output build/sampled-attachment-check
```
