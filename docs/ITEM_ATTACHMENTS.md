# Transfer a held item between anchors

<!-- SPDX-License-Identifier: Apache-2.0 -->

Use an attachment to move a rigid item between a character's hands, a holster,
a weapon socket or another live entity. Anchors may follow animation or
physics. The item and its ordinary mesh descendants follow the anchor's
committed matrix, so their relative alignment survives subsequent ticks.

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
