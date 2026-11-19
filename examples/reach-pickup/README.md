# Reach and pick up a resting prop

This C# example handles a known released item in a single-player world.
A `pickup` action begins a reach. The first-person hand opens as its IK
weight increases, then closes around the calibrated grip. Once the reach
has reached full weight, the game attempts pickup on each tick within its
retry window. The native engine checks the final wrist frame and four posed
skin vertices against the item before it retires the physics proxy.

A `cancel_pickup` press during the reach returns the hand to its incoming
pose and stops attempts. The same path eases out after a timeout. On a
successful pickup the engine attaches the item at its current world pose.
The game fades out IK using the final target it had stored before attachment,
so the hand can bring the item back without following its own attached prop.
A pickup already committed on the preceding tick starts the return with the
item held.

## Author the interaction

Author an independent rigid item drawable, a two-bone reach chain at slot 1,
and a configured finger layer that can sample a closed-to-open motion.
Attach and release an item with a fitted collision proxy, then let it settle
within the reachable area. Slot `target_node` binding is for an attachment
within a rig. This example uses a world-space point target from the released
prop's last committed transform and a rig-space end orientation. The final
contact check evaluates the resulting pose on the attempted tick.
[Two-bone IK](../../docs/TWO_BONE_IK.md) · [Item attachments](../../docs/ITEM_ATTACHMENTS.md)

Supply these typed fields when loading `Poima.Examples.ReachPickupGame`:

| Fields | Configuration |
| --- | --- |
| `Actor`, `Rig`, `Hand`, `Item`, `FingerMesh` | Live controller, rig wrapper, receiving hand node, rigid item drawable and skinned drawable IDs. |
| `Vertex1` through `Vertex4` | Cooked fingertip vertex indices on the same skinned drawable. |
| `GripX`, `GripY`, `GripZ`, `GripQx`, `GripQy`, `GripQz`, `GripQw` | The hand frame in item coordinates, with a unit XYZW rotation. Calibrate it from the equipped pose. |
| `PoleX`, `PoleY`, `PoleZ` | Elbow bend point in rig space. |
| `FingerLayer`, `OpenClip`, `OpenTime` | Layer slot, baked clip index, and the open key time in seconds. Time zero is the closed pose. |
| `AimTicks`, `RetryTicks`, `ReturnTicks` | Positive fixed-tick durations. The first attempt is at the end of the aim interval, followed by up to `RetryTicks` further attempts. |
| `FrameLimit`, `AngleLimit`, `SurfaceLimit` | Wrist position tolerance in metres, wrist angle tolerance in degrees, and fingertip distance tolerance in metres. |

The scene should give the hand a clear route to the grip. Calibrate the elbow
pole, sleeve and joint ranges for the chosen character. The sample uses a
rigid item and a right-handed orthogonal rig frame. It owns one known item
through one pickup and assumes no other system moves its binding during the
interaction. Discovery, obstruction planning and inventory rules are game
decisions. Use [shared world depth](../../docs/VIEWMODELS.md#share-depth-with-the-world)
where the hand and released prop should depth-test together.

## Build and verify

Build the module and matching CoreCLR bridge with .NET 10:

```sh
dotnet build examples/reach-pickup/Poima.ReachPickupGame.csproj -c Release -o build/reach-game
dotnet build managed/Poima.ManagedBridge/Poima.ManagedBridge.csproj -c Release -o build/reach-bridge
python tests/item_reach_pickup.py --binary <poima> --output build/reach-proof \
  --assembly build/reach-game/Poima.ReachPickupGame.dll \
  --bridge build/reach-bridge/Poima.ManagedBridge.dll --hostfxr <platform-hostfxr>
```

The verifier authors original analytic geometry and needs no external model.
It lets a real rigid body settle, starts and cancels one reach, and approaches
the target from beyond reach on the next attempt. It checks recoverable
misses, successful contact, collision body retirement, and byte-exact
continuation from a fresh host that loads the canceled reach in progress.
It retains literal requests and responses in a fresh output directory.

For Native AOT, publish the same project with
[`publish_native_gameplay.py`](../../scripts/publish_native_gameplay.py) and
`--type Poima.Examples.ReachPickupGame`. Replace the three CoreCLR paths
with `--descriptor <native-gameplay.json>` and choose a fresh output.
[Qualification and imported-rig comparison](../../docs/evidence/m2-reach-pickup.json)
