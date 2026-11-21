# Rigid mesh level of detail

Set `StaticMesh.lods` to reuse a drawable's transform and material with
progressively smaller cooked primitives as the camera moves away. Use the
[asset importer](ASSETS.md) to cook each mesh and inspect its primitive index.
The original `asset` and `primitive` give LOD0. Omit `lods` for a single mesh.

```json
{
  "asset": "<LOD0 cooked model hash>",
  "primitive": 0,
  "visible": true,
  "lods": [
    {"asset": "<LOD1 cooked model hash>", "primitive": 0, "min_distance_m": 18},
    {"asset": "<LOD2 cooked model hash>", "primitive": 1, "min_distance_m": 50}
  ]
}
```

Pass the component value to a guarded `world.transact` `component.set` operation
on an entity, or include it in a spawn template. Author one to four alternates
with finite, strictly increasing distances in `(0, 1000000]` meters. Each
primitive must have indexed triangles, no skin weights, and fewer triangles than
the preceding tier. Align all meshes in the same local coordinates. The renderer
uses the LOD0 material and texture maps with the selected mesh's normals, UVs
and tangents. An effective mapped material requires UV0 on each tier, and an
effective normal map requires valid tangent frames. A transaction that adds an
invalid chain or makes its textures incompatible is rejected before publication.

Each render submission chooses the last tier whose minimum is at most the
Euclidean distance between the active camera and the drawable's world-space
origin. A threshold equality selects the new tier. The selected primitive
supplies the bounds and geometry for the camera and all eligible shadow views.
It keeps the drawable's entity ID, transform and shadow controls. When a tier
changes, that object's motion history starts using its new geometry. Other
drawables keep their history. Selection is stateless and can switch repeatedly
if a camera oscillates across a threshold.

LOD applies to ordinary rigid world drawables. Camera-routed meshes, skinned
geometry and deforming snow tiles use their dedicated paths. Physics and
navigation use the explicitly authored collision geometry. Acoustic mesh
proxies and editor picking retain LOD0 geometry.

## Inspect and export

Read `world.describe` with `view: "component"` and `name: "StaticMesh"` for the
installed authoring schema. The `mesh_lod` capability indicates that the
build can render the chain. Headless authoring can resolve the assets and
verify the package closure.

`world.asset.references` reports each explicit alternate at
`/lods/<index>/asset` under its owning entity or template `StaticMesh`.
`world.dependencies` validates and includes every referenced cooked tier
even if the camera did not select it. A bundle with any LOD chain requires a
runtime descriptor with `features.mesh_lod: true`. The cooked model format
and gameplay save format remain compatible. An older authoring binary may
reject the additional component field.

`render_diagnostics.last_draws.mesh_lod` is present when the last completed
submission contains any LOD-enabled drawables. Its five-element
`tier_counts` array covers tiers 0 through 4 before camera and shadow culling.
`sample` lists the first `sample_limit` decisions in draw order as
`{"id": "...", "tier": 1}`. Hidden entities are excluded by scene extraction.
Use the existing camera and shadow triangle counters to inspect submitted work.
Already uploaded tier geometry stays cached while the chain is active.
This path does not generate proxy meshes, stream assets, blend across
thresholds, or measure game-scale performance.

## Verification

The authoring fixture cooks original analytic geometry, checks strict
admission and rollback, follows continuation cursors, verifies entity and
template closure, and rejects a missing alternate. Run it on a configured
native build:

```sh
ctest --test-dir build/linux-game -R '^rigid_mesh_lod_contract$' --output-on-failure
```

Supply an installed Windows runtime to run the additional Vulkan checks.
The fixture tests the exact threshold, projected silhouettes, LOD0 material
inheritance and camera/shadow triangle counts. It also takes a collision ray
through geometry that only exists in LOD0, then repeats the capture and ray
query in the relocated game with the original project path unavailable.

```powershell
python tests/rigid_mesh_lod_capture.py path/to/poima.exe --runtime-root path/to/runtime --output build/lod-capture --gpu 0
```

Use `--windows-interop` when starting a Windows executable from WSL.
Use `--lighting-path deferred` to test deferred rendering.
