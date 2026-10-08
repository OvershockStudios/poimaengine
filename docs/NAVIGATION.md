# Navigation

Poima can bake capsule-clearance-aware walkable space from authored static collision geometry and query routes through the native world service. Configure `POIMA_ENABLE_NAVIGATION=ON` to build the pinned Recast/Detour module. The authoring module is independent of simulation and graphics; compiled runtime queries additionally require simulation and a frozen world binding. Discovery exposes `navigation.available` for authoring and `navigation.runtime_available` when both navigation and simulation are built. A compiled game additionally needs a bound world; build availability alone does not grant its per-instance callback. A disabled authoring backend returns `-32003` for the static methods.

These APIs are evolving, outside the stable authoring-core v1 contract. Version 0.0.64 adds a durable world binding, checked export/save content closure and compiled gameplay queries. Navigation remains static and single-mesh: it does not provide a navigation editor panel, crowds, avoidance, dynamic obstacles, off-mesh actions, streaming or automatic rebaking.

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

A disconnected destination may have a valid endpoint projection but only a partial route; `reachable_end` describes where that corridor stops. Missing endpoints are unreachable. Exhausted node or output budgets remain explicit even if some corners are available; callers must not treat them as a complete route. No path or endpoint is silently truncated and labeled complete. Endpoint lookup uses a bounded linear polygon scan rather than the pinned upstream BV-tree builder, whose temporary-allocation failure path lacks a check. No navigation performance claim follows from this implementation. Detour scratch is independent per query; these authored reads have no stored path cursor, NPC state, save operation or simulation mutation.

## Bind a world

After baking, submit these `world.transact` parameters at the current authored revision to bind the returned asset:

```json
{"request_id":"00000000000000000000000000000064","base_revision":1,"ops":[{"op":"navigation.set","asset":"<64 lowercase hexadecimal asset hash>"}]}
```

The operation writes optional authored `navigation:{"asset":"<hash>"}`. `asset:null` removes the field. Profile, effective clearance and source fingerprint remain authoritative in the package. Binding uses the normal preview, retry, history, undo/redo and reopen paths. Validation checks the complete transaction candidate, not an intermediate operation. A final binding change adds `changed_world_fields:["navigation"]`; an unchanged binding or unrelated edit omits that key.

Non-null binding admission freshly reads and hashes the regular `.pnav` file, decodes its canonical checked encoding, and compares its source fingerprint with the candidate's static collision topology. This check does not trust the authored path-query cache. Later geometry edits may retain a stale binding, but frozen use refuses it with `-32009`. Bake and explicitly replace the binding, or clear it; start, export and load never silently rebake. Unrelated names/materials do not stale topology.

`world.inspect` reports the authored binding without loading packages. Frozen dependencies add `needs_navigation:true` and the actual `.pnav` hash/length; typed [asset references](ASSET_REFERENCES.md) expose a world-owned `navigation` edge at `/asset`. An unbound world omits the binding and navigation-only result fields. A backend-disabled build can inspect, query authored references and clear a shaped binding, while binding admission and frozen use require the actual backend.

## Query from compiled gameplay

A game declares `INavigationGame` independently of animation and movement markers. The callback is Tick-only and plans from a live native `CharacterController` foot position. It rejects dead/non-character handles and a capsule larger than the baked clearance. Reads see committed prephysics state; a staged `SetCharacterInput` command has not yet moved the character. A route does not provide stair assistance, jumping or a collision bypass.

`GameContext.FindNavigationPath(agent, goal, corners, extents, maxPolygons, maxNodes)` takes a caller-owned `Span<NavigationPoint>` of capacity 2–256. Defaults are extents `(2,4,2)`, 256 polygons and 4,096 search nodes. Compiled polygon budgets allow 1–256 and node budgets 32–4,096; finite goal/extents bounds match the authored query. The result contains the asset SHA, status, corner/polygon counts, requested start and nullable endpoint projections/distances. Only `Complete` grants a complete route; other statuses describe unreachable or partial/budget-limited results. Returned float-precision corners are copied values, not borrowed mesh storage. Failure leaves the supplied span unchanged.

At most eight actual native query attempts are allowed per Tick, including failed attempts; the ninth rejects and the next Tick resets the allowance. SDK argument rejection before entering the callback does not consume a native attempt. Queries do not bake or read package files during Tick. The mesh is immutable; each query uses bounded independent Detour scratch.

The named `navigation_query_v1` extension retains service epoch 7 and adds a callback at offset 216 in a 224-byte allocation. Request/result/point sizes are 64/128/12 bytes. Existing 176/192/208/216-byte negotiated views remain unchanged. The navigation marker grants only its named callback; implement `ICharacterInputGame` separately to steer using normal native movement. See the [compiled API contract](MANAGED_GAMEPLAY.md#plan-npc-routes-from-c) and [actual follower fixture](../tests/managed_navigation_gameplay/NavigationGame.cs). There is no `runtime.navigation.path` JSON-RPC adapter in this checkpoint.

The fixture stores up to ten XYZ corners in a generated capacity-30 float buffer plus cursor, fitting the existing 512-byte component limit. Scalar game state stores its goal and planning flags. The independent [replay harness](../tests/navigation_runtime_contract.py) observes a multi-corner detour around cover and native arrival without patching route fields or teleporting the NPC. See its [build/run instructions](../tests/managed_navigation_gameplay/README.md).

## Freeze, restore and export

Freeze freshly validates the binding against its source document and includes `.pnav` bytes in complete content identity. Runtime owns that checked immutable mesh. Saved-source restore validates the saved document's topology/package, even when the current authored world has changed or cleared its binding. Missing, corrupt or stale saved navigation refuses before activation and retains the current runtime. Route buffers/cursor, scalar gameplay state and native character state use existing snapshots; this extension adds no save format. Explicit component upgrades do not implicitly change navigation topology or replace bindings.

Matched SDK/bridge and Native AOT negotiation checks the backend and frozen binding before game construction and `Initialize`. Arbitrary DLL/module initialization is outside that guarantee. Compatible CoreCLR reload retains route state; a Native AOT library remains process-pinned and does not support compatible replacement.

Export requires a runtime descriptor advertising `features.navigation:true`; bundle verification repeats the gate. A navigation-enabled runtime must carry the inventoried `share/poima/licenses/RecastNavigation/License.txt`. The bound `.pnav` is part of exported content. These closure checks remain separate from graphics/player execution and clean-machine deployment qualification.

## Bounds and verification

Geometry is limited to 250,000 triangles and 750,000 vertices per bake, with the existing mesh-collider limits of 100,000 triangles/300,000 vertices per body. World-space coordinates must stay within ±1,000,000 meters. The XZ grid is bounded to four million cells and each quantized dimension below 65,535; the vertical extent is bounded to 1,024 voxels. A mesh supports at most 32,767 polygons, retaining Detour's reserved neighbor flag bit. Recast allocations have a 512 MiB admission ceiling, individual Detour operations have a 32 MiB allocation ceiling, and packages are limited to 16 MiB. These are admission bounds, not a guaranteed CPU time or total process memory budget.

For the authored `world.navigation.path` RPC, `max_polygons` allows 1–4,096, `max_corners` 2–4,096, and `max_nodes` 32–4,096. Defaults are 256/256/4,096. The compiled API above has the narrower 256-polygon/256-corner ceilings. Invalid parameters or unsupported geometry return `-32602`; revision/fingerprint conflicts return `-32009`. Validation or navmesh-construction failure publishes no package.

Native tests cover wall detours, narrow/wide capsule openings, disconnected surfaces, low headroom, checked reload and same-build bytes, malformed package indices, every Detour decode/query allocation failure stage, memory-ceiling cleanup and subsequent recovery. Protocol fixtures cover discovery/unavailable builds, artifact identity, source staleness, unchanged authored state, budget statuses, fresh owners, and cached-versus-uncached external corruption. At the 0.0.63 static checkpoint, Windows and Linux each passed these native checks plus six protocol groups over 70 RPCs with eight clean owner exits. A disabled build passes its availability group and intentionally skips five navigation-enabled groups. The checked package limit is 32,767 polygons, preserving Detour's reserved neighbor flag; a regression reproduces acceptance of the unsafe 32,768-polygon boundary in the earlier implementation. These fixtures do not measure game-scale performance. [Recorded qualification](evidence/m2-navigation.json).

Reproduce with a navigation-enabled build using `poima-navigation-test` and `tests/navigation_contract.py <binary> --navigation 1`. Use `--windows-interop` only when a WSL Python runner launches a Windows binary. The disabled contract uses `--navigation 0`.

The dependency is [Recast Navigation v1.6.0](https://github.com/recastnavigation/recastnavigation/tree/6dc1667f580357e8a2154c28b7867bea7e8ad3a7), pinned by commit and archive SHA-256. Only Recast and Detour are linked; upstream demos, crowd and tile-cache modules are excluded. Its [zlib license](https://github.com/recastnavigation/recastnavigation/blob/6dc1667f580357e8a2154c28b7867bea7e8ad3a7/License.txt) is installed with the engine's dependency notices. The module does not claim console qualification.

The [0.0.64 runtime-navigation record](evidence/m2-runtime-navigation.json) reports separate CoreCLR and actual published Native AOT follower runs. Each Linux run passes ten checks over 1,009 RPCs with five clean owner exits, including a navigation-disabled simulation host. Each Windows run passes nine checks over 998 RPCs with four clean owner exits. They cover native detour/arrival, eight-attempt quota and failed-attempt accounting, span preservation, pause, late-batch rollback, save/load, fresh-owner continuation, immediate replan and rejection on unbound/older hosts. CoreCLR additionally checks compatible reload; Native AOT checks replacement rejection. These bounded results do not qualify crowds, dynamic obstacles, arbitrary stairs, forged non-Tick callbacks, general malformed raw Runtime PODs, graphics/editor input, performance or deployment.

## Exported player check

The [shipping record](evidence/m2-navigation-shipping.json) uses the 0.0.64 runtime and original
compiled follower and static geometry, adding only a separate spectator and
camera for the player entry. An exported Windows Native AOT bundle is relocated
outside the checkout; its owned source is removed before execution. Loaded at
tick zero, gameplay completes one six-corner route and arrives during 1,400
hardware Vulkan replay ticks with zero NVRHI errors or runtime replacements.
The fixture has no visible mesh components, so this is player lifecycle and
compiled-navigation proof rather than a visible character demonstration.

A separate fresh headless world runs the route contract through the exact bundled
engine and artifact: eight checks, 987 RPCs and three clean owner exits. Its
precise native poses, rollback, save continuation and replan checks do not imply
in-player save UX. Both recorded runtime and artifact are from 0.0.64. [Reproduce](../tests/managed_navigation_gameplay/README.md#exported-windows-player).
