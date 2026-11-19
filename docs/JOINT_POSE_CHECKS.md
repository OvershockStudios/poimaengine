# Check committed joint angles

<!-- SPDX-License-Identifier: Apache-2.0 -->

Use `runtime.pose.joints` to find an animation layer or IK goal that moves a
joint beyond a calibrated range. Select model node indices on a live
`AnimationRig`. The read reports the local rotations that survived animation
blending, procedural updates and physics ownership. Each joint has its own
reference rotation, constraint axes and limits. A `PhysicalRig` is optional.

## Set a reference and limits

Use the rig wrapper ID and find node indices on the cooked model. At most 64
distinct nodes on one rig fit in a request. The default reference is the
world-authored node's local rotation frozen when its rig instance starts.
To measure from an equipped pose, supply its calibrated node-local
`reference_rotation`. The reference and committed rotation must use the same
parent space. The reply includes the live node and its parent.

`limit_frame` places the constraint axes in the reference node's coordinates
and defaults to identity. Its X axis is the hinge or twist axis. Its Y and Z
axes define the swing cone. Supply normalized XYZW quaternions. The relative
rotation uses active right-handed quaternion multiplication in this order.

```text
inverse(limit_frame) * inverse(reference_rotation) * local_rotation * limit_frame
```

```json
{
  "session_id": "10000000000000000000000000000001",
  "tick": 42,
  "structure_revision": 1,
  "rig": "a0000000000000000000000000000012",
  "joints": [
    {
      "node": 7,
      "limits": {"kind": "hinge", "minimum": -0.1, "maximum": 0.8},
      "limit_frame": [0, 0, 0.7071067811865476, 0.7071067811865476]
    },
    {
      "node": 11,
      "limits": {
        "kind": "swing_twist",
        "swing_y": 0.6,
        "swing_z": 0.35,
        "twist_min": -0.4,
        "twist_max": 0.5
      }
    }
  ]
}
```

All angle limits and results use radians. Hinge and twist intervals contain
zero, lie within ±3 radians, span at least 0.01 radians, and have at least
one endpoint at least 0.01 radians from zero. The swing half-cones each lie
in 0.01–3 radians and use the [same swing ellipse as the physical target fitter](PHYSICAL_ANIMATION.md).
Choose and verify plausible ranges for each rig and action.

## Read a failure

- `within_limits` admits the measured rotation. The fit preserves the local
  rotation and the correction is zero. The inspection absorbs corrections up
  to 1e-6 radians to account for cooked key quantization. The response reports
  this tolerance. The physical drive API keeps its existing strict admission.
- `outside_limits` reports the suggested fitted local rotation and a shortest
  correction angle. The flags identify off-axis hinge swing, bounded twist
  and swing-cone projection. Poima retains the committed pose.
- `indeterminate` reports the shortest rotation magnitude, the measured
  relative quaternion and a reason. The signed twist, swing and fit are null.
  The chart can be indeterminate near 180° swing or the signed twist cut.

Rows follow request order. A failed fit leaves the other rows available.
The total status reports an outside joint first, followed by an indeterminate
joint and then full admission. The fit describes a coordinate-space projection.
It leaves translation, scale, surface contact and temporal behavior for
separate checks. A node-local sample also cannot report how many times a joint
spun between observations. Sample moving actions across ticks and compare with
the player view. [Frame comparisons](POSE_FRAME_COMPARISON.md) · [Surface measurements](SURFACE_MEASUREMENTS.md)

The method leaves world, live pose and save state intact. It requires an exact
session and tick. An optional structure revision guards against same-tick
topology changes. Missing rigs return `-32004`. Malformed limits and absent
node bindings return `-32602`. A stale tick or structure returns `-32009`.
An unavailable or mismatched session returns `-32030`. Invalid committed
rotation data returns `-32040`. Simulation-disabled
builds return `-32003`. The method sits outside runtime-game-session v1.
Use `asset.animation.physical_limits` for a separately authored physical rig's
limits on a single source clip. [Source pose diagnostics](PHYSICAL_POSE_DIAGNOSTICS.md)

## Reproduce the native check

```sh
python3 tests/runtime_joint_pose.py --binary build/linux-game/poima \
  --output build/joint-pose-check
```

Add `--windows-interop` when using a Windows binary from WSL. The test
checks a neutral and rotated limit frame, a signed hinge and cone projection,
an admitted blend followed by an out-of-range composite, and a changed joint
after live end-orientation IK. It retains the requests, results and
byte-identical saves taken around inspection.
