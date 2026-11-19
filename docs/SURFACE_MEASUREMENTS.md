# Measure hand-to-item surface gaps

<!-- SPDX-License-Identifier: Apache-2.0 -->

Use `runtime.surface.measure` to check a chosen point on a rigid or skinned mesh
against the nearest point on an item mesh. The engine uses the committed joint
transforms and the drawable's skin palette at one guarded tick. It samples the
same indexed geometry used for drawing without opening a window.

Use this check alongside [grip frame comparisons](POSE_FRAME_COMPARISON.md).
Frame comparisons measure bones and authored offsets. Surface measurement
includes the vertex deformation from the actual skin weights.

## Select a surface point

Find the live drawable entity, such as the `SkinnedMesh` child of an instantiated
rig node. Select a source by cooked vertex index or by cooked triangle ordinal
and three barycentric weights in index order. Each weight is in 0–1 and the sum
must be within 1e-8 of one. Ordinals start at zero and refer to the cooked model
hash. Import can rewrite the original mesh's vertex indices.

Find the drawable's cooked model hash and primitive in its `SkinnedMesh` or
`StaticMesh` component. Call `asset.mesh.triangles` with `asset` and
`primitive` to enumerate the triangle ordinals and their three vertex indices
in winding order. Pass `offset` and `limit` to page up to 64 rows. Join
the resulting indices with the vertex positions and skin weights from
[`asset.animation.sample`](ANIMATION_ASSETS.md#import-and-inspect). This keeps
surface selections tied to the model identity.

Select the target drawable and, optionally, a list of its triangle ordinals.
Use a handle-only triangle selection when the blade or guard shares the same
drawable. Omitting the list checks the entire target. Up to 32 pairs and
32,768 target triangle checks fit in one request, including repeated searches.
Each explicit target list permits 1–4,096 distinct ordinals. For exact distance
ties the lowest ordinal wins, independent of request order.
`triangle_checks` reports the total number of target triangles examined.

```json
{
  "session_id": "10000000000000000000000000000001",
  "tick": 42,
  "structure_revision": 1,
  "pairs": [{
    "source": {
      "entity": "a0000000000000000000000000000012",
      "triangle": 7,
      "barycentric": [0.4, 0.3, 0.3]
    },
    "target": {
      "entity": "b0000000000000000000000000000012",
      "triangles": [12, 13, 14, 15]
    },
    "distance_limit_m": 0.006
  }]
}
```

Use the current session, tick and live entity IDs in the method parameters.
If a drawable was spawned or replaced, include the structure revision from the
same observation. Targets can be rigid or skinned indexed drawables. A mesh
component with `visible:false` or an implicit render box rejects. Source and target
can share a drawable. Supply disjoint triangle regions when measuring one part
of a mesh against another.

## Interpret the measurement

Each row reports the sampled world-space source point and the closest target
point with its triangle ordinal and barycentric coordinates. `world_delta_m`
points from target to source. `distance_m` is its length. The optional
`within_distance_limit` result compares that length with an inclusive limit in
0–1e9 metres. Omit the limit if you only need a measurement.

Triangle normals follow winding in world space. `normal_offset_m` is the signed
projection of the separation onto the target normal. An open or inconsistently
wound surface can have either sign. An inside/outside classification requires
additional geometry information. `normal_alignment` is the dot product of the
two triangle normals. It returns null for a vertex source or when either triangle
has no reliable normal. Collapsed triangles can still supply an edge or point
witness. They report a null normal and target normal offset when selected.

The read uses committed geometry before camera-specific foreground offsets,
clearance and presentation interpolation. Inspect matching native player and
side views for visible grip quality. Material alpha and rasterization can
change which parts of a sampled triangle are visible. This method measures a
selected point against the selected surface. Whole-hand intersection and
automatic contact corrections require separate checks.

Unknown entities return `-32004`. Malformed selectors, unavailable drawable
geometry and exceeded work limits return `-32602`. A stale tick or structure
revision returns `-32009`. A missing or mismatched session returns `-32030`.
Simulation-disabled builds return `-32003`. World coordinates outside ±1e12
or unevaluable skin transforms return `-32040`. Inspection leaves the committed
tick and saves alone. This method is outside runtime-game-session v1.

## Run the analytic fixture

```sh
python3 tests/runtime_surface_measure.py --binary build/linux-game/poima \
  --output build/surface-measure-check
```

On Windows use the native executable. From WSL add `--windows-interop`.
The fixture covers posed vertices, edge projection, collapsed faces, topology
and tick guards, target selection bounds and complete save equality around reads.
