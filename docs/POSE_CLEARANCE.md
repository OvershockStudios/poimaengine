# Check pose clearance

Use `runtime.pose.clearance` to check arm, palm, finger or equipment segments
against the current physics scene. Read it at the same committed tick as
[pose and grip observations](RUNTIME_OBSERVATION.md). This is a read-only
development method available with simulation and outside runtime game-session v1.

Each endpoint selects a live entity and an optional point in its local frame.
A missing point uses the entity origin. The engine reads the committed world
matrix, including its scale. The capsule radius stays in world metres. Two
coincident endpoints form a sphere.

```json
{
  "session_id": "00000000000000000000000000000001",
  "tick": 188,
  "structure_revision": 2,
  "segments": [{
    "endpoints": [
      {"entity": "00000000000000000000000000000002"},
      {"entity": "00000000000000000000000000000003", "point": [0.02, 0, 0]}
    ],
    "radius": 0.008,
    "previous": [[0.12, 1.39, -0.28], [0.15, 1.40, -0.31]]
  }],
  "ignore": ["00000000000000000000000000000004"]
}
```

Provide 1–16 segments, radii from 1 mm to 1 m, and up to 128 distinct live
entities to exclude. Exclude the owning character collider or intentionally
contacted equipment as appropriate. Discovery exposes the field bounds.
Each segment's cylinder half-height is limited to 10 m and capsule travel to
10 km per sweep interval. Oversized geometry or exceeded physics candidate
budgets reject the request. Stale tick and structure guards return `-32009`.

Results are in request order. Every row reports both committed world points,
`previous_hit`, `committed_hit`, `sweep_blocker` and `status`.

| Status | Meaning |
| --- | --- |
| `endpoint_only` | No previous points supplied and the committed capsule found no hit. |
| `blocked_endpoint` | At least one static endpoint test hit. Read both hit fields to determine which pose. |
| `blocked_sweep` | Both endpoint capsules were clear. The conservative sweep hit. |
| `clear` | Both endpoint capsules and the requested sweep found no hit. Identical poses require one static test. |

Leave `previous` absent when starting a take. Feed a row's `committed` points
back as `previous` at the next tick. The owner verifies the current tick and
structure revision. Preserve the earlier response to establish where those
caller-supplied previous points came from. At a rollback, seek or structural
change, start a new sequence unless the comparison is intentional.

The endpoint queries use the requested radius. With clear endpoints the
sweep follows a linear world-space path for each point over four intervals
against the **current** colliders. Each interval conservatively expands the
start capsule to enclose segment rotation and length change. A sweep hit
includes a 1-based interval and an interval-local hit fraction. The expanded
shape can report a conservative obstruction, including overlap at an
interval start. Those hits are distinct from exact endpoint overlaps.

Calibrate radii and local points for the intended skin or equipment coverage.
Checking bone capsules measures the chosen proxy shape. The result does
not reconstruct the actual joint arc, skinned surface, or earlier collider
positions, and it does not change pose admission. Use
[the IK clearance preview](TWO_BONE_IK.md#probe-a-reach-for-obstacles) for
a proposed solve within gameplay and
[guarded pickups](ITEM_ATTACHMENTS.md#recheck-the-arm-after-movement) for
ownership changes.

## Verify the installation

Run `ctest --test-dir <build-directory> -R '^runtime_pose_clearance_contract$'`
from a simulation-enabled build. The native fixture moves two offset points
across a thin static box. Both endpoint capsules clear it and interval 2 reports
the obstruction. A raised segment clears the same route. The fixture also checks
endpoint overlap, a sphere, stale requests and byte-identical saves surrounding
the reads.
[Linux, Windows and imported-rig results](evidence/m2-pose-clearance.json)
