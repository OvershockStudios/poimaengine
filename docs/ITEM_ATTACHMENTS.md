# Transfer a held item between anchors

<!-- SPDX-License-Identifier: Apache-2.0 -->

Use an attachment to move a rigid item between a character's hands, a holster,
a weapon socket or another live entity. Anchors may follow animation or
physics. The item and its ordinary mesh descendants follow the anchor's
committed matrix, so their relative alignment survives subsequent ticks. If
the prop is part of an imported skin and has a single joint owner, extract its
rigid primitives before attaching it. [Rigid mesh extraction](ANIMATION_ASSETS.md#extract-a-rigid-item-from-a-skin)

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
another command. Transfer preserves the drawable's authored viewmodel
membership. A physical release or visibility change requires an explicit
gameplay operation. Use animation or IK to bring the receiving hand to the
anchor before transferring the item.

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
