# Editable rigs and native clip playback

Poima connects imported glTF rigs to ordinary authored entities, fixed-tick native playback and Vulkan compute skinning. Version 0.0.33 adds timed crossfades and transition inspection. Agents can inspect bones, edit their baseline transforms, set complete playback state, advance the simulation and capture the result through the same world protocol. This animation foundation supports two-pose transitions; it does not provide a layered animation graph or a finished character controller.

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

`runtime.step` accepts an optional `animations` array of at most 64 commands. Every command supplies the complete destination state and requires these six fields:

```json
{"entity":"<wrapper ID>","clip":0,"time":0,"speed":1,"loop":true,"playing":true}
```

`clip` is a zero-based index in that wrapper's model, or null for the authored rest pose. Time is finite and within 0–1e9 seconds. Speed is finite and within 0–8; negative/reverse playback is not supported. `loop` and `playing` are Booleans. Commands for nonexistent rigs or duplicate targets reject the batch. Optional `blend_ticks` selects a crossfade duration from 0 to 3600 fixed ticks (up to 60 seconds). Omission means zero, retaining immediate replacement; authored `AnimationRig` fields do not include this runtime-only setting.

The following protocol sequence assumes the model was imported and the world is at revision zero. Substitute the returned model hash and use an unused request/session ID for each operation:

```json
{"jsonrpc":"2.0","id":1,"method":"world.transact","params":{"request_id":"11111111111111111111111111111111","base_revision":0,"ops":[{"op":"asset.instantiate","id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","asset":"<model hash>","name":"Animated actor"}]}}
{"jsonrpc":"2.0","id":2,"method":"runtime.start","params":{"session_id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","revision":1}}
{"jsonrpc":"2.0","id":3,"method":"runtime.step","params":{"session_id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","request_id":"22222222222222222222222222222222","expected_tick":0,"ticks":60,"animations":[{"entity":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","clip":0,"time":0,"speed":1,"loop":true,"playing":true}]}}
{"jsonrpc":"2.0","id":4,"method":"runtime.entity","params":{"session_id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","tick":60}}
```

`runtime.entity` returns `local_transform` and the existing world matrix. Its `animation` field is null on ordinary nodes; wrappers return `clip`, effective `time`, `speed`, `loop`, `playing`, `duration` and nullable `transition`. Query a mapped bone entity to inspect its evaluated local pose. A runtime capture additionally needs an existing authored camera and a new output path.

Commands apply before the first requested tick. After N ticks, playing time is the requested time plus `N / 60 * speed`, wrapped by clip duration when looping or clamped at the end otherwise. A nonlooping clip reports `playing:false` at its endpoint and retains its final pose. Zero-duration clips remain at zero and stop. Speed zero retains time while preserving the requested playing flag.

Set `playing:false` with a chosen time to seek or hold a pose. Null clip always normalizes to time zero and `playing:false`, selecting the authored baseline as the destination. With no blend it restores that baseline immediately. Each sample starts from that baseline; clip channels replace only their targeted local translation, rotation or scale. Switching clips cannot accidentally preserve a field animated only by the previous clip. Playback clocks use a command anchor tick/time, so partitioning identical fixed ticks into different requests does not accumulate different time increments.

The step's existing expected-tick check and retained in-session retry receipts also cover animation commands. Replaying a retained successful request does not advance the clock again. Omitted and explicit zero `blend_ticks` normalize to the same receipt parameters.

## Timed crossfades

To transition to clip 1 over half a second, include a duration of 30 ticks:

```json
{"entity":"<wrapper ID>","clip":1,"time":0,"speed":1,"loop":true,"playing":true,"blend_ticks":30}
```

The transition starts at the current committed tick before the requested step. At elapsed tick `k`, destination weight is `k / blend_ticks`, clamped to one. A 30-tick fade is halfway after 15 ticks and complete after 30. Progress uses simulation ticks independently of clip speed or `playing`; pausing a clip holds its pose but does not pause its fade. Pausing the simulation holds both. Zero duration cancels any previous fade and replaces the pose immediately.

For an ordinary fade, source and destination clocks both advance according to their own playback settings. Each samples against the frozen authored baseline. Translation and scale mix linearly in local space; rotation follows the normalized shortest quaternion arc. This does not blend matrices, preserve foot contacts or automatically synchronize locomotion phases.

Interrupting an active fade takes its currently evaluated local pose as an immutable source, then transitions toward the new destination. The interrupted source is frozen rather than recursively retaining earlier transitions. The starting pose is continuous; velocity is not guaranteed continuous. Each rig retains at most one frozen source pose, bounded by the existing mapped-node budget. Endpoint evaluation uses the exact destination pose and stops sampling the expired source.

While a fade is active, `animation.transition` reports:

| Field | Meaning |
| --- | --- |
| `start_tick`, `duration_ticks`, `elapsed_ticks` | Transition anchor, requested duration and committed progress. |
| `weight` | Destination contribution from zero to one. |
| `source_frozen` | True for an interrupted transition's evaluated pose. |
| `source_clip`, `source_time`, `source_speed`, `source_loop`, `source_playing` | Outgoing clip state for an ordinary fade; all null for a frozen source. A rest source has null clip, zero time and false playing. |

The enclosing animation fields always describe the destination, not the blended pose. Inspect mapped node transforms or capture the runtime to observe that pose. `transition` is null when absent or complete. This is current-state inspection, not a retained timeline or scrubbable simulation history.

## Ownership, validation and rollback

Rig nodes and their descendants own animation-driven transforms. That subtree cannot contain physics bodies, character controllers or a camera controlled by a character controller. Ordinary animated cameras are allowed. A physics-controlled outer wrapper may carry an animated rig; this does not implement extracted root motion or animation-driven colliders.

Skinned mesh acoustics are rejected because the current acoustic geometry path does not follow vertex deformation. Use an explicit separate rigid collision/acoustic proxy. The authoring schema rejects combining `SkinnedMesh` and `AcousticMaterial`.

Clip clocks, transition state, immutable interruption poses and local transforms join the existing batch checkpoint for physics, gameplay state and sound. A failed sample—including a cubic curve with an invalid intermediate scale—or an edited runtime hierarchy whose matrix exceeds finite ±1e12 bounds rolls back the whole requested step. Matrices are validated during runtime hierarchy evaluation, including reparented trees. Retrying a valid command after failure starts from the prior committed tick and state.

Snapshots own their palette data. Mesh-local palettes derive from current entity transforms as `inverse(mesh_world) * joint_world * inverse_bind`; later stepping cannot mutate an earlier snapshot. The renderer uses these palettes through the existing [compute skinning pass](GPU_SKINNING.md). A GPU-only deformation failure remains a rendering error; capturing a bad blend does not retroactively roll back previously committed simulation ticks.

## Native interfaces and bounds

`RuntimeEntityDefinition` adds optional `animation_rig`, `rig_node` and `skinned_mesh` bindings. `AnimationCommand` adds optional-duration `blend_ticks` with a zero default. `Runtime::step` takes a trailing `std::vector<AnimationCommand>`, and `Runtime::animation(id)` returns optional `RuntimeAnimationState`. `RuntimeEntityState` exposes the local transform and optional playback state. C# gameplay exposes `GameContext.GetAnimation` and `SetAnimation` through service ABI version 3; see [managed gameplay](MANAGED_GAMEPLAY.md#control-animation-from-c) for queued-write timing, conflicts and reload behavior.

`validate_runtime_animation(definition)` is built without Jolt and shared by authoring/runtime validation. `CompiledAnimation` validates and copies source curves once, retains the parent traversal, and supports a supplied authored TRS baseline. Runtime rigs sharing a model share the compiled sampler. CPU sampling does not require a graphics device or scan every source key for validation on every tick. This has not yet established production animation throughput or crowd budgets.

Current per-world limits are 128 rigs, 10,000 mapped nodes, 65,536 channels counted across rig instances, two million keys across unique compiled models, and 32,768 palette-joint entries across skinned primitive instances. The existing 10,000-entity limit includes wrappers, nodes, primitive children and unrelated entities. Per-model import limits and renderer memory limits also apply. A single runtime step advances 1–600 ticks and accepts at most 64 complete animation commands.

## Verification and remaining work

`tests/runtime_animation_native.cpp` covers analytic joint poses, independent instances, fixed-tick partitioning, authored baselines, immutable palettes, ownership checks and physics rollback after an invalid intermediate pose. `tests/runtime_animation_contract.py` exercises the authoring/runtime JSON protocol. `tests/runtime_animation_capture.py` compares live capture with the existing asset reference path. Executed platforms, results and hashes belong in [the runtime checkpoint evidence](evidence/m2-runtime-animation.json).

The 0.0.33 crossfade checks add `tests/runtime_animation_blend_contract.py` (seven protocol groups), `tests/runtime_animation_blend_capture.py` (live versus independently authored reference poses), and `tests/desktop_animation_controls.py` (typed authored and guarded live controls). All 32 runtime and 27 authoring-only Linux CTest suites passed. Windows native/protocol checks passed; two GPUs produced 24 exact-pixel reference comparisons from 48 captures and passed 101 scripted editor actions each. The [crossfade evidence](evidence/m2-animation-blending.json) records hashes and limitations. Windows was locked during final GUI qualification, so native viewport captures are available but visual review of the new editor controls remains pending. These fixtures do not measure production character throughput.

The [0.0.34 C# integration evidence](evidence/m2-managed-animation.json) additionally checks typed query/command transfer, queued-write timing, command conflicts and limits, native transition continuity through compatible code reload, and rollback shared with physics, sound and managed fields.

Masked/additive layers, blend spaces/state graphs, inertial transitions, events, IK, retargeting, root-motion extraction, ragdolls, animation compression/streaming, motion vectors and previous-frame skinning remain unfinished. Direct FBX import is not implemented. Mixamo rigs/animations have not been qualified; a supported glTF conversion still needs actual import and visual validation, and separately rigged clips cannot be assumed compatible without retargeting. Native Linux graphics and console graphics remain unqualified.
