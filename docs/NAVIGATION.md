# Static navigation

Poima can bake capsule-clearance-aware walkable space from authored static collision geometry and query routes through the native world service. Configure `POIMA_ENABLE_NAVIGATION=ON` to build the pinned Recast/Detour module. It is independent of simulation and graphics. Discovery exposes `navigation.available`; a disabled build returns `-32003` for these methods.

These APIs are evolving, outside the stable authoring-core v1 contract. Navigation packages are not yet registered in the automatic game-export or save content closure; runtime asset binding is a separate integration. This implementation is a static, single-mesh navigation foundation. It does not provide a compiled gameplay navigation callback, a navigation editor panel, crowds, avoidance, dynamic obstacles, off-mesh actions, streaming or automatic rebaking.

## Bake collision topology

```json
{"jsonrpc":"2.0","id":1,"method":"world.navigation.bake","params":{"revision":1,"profile":{"radius":0.3,"height":1.8,"climb":0.3,"slope":45,"cell_size":0.15,"cell_height":0.1}}}
```

`revision` is required. The profile object and its fields are optional; the example shows their defaults. Units are meters, except slope in degrees. Radius must be 0.05–2, height greater than twice radius and at most 4, climb 0–2 and no greater than height, slope 0–60, cell size 0.025–1, and cell height 0.025–0.5. Choose a profile for the actual physical actor. Baking rounds radius and height upward, and climb downward, to whole voxel counts; returned `metadata.clearance` reports those effective values.

Only static `BoxCollider` and indexed, unweighted `MeshCollider` geometry participates. Rendering visibility and material textures do not affect navigation. Mesh-collider holes and fence openings are represented by their actual triangles, subject to voxel resolution and capsule clearance; texture transparency does not create physical openings. Moving colliders are counted in `ignored_moving_colliders` and excluded. A selected static collider beneath a moving or animation-owned ancestor, or with a sheared hierarchy, rejects the bake. Mesh winding remains significant: a reversed face is not automatically made walkable.

The result contains `revision`, `scope:"authored"`, `asset`, `source_fingerprint`, `metadata`, `reused` and `ignored_moving_colliders`. Metadata includes the profile, effective clearance, source triangle/vertex counts, grid dimensions, polygon count, and pinned backend identity. The bake publishes `<asset>.pnav` in the world's existing `.assets` directory. It uses a flushed temporary file followed by atomic replacement; power-loss directory durability is not promised. Identical input on the same build/platform produces a reusable content-addressed package. Cross-platform byte equality is not promised.

Bake is synchronous and may occupy the owner while it runs. For long bakes, use a dedicated native headless process rather than the human editor's active shared session. It does not advance or replace a live runtime, edit the authored world, change its revision/history, or resolve unrelated rendering/audio assets. A read-only world rejects bake with `-32081` and removes it from its discovery; inspect/path remain available.

## Inspect and query

```json
{"jsonrpc":"2.0","id":2,"method":"world.navigation.inspect","params":{"revision":1,"asset":"<64 lowercase hex characters>"}}
```

```json
{"jsonrpc":"2.0","id":3,"method":"world.navigation.path","params":{"revision":1,"asset":"<64 lowercase hex characters>","start":[-4,0.2,0],"end":[4,0.2,0],"extents":[0.5,0.5,0.5],"max_polygons":256,"max_corners":256,"max_nodes":4096}}
```

Both calls require the current authored `revision` and a checked package identity. The source fingerprint includes static collider IDs, geometry references and effective hierarchy transforms. It excludes names, render materials, friction and other values that cannot change collision topology. A changed revision alone does not invalidate geometry; a changed source fingerprint rejects with `-32009`. Moving collider state and spawned/runtime objects are not included: this is an authored static query, not an observation of current physical obstructions.

The owner caches one validated immutable mesh and the source fingerprint for the current revision. Each call checks that the package path is still a regular, unlinked file. It hashes/decodes bytes when loading a new asset; repeated reads use the immutable checked mesh. External modifications to an already-loaded package do not alter that owner; a fresh owner or loading a different asset validates bytes again. Bake also verifies existing bytes before returning a reused artifact. Missing, corrupt or malformed uncached packages return `-32050`. Packages store checked neutral polygon/detail arrays and reconstruct Detour data, rather than trusting serialized pointers or unchecked raw memory layouts.

`start` and `end` are world-space points. `extents` defines the endpoint search box, defaults to `[2,4,2]`, and allows 0.01–100 per axis. Projection can select a nearby floor or a surface across cover, so choose extents carefully and inspect `projected_start`, `projected_end` and their distance from the requested points. A missing projection is `null`, with its distance also `null`. A route is a sequence of navigation surface corners, not a motion command or collision bypass.

The response retains the common asset/source/metadata fields and adds `path` with:

- `status` and `complete`: only `complete` grants a complete route. Other statuses are `partial`, `unreachable`, `out_of_nodes` and `buffer_limit`.
- Requested/projected endpoints, projection distances, `reachable_end`, traversed polygon count and ordered `corners`.
- The applied polygon, corner and node budgets in `limits`.

A disconnected destination may have a valid endpoint projection but only a partial route; `reachable_end` describes where that corridor stops. Missing endpoints are unreachable. Exhausted node or output budgets remain explicit even if some corners are available; callers must not treat them as a complete route. No path or endpoint is silently truncated and labeled complete. Endpoint lookup uses a bounded linear polygon scan rather than the pinned upstream BV-tree builder, whose temporary-allocation failure path lacks a check. No navigation performance claim follows from this implementation. Detour scratch is independent per query; there is no stored path cursor, NPC state, callback, save operation or simulation mutation.

## Bounds and verification

Geometry is limited to 250,000 triangles and 750,000 vertices per bake, with the existing mesh-collider limits of 100,000 triangles/300,000 vertices per body. World-space coordinates must stay within ±1,000,000 meters. The XZ grid is bounded to four million cells and each quantized dimension below 65,535; the vertical extent is bounded to 1,024 voxels. A mesh supports at most 32,767 polygons, retaining Detour's reserved neighbor flag bit. Recast allocations have a 512 MiB admission ceiling, individual Detour operations have a 32 MiB allocation ceiling, and packages are limited to 16 MiB. These are admission bounds, not a guaranteed CPU time or total process memory budget.

`max_polygons` allows 1–4,096, `max_corners` 2–4,096, and `max_nodes` 32–4,096. Defaults are 256/256/4,096. Invalid parameters or unsupported geometry return `-32602`; revision/fingerprint conflicts return `-32009`. Validation or navmesh-construction failure publishes no package.

Native tests cover wall detours, narrow/wide capsule openings, disconnected surfaces, low headroom, checked reload and same-build bytes, malformed package indices, every Detour decode/query allocation failure stage, memory-ceiling cleanup and subsequent recovery. Protocol fixtures cover discovery/unavailable builds, artifact identity, source staleness, unchanged authored state, budget statuses, fresh owners, and cached-versus-uncached external corruption. Windows and Linux each pass these native checks plus six protocol groups over 70 RPCs with eight clean owner exits. A disabled build passes its availability group and intentionally skips five navigation-enabled groups. The checked package limit is 32,767 polygons, preserving Detour's reserved neighbor flag; a regression reproduces acceptance of the unsafe 32,768-polygon boundary in the earlier implementation. These fixtures do not measure game-scale performance. [Recorded qualification](evidence/m2-navigation.json).

Reproduce with a navigation-enabled build using `poima-navigation-test` and `tests/navigation_contract.py <binary> --navigation 1`. Use `--windows-interop` only when a WSL Python runner launches a Windows binary. The disabled contract uses `--navigation 0`.

The dependency is [Recast Navigation v1.6.0](https://github.com/recastnavigation/recastnavigation/tree/6dc1667f580357e8a2154c28b7867bea7e8ad3a7), pinned by commit and archive SHA-256. Only Recast and Detour are linked; upstream demos, crowd and tile-cache modules are excluded. Its [zlib license](https://github.com/recastnavigation/recastnavigation/blob/6dc1667f580357e8a2154c28b7867bea7e8ad3a7/License.txt) is installed with the engine's dependency notices. The module does not claim console qualification.
