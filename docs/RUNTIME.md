# Fixed-step native runtime

Poima 0.0.5 adds an optional EnTT/Jolt runtime alongside authored-world editing. Agents can start a frozen scene revision, apply bounded input batches, inspect live entities and capture live camera views. Simulation uses native C++ and a fixed 60 Hz schedule; Python only drives the external tests. This is an initial controllable physics scene, not yet a packaged game. Poima 0.0.6 adds a [continuous native player and visual replay](PLAYER.md) over the same runtime.

## Build

```sh
cmake --preset runtime-headless
cmake --build --preset runtime-headless
ctest --preset runtime-headless
```

This preset downloads pinned, hash-verified Jolt 5.4.0 and EnTT 3.16.0 sources on first configuration. It needs no display or GPU. The existing `headless` preset remains an authoring-only build with no dependency downloads. Simulation is optional through `POIMA_ENABLE_SIMULATION`; discovery reports its actual availability.

For a native Windows executable with both simulation and Vulkan capture, use the workspace toolchain from [BUILD.md](BUILD.md), then:

```sh
cmake --preset windows-runtime
cmake --build --preset windows-runtime
cmake --build --preset windows-runtime --target poima-runtime-test
./build/windows-runtime/poima-runtime-test.exe
```

Both presets use two build workers. The physics baseline enables double-precision positions and Jolt's cross-platform determinism option, disables AVX/FMA and other optional CPU extensions, and uses a single-threaded job scheduler. Those choices simplify initial replay qualification; they are not a performance recommendation for every shipping game. The integration supplies `<type_traits>` when compiling the pinned Jolt release with LLVM-MinGW's newer libc++, without modifying downloaded source. [Dependency notices](../THIRD_PARTY_NOTICES.md).

## Try the physics room

The example defines a floor, blocking wall, falling crate, capsule controller, attached first-person camera and independent observer. Feed it to a new authoring world:

```sh
./build/runtime-headless/poima world build/physics.world.json < examples/physics-room.jsonl
./build/runtime-headless/poima world build/physics.world.json
```

Send these requests to the second process, one per line:

```json
{"jsonrpc":"2.0","id":1,"method":"runtime.start","params":{"session_id":"00000000000000000000000000000384","revision":1}}
{"jsonrpc":"2.0","id":2,"method":"runtime.step","params":{"session_id":"00000000000000000000000000000384","request_id":"00000000000000000000000000000401","expected_tick":0,"ticks":120}}
{"jsonrpc":"2.0","id":3,"method":"runtime.step","params":{"session_id":"00000000000000000000000000000384","request_id":"00000000000000000000000000000402","expected_tick":120,"ticks":180,"inputs":[{"entity":"00000000000000000000000000000064","move":[0,1]}]}}
{"jsonrpc":"2.0","id":4,"method":"runtime.entity","params":{"session_id":"00000000000000000000000000000384","id":"00000000000000000000000000000064","tick":300}}
```

The controller first lands, then walks forward until it meets the wall. The returned foot position is approximately y=−0.02, z=−2.46 in this fixture; the small penetration is within Jolt's configured contact slop. The authored spawn position stays `[0,1,2]`.

Using the Windows runtime executable in a native desktop session, a live observation can then be requested with:

```json
{"jsonrpc":"2.0","id":5,"method":"runtime.capture","params":{"session_id":"00000000000000000000000000000384","tick":300,"camera":"00000000000000000000000000000065","path":"build/player-at-wall.bmp"}}
```

Use camera ID `000000000000000000000000000000c8` for the independent observer. As with authored captures, the output must be a new file in an existing directory. Capture opens a bounded native window; a no-display render path remains future work; continuous play is available separately through `runtime.play`. Keep the same authoring process open while using a runtime session: EOF destroys its live state.

## State and operation contract

| Method | Behavior |
| --- | --- |
| `runtime.start` | Require the current authored `revision` and a fresh caller-generated `session_id`; validate and instantiate the complete runtime before publishing it. |
| `runtime.inspect` | Return tick, fixed timestep, entity/body/controller counts, source revision and whether authoring has changed since start. |
| `runtime.entity` | Inspect one stable entity ID, its live world matrix, body velocity, motion kind/target/progress and controller contact/look state; optional exact `tick` guard. |
| `runtime.raycast` | Query the closest live collider with an exact tick guard, world ray and optional ignored IDs. [Query contract](PHYSICS_INTERACTIONS.md). |
| `runtime.step` | Apply 1–600 fixed ticks with optional inputs for up to 32 distinct controller entities and up to 128 timed kinematic targets. Requires `session_id`, `expected_tick` and `request_id`. |
| `runtime.capture` | Render a selected live camera at an exact required `tick`, returning the frozen authored revision, session and tick with the image metadata. |
| `runtime.stop` | Drop live state and physics resources for the matching session. It does not write back to the authored world. |

Use `world.describe` for schema revision 25 and all parameter schemas. Input vectors are `move: [right, forward]` in [−1,1] and `look: [yaw-left, pitch-up]` in degrees. Movement magnitude is clamped to one, retaining analog magnitude below one. Move is held for the whole batch; look deltas, `jump` and `use` apply only on its first tick. A loaded [C# gameplay module](MANAGED_GAMEPLAY.md) receives these inputs before physics advances. Omitted input is neutral. Pitch is limited to ±85°, yaw wraps, and jumping requires ground support. Gravity is currently fixed at `(0,−9.81,0)` m/s². This is direct velocity control, including in air; acceleration tuning and a production movement model are not implemented.

Entity creation order, controller update order and hierarchy traversal are explicit and independent of EnTT's storage iteration order. Body state is synchronized into runtime transforms after a batch; camera and mesh snapshots derive from those transforms. Rendering owns copies and never changes physics state.

Authored edits remain allowed while a runtime runs. They do not silently update live physics. `runtime.inspect` marks the source stale; `world.capture` observes current authoring and `runtime.capture` observes the frozen runtime plus its evolved state. Stop and start with a fresh session ID to adopt authored changes. Live structural changes and gameplay reload integration are future work.

The last 32 successful step receipts support identical retries without advancing twice. Reusing a receipt ID with changed parameters fails; an evicted receipt's stale `expected_tick` still prevents duplicate simulation. Receipts live only for that runtime session. Repeating the active session's identical start returns the original start receipt without rewinding. A stopped session ID cannot be reused in the same authoring process; up to 10,000 distinct session IDs are retained. The most recently stopped session can be stopped again safely.

Parameter/reference failures occur before stepping. A physics batch also takes a trusted internal Jolt/character checkpoint. A reported update/capacity failure restores that checkpoint, controller angles, kinematic targets/progress, transforms and tick before reporting failure. The rollback path is tested with a real contact-capacity overflow. This internal checkpoint is not a persistent save format and never accepts caller-supplied binary bytes.

## Physics components

| Component | Fields and restrictions |
| --- | --- |
| `BoxCollider` | Local `half_extents` in meters, `motion: "static"`, `"dynamic"` or `"kinematic"`, `mass` in kg, `friction` and `restitution`. All fields are required. Entity/world scale multiplies extents. Static bodies can inherit a nonsheared, nonmoving hierarchy; static colliders below a moving body/controller are rejected. Dynamic and kinematic bodies must be roots. |
| `MeshCollider` | Explicit cooked model `asset` and `primitive`, contact `friction` and `restitution`. Static indexed triangles preserve geometric openings. See [mesh collision](MESH_COLLISION.md) for winding, geometry budgets and validation. |
| `CharacterController` | `radius`, total `height`, `speed`, `jump_speed`, and stable `camera` ID. The entity origin is at its feet. It must be an unscaled root rotated only about Y; the camera must be a direct child with a Camera component. |

Characters use Jolt's rigid-body `Character` capsule, not `CharacterVirtual`. They collide with static/dynamic/kinematic boxes, static mesh front faces and other characters. Camera pitch affects the attached camera; yaw controls movement and character orientation. Controllers and BoxCollider cannot coexist on one entity. MeshCollider cannot combine with either component; this combination is rejected at authoring time. Complete physics references/geometry and hierarchy restrictions are checked during runtime preparation, allowing unresolved asset references to remain authorable for repair.

Collider half extents, both authored and after scale, are limited to 0.001–10,000 meters. Mass is (0,1,000,000], friction [0,2] and restitution [0,1]. Controller radius is 0.05–2 meters, height must exceed twice the radius and be ≤4 meters, speed is (0,30] m/s, and jump speed is [0,20] m/s. Initial physics positions are bounded to ±1,000,000 meters. These are implementation guards, not a qualified large-world envelope. Sheared collision geometry is rejected rather than approximated as a box.

The initial world allows 10,000 entities, 4,096 physics bodies and 32 characters. Jolt pair/contact capacities are 8,192 each; dense overlap can hit those limits before the body limit. Temporary physics storage starts at 16 MiB with an allocation fallback, so 16 MiB is not a hard total-memory cap. Continuous collision detection is enabled for dynamic bodies and controllers. Collision layers currently distinguish static and moving objects only.

## Errors and qualification

Missing simulation support reports `-32003`; invalid start configuration/JSON input reports `-32602`; session mismatch or absence `-32030`; another active start `-32031`; stale tick/revision `-32009`; reused request/session IDs `-32010`; missing runtime entities `-32004`; and a rejected native step or physics failure `-32040`. A failed start leaves authoring intact. Existing capture-specific errors still apply.

The native tests check landing, wall blocking, jumping, camera look, exact same-build replay across different command chunking, invalid input rejection, snapshot identity and rollback of all 150 bodies in a contact-overflow fixture. Black-box protocol tests check retries, stale observations, frozen authored/live state, failed-start repair, process-restart replay, receipt eviction and repeated runtime lifetimes. GPU checks compare falling-crate pixels and camera poses against inspected live state. [Recorded evidence](evidence/m2-runtime.json).

The recorded 491-tick fixture produced exactly equal numeric results for all eight inspected entity/camera states on Linux/GNU 15.2 and native Windows/Clang 23.1.2. It covers landing, wall collision, jumping, camera look and analog movement. This comparison uses parsed numeric values, not a binary state comparison. Determinism is qualified by these fixtures and exact build settings; arbitrary content, other toolchains, multithreaded scheduling and long-running multiplayer rollback need separate qualification. The controller uses Jolt's quaternion/SIMD routines rather than platform libm trigonometry for movement.

Native Windows Vulkan captures passed on both the NVIDIA and AMD GPUs. View the [spawn scene](evidence/m2-runtime-spawn.png), [settled crate](evidence/m2-runtime-settled.png) and [attached first-person camera](evidence/m2-runtime-first-person.png). These show the basic box renderer and physics observation path, not the planned final graphics.

An initial [C# gameplay API](MANAGED_GAMEPLAY.md) now supports typed state, use-driven interactions and compatible-field reload. General save migrations and gameplay save commands, clocks beyond the tick counter, seeded random streams, general component/event APIs, stairs, crouching, swimming, moving-platform handling, configurable input actions, render interpolation and packaging remain unimplemented. Earlier C# reload/shipping lab measurements remain separate experiments. M0, M1 and M2 remain open.

Static mesh geometry and PBR factors are frozen with the runtime definition. Imported geometry does not automatically supply a collider. See [static assets](ASSETS.md).

[Lights and environment settings](LIGHTING.md) are frozen at runtime creation. `runtime.lighting` exposes their resolved state with an optional tick guard; light poses follow their runtime entity hierarchy. Authoring a different intensity or exposure affects future sessions, not the current one.

Poima 0.0.13 adds [collider raycasts and timed kinematic motion](PHYSICS_INTERACTIONS.md). The sliding-door fixture verifies collision/query/render agreement, child motion and safe motion-state rollback. Poima 0.0.14 exposes this native interaction surface through the integrated C# module; the earlier managed labs remain separate fixtures.

Poima 0.0.15 adds [native acoustic snapshot observation](AUDIO.md) at a guarded session/tick. Clip and acoustic settings are frozen with the runtime definition; geometry/emitter/listener poses come from current live transforms. These offline captures do not advance physics or play through a device.

## Runtime animation

Version 0.0.21 adds editable rig bindings and fixed-tick clip playback. `runtime.step` accepts an optional `animations` array of full playback commands, committed or rolled back together with physics, gameplay and audio state. `runtime.entity` includes the current `local_transform` and optional rig `animation` state. [Runtime animation](RUNTIME_ANIMATION.md) documents bindings, seek/pause/loop/rest semantics, authored baselines, rendering and ownership limits.

## Portable runtime snapshot foundation

The C++ runtime has an experimental logical checkpoint API. The [save-slot service](#durable-save-slots) adds storage and guarded activation around it. A typed C# save command and dedicated editor save/load window remain unfinished. Stopping an ordinary session still discards its unsaved runtime state.

```cpp
// frozen_definition and content_sha256 come from the trusted host/content system.
const auto bytes = runtime.save_snapshot(content_sha256);
auto staged = poima::Runtime::from_snapshot(frozen_definition, content_sha256, bytes);
// A host may activate staged only after its own session/presentation preparation.
```

For a world with gameplay, pass the trusted `GameplayConfig` as the fourth argument. The snapshot contains no executable paths. Restore requires the same backend, assembly image hash, type and complete schema; there is no automatic migration, CoreCLR-to-Native-AOT interchange or renamed-field compatibility in this format. All registered values are restored by name and type, including decimal-string `int64` values. Entity fields must resolve in this world or be the all-zero null handle. Gameplay `Initialize` is skipped on restore. Loading the host-selected module still invokes its ordinary constructors/static initialization, whose external side effects are outside the world transaction.

The host supplies the lowercase SHA-256 of its **complete frozen authored content and referenced asset closure**, and supplies the original `RuntimeDefinition`. This low-level API compares that identity; it does not compute or independently verify the supplied content closure. Reusing a digest for changed content violates the API contract. The original definition preserves authored animation baselines and camera orientation. World identity, authored revision, exact entity membership/parents and component classifications are also checked.

| State | Checkpoint behavior |
| --- | --- |
| World | Fixed tick, stable string entity IDs and frozen authored revision; the current runtime has no spawn/despawn. |
| Physics | World poses, linear/angular velocities and awake/asleep state; geometry and material settings come from the frozen definition. |
| Character | Pose, velocity, yaw and pitch; ground support is queried again after all bodies are restored. |
| Kinematic motion | Original start/target poses, total duration and elapsed ticks; loading does not restart an in-progress door movement. |
| Animation | Original clock anchors, source/destination controls and interrupted-fade local poses, including immutable frozen sources. |
| Sound | Logical voices, IDs, allocator, start/stop times and gains; clips resolve against trusted emitter content hashes. |
| Gameplay | Exact module/schema binding, complete supported scalar/entity values and gameplay revision. |

`from_snapshot` constructs a separate runtime and returns it only after validation. Failures destroy the candidate and leave an existing runtime untouched. It does not deserialize bytes through Jolt's internal recorder. Physics solver warm starts, contact caches, sleep timers, rendering resources and audio DSP history are rebuilt, rather than persisted. Future contact trajectories need not be bit-identical after loading. A host activating the candidate must separately reset presentation/audio streams, input, session IDs and retry receipts; this function does not replace a `WorldSession` or editor session itself.

The version-1 diagnostic JSON envelope carries a SHA-256 of its canonical payload. Checksums detect corruption, not deliberate save editing or authenticity. Decode rejects duplicate/unknown fields, incomplete entity/field sets, invalid references and numbers, mismatched content, excessive nesting and oversized data. Whole snapshots are limited to 64 MiB, 32 nesting levels and two million parser events; animation state is additionally limited to 16 MiB and sound state to 512 KiB. Counts retain the runtime's existing limits. Snapshot IDs contain 1–256 UTF-8 bytes without NUL (authored worlds already enforce their stricter 32-hex identities). Live state outside the supported save bounds is rejected during export instead of producing an unusable checkpoint.

This synchronous implementation allocates JSON and a staged runtime; it is not a hitch-free asynchronous save system. It has no persistence policy for unimplemented general components, inventories, spawn/despawn, streamed regions or future environmental systems. Migrations, cancellation, performance budgets and platform storage adapters remain separate work.

Native tests cover motion/character continuation, interrupted animation, sound, corrupt data and content binding. `poima-runtime-save-test --write-fixture PATH` and a separate `--read-fixture PATH` process qualify an explicit test file round trip; those test-only file operations do not implement engine save slots. The separate `poima-runtime-save-gameplay-test HOSTFXR BRIDGE ASSEMBLY TYPE` (or `--native DESCRIPTOR` for Native AOT) uses the checked-in managed fixture to exercise real C# tick continuation and collectible module lifetimes. [Snapshot evidence](evidence/m2-runtime-snapshot.json) records the qualified configurations and limits.

## Durable save slots

The native world service exposes `save.status`, `save.configure`, `save.inspect`, `save.write` and `save.load`. These operate through `poima world`, shared sessions and the editor's native service. Discover their complete parameter schemas with `world.describe` (schema revision 27). A dedicated human save window and a typed gameplay `Save`/`Load` API are not implemented yet.

Create an ordinary directory outside the asset store, then configure its path. Relative paths resolve beside the world document. Configuration is session-local; reopening requires configuration again. Packaged games additionally reject roots and derived slot paths inside their immutable bundle. Hosts constructing a read-only `WorldSession` pass the full bundle root as the third constructor argument; without it, protection covers only the world file's parent directory.

```json
{"jsonrpc":"2.0","id":1,"method":"save.configure","params":{"request_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","expected_generation":0,"root":"saves"}}
{"jsonrpc":"2.0","id":2,"method":"save.inspect","params":{"slot":"checkpoint"}}
```

The directory must already exist; the engine creates a `slot-checkpoint` child on the first write. Slot names contain 1–64 lowercase ASCII letters, digits, underscores or hyphens. Paths with symbolic links or Windows reparse points are rejected. Inspecting a missing slot creates no files and reports generation 0. Each slot has a nonblocking OS lock; simultaneous access reports failure instead of waiting indefinitely.

Before saving, inspect the runtime and slot. Supply the configuration generation, slot generation, runtime session, tick and gameplay revision from those observations:

```json
{"jsonrpc":"2.0","id":3,"method":"save.write","params":{"request_id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","configuration_generation":1,"slot":"checkpoint","expected_generation":0,"session_id":"11111111111111111111111111111111","expected_tick":300,"expected_gameplay_revision":0}}
```

The first successful write reports generation 1, SHA-256 and byte count. These example runtime values are placeholders for an actual active session. A slot retains the latest 32 write receipts across process restarts. Retrying the same request returns its original committed generation before checking live runtime guards, so a lost response can be recovered after the simulation advances or closes. A receipt does not guarantee that its historical payload is still retained; inspect `current`, `previous` and `selected` for availability. After reopening, refresh the session-local `configuration_generation` guard; it routes the request but is excluded from its persisted semantic identity. Other changed parameters under the same retained request ID are rejected. Once a receipt expires, its old generation guard prevents a duplicate write.

Restore supplies guards for both the current authored world and the runtime being replaced, plus a fresh runtime ID:

```json
{"jsonrpc":"2.0","id":4,"method":"save.load","params":{"request_id":"cccccccccccccccccccccccccccccccc","configuration_generation":1,"slot":"checkpoint","expected_generation":1,"revision":1,"expected_session_id":"11111111111111111111111111111111","expected_tick":300,"expected_gameplay_revision":0,"new_session_id":"22222222222222222222222222222222"}}
```

When no runtime is active, the three `expected_session_id`, `expected_tick` and `expected_gameplay_revision` guards must all be null. A successful load starts at the saved tick under the new session ID. Configuration/load retry receipts are session-local and bounded to 32 combined entries. A retained load retry returns its receipt without replacing the runtime again.

Each save contains the frozen authored definition and logical snapshot, with a total 64 MiB limit. The service computes a content identity from that definition and its full typed asset inventory. Hidden meshes, collision-only geometry and disabled emitters still contribute their referenced assets. Start and restore read and validate these files with fresh caches. Missing, corrupt or incompatible assets reject the load. Assets remain external: the save is not a self-contained asset archive.

Loading stages a separate runtime against the **saved** definition. Current authored edits and their revision/history remain unchanged. If the saved definition differs, the result reports `source_stale:true`. Failure preserves the existing runtime. Success invalidates its old session, input and runtime receipts. In the editor, saving/configuration/loading requires paused playback; restored sessions remain paused and derive their hierarchy from the saved runtime. Stop still discards unsaved changes.

Saved bytes never select executable paths. By default, restore reuses the active runtime's trusted gameplay configuration. A fresh process must supply `gameplay` using the existing CoreCLR selection (`hostfxr`, `bridge`, `assembly`, `type`) or a Native AOT `descriptor` with optional `expected_descriptor_sha256`. Explicit `gameplay:null` selects no module. Backend, image, type and schema must match the saved state. Ordinary trusted constructors/statics can execute during staging even though gameplay `Initialize` is skipped.

### Publication and recovery

Each write creates and flushes an immutable payload, verifies it by reading it back, preserves the previous committed manifest, and publishes a checksummed manifest by replacement. Normal operation retains two payload generations. POSIX also flushes directory changes. Windows flushes files and uses `ReplaceFileW` for an existing manifest. These mechanisms and injected process exits do not establish device/filesystem power-loss guarantees.

`save.inspect` exposes verification and recovery status. If the newest payload is damaged, reads can select the verified prior payload; loading it requires `allow_recovery:true`. Writing after that condition requires `acknowledge_recovery:true` and preserves the failed generation separately as `quarantined`. A second corruption while one generation is quarantined blocks further recovery writes rather than discarding evidence. A corrupt manifest can fall back to the prior committed manifest for explicit read recovery only; writes refuse because newer receipt history may be missing. New slots or deliberate external archival/repair remain necessary in those cases. Orphan staging files are never promoted to committed saves.

Checksums detect accidental corruption, not authenticity. Wrong state/generation guards report `-32009`, conflicting request IDs `-32010`, and storage/content failures `-32070`. A failure after publication can have an uncertain result; retry the identical write request to resolve it. `cleanup_pending` reports retained staging debris when cleanup could not finish.

This is synchronous, bounded storage for the current fixed entity set. Autosave scheduling, general schema/content migrations, spawn/despawn persistence, platform/cloud providers, cancellation and large-save performance qualification remain unfinished. Runtime physics and audio reconstruction retain the snapshot limits above; loading is not a promise of bit-identical future contact simulation.
