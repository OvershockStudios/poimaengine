# Compare hand and item frames

<!-- SPDX-License-Identifier: Apache-2.0 -->

Use `runtime.pose.compare` to measure whether a hand, attachment or prop
lines up at a committed tick. Each comparison takes a source frame, such as the
palm on a skinned rig, and a target frame, such as a grip point on the item.
Supply up to 32 pairs in one request to inspect both hands or several joints at
the same tick.

## Choose the frames

Discover the method with `world.describe` and get the current session and tick
from `runtime.status`. Find a rig joint’s live entity ID through the imported
model mapping. Use the entity origin directly when the joint or item already
has the desired pivot. Otherwise provide a rigid local offset in metres and a
normalized XYZW quaternion. The engine composes the full committed entity
matrix with the offset. Use calibrated offsets from the rig or item definition.

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "runtime.pose.compare",
  "params": {
    "session_id": "10000000000000000000000000000001",
    "tick": 42,
    "structure_revision": 1,
    "pairs": [{
      "source": {"entity": "a0000000000000000000000000000012"},
      "target": {
        "entity": "b0000000000000000000000000000012",
        "offset": {"position": [0.04, 0.01, 0], "rotation": [0, 0, 0, 1]}
      },
      "position_limit_m": 0.015,
      "rotation_limit_deg": 5
    }]
  }
}
```

Limits are optional and depend on the intended contact. Both limits are
inclusive. Position admits finite values in 0–1e9 metres and rotation admits
0–180 degrees. Offsets admit positions within ±1e9 metres per axis. Exact
session and tick guards always apply. Supply `structure_revision` when
matching an entity lookup or capture across spawn and despawn operations.

## Interpret the comparison

Rows follow request order. Each frame has its entity ID, composed
column-major world matrix and a basis classification. `world_delta_m` points
from target to source. `target_delta_m` expresses the same displacement in the
target's normalized axes. `position_error_m` is the length of the world delta.
`rotation_error_deg` is the magnitude of the shortest relative rotation.

A `regular` basis is right-handed and has perpendicular axes after column
normalization. Positive unequal scales can have a regular basis. Rotations
inherited through unequal scale can produce `sheared` bases. The threshold for
a normalized off-diagonal dot product is 1e-6. A left-handed orthogonal basis
reports `reflected`. A collapsed axis or normalized determinant magnitude
at or below 1e-8 reports `degenerate`. Treat these classifications as
measurement conditions. Even a small authored scale variation can prevent
a rigid angular comparison on a particular pose.

An angular answer requires two regular bases. A target-local displacement
requires a regular target basis. Unavailable quantities return null. The
corresponding `within_position_limit` and `within_rotation_limit` fields
report null when a limit was omitted or its metric is unavailable. Use the
world displacement and full matrices to investigate those cases.

The read covers committed gameplay transforms and scoped render-independent
measurements. Pair it with a native frame from the same tick to inspect the
visible hand, skinned contact and grip silhouette. Capture a player view and an
overhead or side view when useful. Viewmodel offsets, camera interpolation and
device presentation have their own observation paths. This read performs no
simulation steps, solver changes or render work.

For a fingertip or palm patch on the actual skinned geometry, measure the gap
against the item triangles at the same tick. [Surface measurements](SURFACE_MEASUREMENTS.md)

Malformed parameters return `-32602`. An unknown entity returns `-32004`.
A stale tick or structure revision returns `-32009`. An absent runtime or
mismatched session ID returns `-32030`. Composition outside numerical range returns
`-32040`. The method is outside runtime-game-session v1.

## Reproduce the original-rig check

Use the [pinned original CC0 Knight](CHARACTER_LIBRARY.md#original-source):

```sh
python3 tests/runtime_pose_frames.py --binary build/linux-game/poima \
  --source /path/to/Knight.glb --output build/pose-frame-check
```

With a Windows Vulkan executable, add `--gpu` for front and overhead
captures. Add `--windows-interop` when running from WSL. The check compares
the wrist, socket and sword frames to independent calculations from the
original glTF and inserts a known offset to exercise the limit results.
