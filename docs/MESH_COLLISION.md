# Static mesh collision

`MeshCollider` gives an imported, unweighted model primitive static triangle collision. Real gaps in fences, railings and open stairs remain gaps; the engine does not replace the mesh with a convex hull. Physics contacts and ray queries use Jolt's indexed mesh acceleration structure. Rendering and collision are explicit independent references, so invisible and collision-only objects work too.

This first implementation is static. Moving or deforming meshes, separate movement and weapon-query channels, finite-radius projectile sweeps and texture-cutout collision masks remain future work. A transparent part of a texture does not remove its underlying collision triangle.

## Authoring

Import a supported glTF/GLB through `asset.import`. On a chosen entity, set:

```json
{
  "op": "component.set",
  "id": "<entity ID>",
  "type": "MeshCollider",
  "value": {
    "asset": "<64-character cooked model hash>",
    "primitive": 0,
    "friction": 0.5,
    "restitution": 0
  }
}
```

Send this operation in a normal revision-guarded `world.transact`. All four fields are required. `primitive` is a zero-based index from `asset.inspect`; it is not a source node index. Model-node transforms are not implicitly applied: the collider uses its entity's authored hierarchy, like `StaticMesh`. Use the corresponding instantiated primitive entity to reuse its placement.

The desktop Inspector offers **Add static collision** on a `StaticMesh`. It copies that primitive reference into a new collider in one undoable transaction. The expanded Mesh Collider section edits its asset, primitive and contact material, and offers removal. Attach/remove require stopped playback and clean Inspector drafts. Conflicts and invalid drafts remain visible rather than being discarded. Imports never attach collision automatically.

| Field | Range / meaning |
| --- | --- |
| `asset` | Existing cooked model package; exact lowercase SHA-256 identity. |
| `primitive` | Integer 0–9999; must exist in that model. |
| `friction` | 0–2 contact friction. |
| `restitution` | 0–1 contact restitution. |

The entity cannot also contain `BoxCollider` or `CharacterController`. Ordinary static parent transforms and positive nonuniform scale are supported. Sheared hierarchies, moving-body/controller ancestors and animation-owned transforms reject. There are no motion or mass fields in this component.

## Geometry and queries

Contacts use front-facing triangles, according to source winding. Give floors upward-facing surfaces and closed obstacles outward-facing surfaces. Open shells do not become solid volumes. Rays deliberately hit both sides and report the same winding normal from either side; the normal is not automatically flipped toward the ray.

`runtime.raycast` retains its session/tick guard, nearest-hit selection and ignore list. Hit results add `triangle`: the zero-based ordinal of the original index triple in the cooked model primitive, before the collision tree reorders triangles. It is `null` for a box or character. Equal hit fractions choose the lexicographically smallest entity ID, then the smallest source triangle ordinal within a mesh. A mesh surface hit has its winding normal even at zero distance; solid primitive origin-inside hits retain `normal: null`.

The existing C# ray API uses the same precise query geometry. Its current ABI does not expose the triangle ordinal; native and JSON clients do. No managed ABI layout change is required to obtain accurate hits.

Input geometry must have finite positions, valid triangle indices, no skin weights, no zero-area triangles and no duplicate triangles, including reversed duplicates. Invalid geometry fails rather than being silently repaired. Shared validation also checks scaled float coordinates: the squared cross-product length must exceed 1e-12, matching the backend’s basic degeneracy threshold, and converted triangles must remain distinct. Jolt can additionally reject triangles that collapse during mesh quantization. Mesh collision is numerically bounded, not exact arithmetic; narrow-feature qualification applies only to the recorded fixtures.

| Budget | Current bound |
| --- | --- |
| Per body | 100,000 triangles; 300,000 vertices |
| World aggregate | 250,000 triangles; 750,000 vertices, counting instances |
| Source coordinates | ±1,000,000 source units |
| Scaled local coordinates | ±10,000 meters |
| Body position | ±1,000,000 meters |

These are resource/numeric guards, not performance or large-world precision guarantees. Each body currently cooks its own acceleration structure at runtime preparation; a persistent collision cache and shared shape optimization remain future work. Existing body/contact budgets still apply.

## Validation, persistence and export

Authoring immediately validates field shapes/ranges and incompatible components. Asset lookup and complete static geometry validation occur when preparing a runtime or export dependency set; authoring can therefore retain an unresolved reference for repair. Animation ownership is also checked by the existing authored rig validator. Missing packages, unsupported weighted geometry and invalid primitive references report asset errors; bad topology, transforms and ownership report validation errors. Backend-specific mesh creation failures remain runtime errors.

Runtime startup builds a complete candidate before publication. A failed start does not publish a partial simulation or mutate authored data. The mesh and material are frozen with that runtime revision. Later authored changes take effect on the next start; existing batch rollback retains immutable mesh shapes.

World dependencies and game export include models referenced only by `MeshCollider`, even without a rendered mesh or original source file. Collider configuration persists in the authored world and participates in undo/redo. This is not an implementation of public game-save serialization.

## Verification

`tests/runtime_mesh_native.cpp` checks analytic gaps, triangle provenance, backfaces, transforms, contacts and failure rollback. `tests/mesh_collision_contract.py` exercises import, authoring, dependency/export boundaries and live queries through the public protocol. `tests/desktop_mesh_collider.py` checks native-backed Inspector actions and queries through a doorway authored in the GUI. These synthetic fixtures do not qualify production character movement or game-scale collision performance.

The [0.0.32 evidence record](evidence/m2-mesh-collision.json) includes both laptop GPUs, Windows native/protocol and actual export checks, and Linux runtime/authoring suites. The editor ran 76 semantic actions on each GPU. [Inspector screenshot](evidence/m2-mesh-collision.png).
