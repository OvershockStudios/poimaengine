# Editable rigs and native clip playback

Poima 0.0.21 connects imported glTF rigs to ordinary authored entities, fixed-tick native playback and Vulkan compute skinning. Agents can inspect bones, edit their baseline transforms, set complete playback state, advance the simulation and capture the result through the same world protocol. This is a single-clip animation foundation; it does not provide an animation graph or a finished character controller.

`world.describe` schema revision 19 exposes the components and commands below. `animation_rig_authoring` is available in headless builds. `runtime_clip_playback` requires the simulation build; `gpu_skinning` requires the renderer. The broader `animation` feature flag remains false because the full planned system is unfinished.

## Authored entities

`asset.instantiate` accepts models containing skins or clips. It creates an ordinary wrapper and node hierarchy with stable derived IDs. Every source model node becomes an entity, including nodes outside the selected scene, so no animation target or rig binding disappears silently. Only primitives belonging to the selected scene become renderable children. Static-only asset instantiation retains its earlier behavior.

| Component | Complete value | Meaning |
| --- | --- | --- |
| `AnimationRig` | `asset`, `clip`, `time`, `speed`, `loop`, `playing` | Wrapper model reference and initial runtime playback state. |
| `RigNode` | `rig`, `node` | Wrapper entity ID and zero-based source model node index. |
| `SkinnedMesh` | `asset`, `primitive`, `visible`, `rig`, `node` | Weighted primitive, wrapper binding and source mesh-node index. |

`asset` is the immutable 64-character model content hash. Entity IDs are 32 lowercase hexadecimal characters. The initial wrapper has `clip:null`, `time:0`, `speed:1`, `loop:true` and `playing:false`. Primitive children also receive the imported material; existing material/texture editing remains available.

Each model node must have exactly one `RigNode` binding inside its wrapper. Bones retain normal names, parent references and `Transform` components. A node may be reparented within its own rig, provided the resulting hierarchy remains valid; its channels still target the same source node index and local transform fields. A skinned primitive must remain an identity-transform direct child of its mapped mesh node, and its asset/primitive must belong to that source node. Weighted geometry cannot be substituted into `StaticMesh`.

Use `entity.query` with component filters to discover the wrapper, nodes and primitive children, then `entity.get`/`world.transact` for inspection and editing. The normal transaction preview, revision checks, retry receipts and atomic validation apply to rig edits. Removing a required binding or introducing a conflicting component must be resolved within the same valid transaction.

`world.capture` renders the authored baseline, even when the wrapper specifies an initial playing clip. `asset.animation.capture` remains an isolated source-model pose preview. To observe live playback, start a runtime and use `runtime.capture` or the existing player/replay loop.

## Playback and observation

`runtime.start` freezes the authored definition at a revision. It samples each rig's configured initial state at tick zero. Authoring changes during a running session do not silently alter its hierarchy, models or baseline transforms; the runtime reports its stale source revision. Stop and start a new session to adopt those structural changes.

`runtime.step` accepts an optional `animations` array of at most 64 commands. Every command replaces the complete state and requires all six fields:

```json
{"entity":"<wrapper ID>","clip":0,"time":0,"speed":1,"loop":true,"playing":true}
```

`clip` is a zero-based index in that wrapper's model, or null for the authored rest pose. Time is finite and within 0–1e9 seconds. Speed is finite and within 0–8; negative/reverse playback is not supported. `loop` and `playing` are Booleans. Commands for nonexistent rigs or duplicate targets reject the batch.

The following protocol sequence assumes the model was imported and the world is at revision zero. Substitute the returned model hash and use an unused request/session ID for each operation:

```json
{"jsonrpc":"2.0","id":1,"method":"world.transact","params":{"request_id":"11111111111111111111111111111111","base_revision":0,"ops":[{"op":"asset.instantiate","id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","asset":"<model hash>","name":"Animated actor"}]}}
{"jsonrpc":"2.0","id":2,"method":"runtime.start","params":{"session_id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","revision":1}}
{"jsonrpc":"2.0","id":3,"method":"runtime.step","params":{"session_id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","request_id":"22222222222222222222222222222222","expected_tick":0,"ticks":60,"animations":[{"entity":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","clip":0,"time":0,"speed":1,"loop":true,"playing":true}]}}
{"jsonrpc":"2.0","id":4,"method":"runtime.entity","params":{"session_id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","tick":60}}
```

`runtime.entity` returns `local_transform` and the existing world matrix. Its `animation` field is null on ordinary nodes; wrappers return `clip`, effective `time`, `speed`, `loop`, `playing` and `duration`. Query a mapped bone entity to inspect its evaluated local pose. A runtime capture additionally needs an existing authored camera and a new output path.

Commands apply before the first requested tick. After N ticks, playing time is the requested time plus `N / 60 * speed`, wrapped by clip duration when looping or clamped at the end otherwise. A nonlooping clip reports `playing:false` at its endpoint and retains its final pose. Zero-duration clips remain at zero and stop. Speed zero retains time while preserving the requested playing flag.

Set `playing:false` with a chosen time to seek or hold a pose. Null clip always normalizes to time zero and `playing:false`, restoring the authored baseline. Each sample starts from that baseline; clip channels replace only their targeted local translation, rotation or scale. Switching clips cannot accidentally preserve a field animated only by the previous clip. Playback clocks use a command anchor tick/time, so partitioning identical fixed ticks into different requests does not accumulate different time increments.

The step's existing expected-tick check and retained in-session retry receipts also cover animation commands. Replaying a retained successful request does not advance the clock again.

## Ownership, validation and rollback

Rig nodes and their descendants own animation-driven transforms. That subtree cannot contain physics bodies, character controllers or a camera controlled by a character controller. Ordinary animated cameras are allowed. A physics-controlled outer wrapper may carry an animated rig; this does not implement extracted root motion or animation-driven colliders.

Skinned mesh acoustics are rejected because the current acoustic geometry path does not follow vertex deformation. Use an explicit separate rigid collision/acoustic proxy. The authoring schema rejects combining `SkinnedMesh` and `AcousticMaterial`.

Clip/clock state and local transforms join the existing batch checkpoint for physics, gameplay state and sound. A failed sample—including a cubic curve with an invalid intermediate scale—or an edited runtime hierarchy whose matrix exceeds finite ±1e12 bounds rolls back the whole requested step. Matrices are validated during runtime hierarchy evaluation, including reparented trees. Retrying a valid command after failure starts from the prior committed tick and state.

Snapshots own their palette data. Mesh-local palettes derive from current entity transforms as `inverse(mesh_world) * joint_world * inverse_bind`; later stepping cannot mutate an earlier snapshot. The renderer uses these palettes through the existing [compute skinning pass](GPU_SKINNING.md). A GPU-only deformation failure remains a rendering error; capturing a bad blend does not retroactively roll back previously committed simulation ticks.

## Native interfaces and bounds

`RuntimeEntityDefinition` adds optional `animation_rig`, `rig_node` and `skinned_mesh` bindings. `Runtime::step` takes a trailing `std::vector<AnimationCommand>`, and `Runtime::animation(id)` returns optional `RuntimeAnimationState`. `RuntimeEntityState` exposes the local transform and optional playback state. These are native C++ APIs; the C# gameplay service ABI does not yet expose animation controls.

`validate_runtime_animation(definition)` is built without Jolt and shared by authoring/runtime validation. `CompiledAnimation` validates and copies source curves once, retains the parent traversal, and supports a supplied authored TRS baseline. Runtime rigs sharing a model share the compiled sampler. CPU sampling does not require a graphics device or scan every source key for validation on every tick. This has not yet established production animation throughput or crowd budgets.

Current per-world limits are 128 rigs, 10,000 mapped nodes, 65,536 channels counted across rig instances, two million keys across unique compiled models, and 32,768 palette-joint entries across skinned primitive instances. The existing 10,000-entity limit includes wrappers, nodes, primitive children and unrelated entities. Per-model import limits and renderer memory limits also apply. A single runtime step advances 1–600 ticks and accepts at most 64 complete animation commands.

## Verification and remaining work

`tests/runtime_animation_native.cpp` covers analytic joint poses, independent instances, fixed-tick partitioning, authored baselines, immutable palettes, ownership checks and physics rollback after an invalid intermediate pose. `tests/runtime_animation_contract.py` exercises the authoring/runtime JSON protocol. `tests/runtime_animation_capture.py` compares live capture with the existing asset reference path. Executed platforms, results and hashes belong in [the runtime checkpoint evidence](evidence/m2-runtime-animation.json).

No C# animation controls, blending/state graphs, events, IK, retargeting, root-motion extraction, ragdolls, animation compression/streaming, motion vectors or previous-frame skinning are included in this slice. Direct FBX import is not implemented. Mixamo rigs/animations have not been qualified; a supported glTF conversion still needs actual import and visual validation, and separately rigged clips cannot be assumed compatible without retargeting. Native Linux graphics and console graphics remain unqualified.
