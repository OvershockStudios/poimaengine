# Editable rigs and native clip playback

Poima connects imported glTF rigs to ordinary authored entities, fixed-tick native playback and Vulkan compute skinning. Timed crossfades, opt-in inertial transitions and authored masked layers share the native playback authority. Agents can inspect bones, edit their baseline transforms, set complete playback state, advance the simulation and capture the result through the same world protocol. This animation foundation supports ordered override/additive layers and finite-time motion corrections; state graphs and a finished character-animation system remain unfinished.

`world.describe` exposes the components and commands below; revision 53 adds masked animation layers while retaining the common playback fields. `animation_rig_authoring` is available in headless builds. `runtime_clip_playback` requires the simulation build; `gpu_skinning` requires the renderer. The broader `animation` feature flag remains false because the full planned system is unfinished.

## Authored entities

`asset.instantiate` accepts models containing skins or clips. It creates an ordinary wrapper and node hierarchy with stable derived IDs. Every source model node becomes an entity, including nodes outside the selected scene, so no animation target or rig binding disappears silently. Only primitives belonging to the selected scene become renderable children. Static-only asset instantiation retains its earlier behavior.

| Component | Complete value | Meaning |
| --- | --- | --- |
| `AnimationRig` | `asset`, `clip`, `time`, `speed`, `loop`, `playing`; optional `layers` | Wrapper model reference and initial runtime playback state. |
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

`clip` is a zero-based index in that wrapper's model, or null for the authored rest pose. Time is finite and within 0–1e9 seconds. Speed is finite and within 0–8; negative/reverse playback is not supported. `loop` and `playing` are Booleans. Commands for nonexistent rigs or duplicate targets reject the batch. Optional `blend_ticks` selects a transition duration from 0 to 3600 fixed ticks (up to 60 seconds). Omission means zero, retaining immediate replacement; authored `AnimationRig` fields do not include this runtime-only setting. Optional `transition_mode` is `crossfade` (default) or `inertial`. Both modes use the same duration limit; zero duration immediately replaces the pose.

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

The step's existing expected-tick check and retained in-session retry receipts also cover animation commands. Replaying a retained successful request does not advance the clock again. Omitted and explicit zero `blend_ticks` normalize to the same receipt parameters. Omitted and explicit `crossfade` modes also normalize together; changing the mode on a retained retry rejects it.

## Timed crossfades

To transition to clip 1 over half a second, include a duration of 30 ticks:

```json
{"entity":"<wrapper ID>","clip":1,"time":0,"speed":1,"loop":true,"playing":true,"blend_ticks":30}
```

The transition starts at the current committed tick before the requested step. At elapsed tick `k`, destination weight is `k / blend_ticks`, clamped to one. A 30-tick fade is halfway after 15 ticks and complete after 30. Progress uses simulation ticks independently of clip speed or `playing`; pausing a clip holds its pose but does not pause its fade. Pausing the simulation holds both. Zero duration cancels any previous fade and replaces the pose immediately.

For an ordinary fade, source and destination clocks both advance according to their own playback settings. Each samples against the frozen authored baseline. Translation and scale mix linearly in local space; rotation follows the normalized shortest quaternion arc. This does not blend matrices, preserve foot contacts or automatically synchronize locomotion phases.

Interrupting an active fade takes its currently evaluated local pose as an immutable source, then transitions toward the new destination. The interrupted source is frozen rather than recursively retaining earlier transitions. The starting pose is continuous; velocity is not guaranteed continuous. Each clock retains at most one frozen source pose, bounded by the mapped-node and layer-node admission limits. Endpoint evaluation uses the exact destination pose and stops sampling the expired source.

While a fade is active, `animation.transition` reports:

| Field | Meaning |
| --- | --- |
| `start_tick`, `duration_ticks`, `elapsed_ticks` | Transition anchor, requested duration and committed progress. |
| `weight` | Destination contribution from zero to one. |
| `source_frozen` | True for an interrupted transition's evaluated pose. |
| `source_clip`, `source_time`, `source_speed`, `source_loop`, `source_playing` | Outgoing clip state for an ordinary fade; all null for a frozen source. A rest source has null clip, zero time and false playing. |

The enclosing animation fields always describe the destination, not the blended pose. Inspect mapped node transforms or capture the runtime to observe that pose. `transition` is null when absent or complete. This is current-state inspection, not a retained timeline or scrubbable simulation history.

## Masked animation layers

`AnimationRig.layers` is optional and defaults to no layers. A rig can configure four unique slots numbered 1–4; the base is implicit and is not slot zero in JSON. Layers compose in ascending slot order, independently of their authored array order.

Each layer requires `slot`, `mode`, `clip`, `time`, `speed`, `loop`, `playing`, `weight` and `mask`. Playback fields follow the base rules. `mode` is `override` or `additive`; `weight` is a finite number in 0–1. A mask contains unique `{node,weight}` entries, using zero-based model-node indices and finite weights in 0–1. Absent nodes have zero contribution. An empty mask is valid. The effective local contribution is the layer's current weight multiplied by that node's mask weight.

The following complete component value assumes an imported model with clips 0–2 and nodes 1–2. Replace the syntactically valid illustrative hash with its actual imported hash:

```json
{
  "asset":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  "clip":0,"time":0,"speed":1,"loop":true,"playing":true,
  "layers":[
    {
      "slot":1,"mode":"override","clip":1,"time":0,"speed":1,
      "loop":false,"playing":false,"weight":0,
      "mask":[{"node":1,"weight":1},{"node":2,"weight":0.5}]
    },
    {
      "slot":3,"mode":"additive","clip":2,"time":0,"speed":1,
      "loop":true,"playing":true,"weight":0.25,
      "mask":[{"node":1,"weight":1}],
      "reference_clip":0,"reference_time":0
    }
  ]
}
```

Set it with a guarded `world.transact` `component.set` operation for `AnimationRig`, supplying the current `base_revision` and a fresh `request_id`. The model must retain complete valid rig-node and skin bindings. Model-specific clip/node bounds are checked against the actual asset. Layer creation, masks, modes and references are authored configuration; editing them during playback does not mutate the existing runtime.

Override blends the underlying local TRS toward the layer's independently evaluated full pose. Its unkeyed channels use the authored baseline too. Effective weight zero leaves local TRS untouched; weight one copies the target exactly. Translation and scale interpolate linearly and quaternion rotation follows the shortest normalized arc.

Additive layers compare their full local pose with a frozen reference. The reference defaults to the authored baseline. Optional `reference_clip` and `reference_time` choose a clip pose sampled once at runtime construction; it does not advance with either clock. Null reference clip requires reference time zero. Override layers cannot use a separate reference.

For node contribution `alpha`, additive translation adds `alpha * (layer.position - reference.position)`. Positive scale multiplies by `(layer.scale / reference.scale)^alpha`. Rotation uses the node-local delta `inverse(reference.rotation) * layer.rotation`, then postmultiplies the underlying rotation by its shortest-arc quaternion power. Thus the rotation order is `underlying * delta^alpha`; changing the order changes the result for noncommuting rotations.

Composition works in local TRS. The hierarchy is recomposed after the ordered layers. An excluded child's local pose remains unchanged, but its world pose can move when a masked ancestor changes. Masks do not isolate entire world-space subtrees or provide IK/contact preservation.

Each layer owns an independently advancing clock, its own crossfade/inertial transition and its own unmasked output history. Base history records base output before composition. Layer history records that layer's evaluated output before global or node weights. Zero-weight layers still sample and validate their content and retain history, so enabling a layer cannot conceal an invalid pose or borrow the base's motion history.

## Layer commands and weight ramps

Target a configured slot through `runtime.step.animations`. `layer` must be an integer 1–4 and `weight` is required. The remaining six playback fields still specify a complete destination state. This example starts a 15-tick inertial transition on slot 1 and raises its weight over 20 ticks:

```json
{
  "session_id":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
  "request_id":"11111111111111111111111111111111",
  "expected_tick":0,"ticks":10,
  "animations":[
    {
      "entity":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","layer":1,"weight":1,
      "clip":1,"time":0,"speed":1,"loop":false,"playing":false,
      "blend_ticks":15,"transition_mode":"inertial","weight_blend_ticks":20
    }
  ]
}
```

`weight_blend_ticks` is an integer 0–3600, default zero. Zero immediately changes the layer weight. Positive duration linearly ramps from the current effective weight to the target, independently of clip playback and clip-transition duration. Interrupting a ramp captures its current value as the next source: weight remains continuous, but weight velocity need not. A paused clip does not pause its ramp; a paused simulation does.

Base commands omit `layer` and must omit JSON weight fields. Native base commands retain their default weight 1 and weight duration zero. An unknown configured slot or duplicate `(entity,slot)` target rejects the batch. Base and distinct slots on the same rig can appear together. The existing limit is 64 combined animation commands per tick, including compiled gameplay's base and layer commands; there is no separate 64-command allowance for layers.

The step's expected-tick guard, rollback and retained retry receipt apply to the complete animation array. Omitted and explicit default `blend_ticks`, `transition_mode` and layer `weight_blend_ticks` normalize consistently. Integer fields are validated before receipt lookup. Reusing a retained request ID with changed parameters rejects; an exact retained retry does not advance clocks again.

## Layer observation

Wrappers with configured layers expose `animation.layers` through `runtime.entity`, and through `runtime.observe` when animation is selected. Unlayered wrappers omit the field. Summaries appear in ascending slot order:

```json
{
  "slot":1,"mode":"override","weight":0.5,"target_weight":1,
  "weight_transition":{
    "start_tick":0,"duration_ticks":20,"elapsed_ticks":10,
    "source":0,"target":1
  },
  "mask_nodes":2,
  "playback":{
    "clip":1,"time":0,"speed":1,"loop":false,"playing":false,
    "duration":2,"transition":null
  }
}
```

This is an illustrative current-state summary, not the exact transition result of the preceding command. `weight` is effective now and `target_weight` is its destination; `weight_transition` is null when absent or complete. `mask_nodes` counts authored mask entries, including zero-weight entries. `playback` has the existing base playback/transition shape for that independent layer. Summaries do not include full masks, reference poses or motion-history buffers. Authored inspection retains the full frozen configuration, and the existing observation response-size cap still applies.

`RuntimeAnimations::state(entity,tick)` and `Runtime::animation(entity)` return the base projection without populating layer summaries. Native callers can request `state(entity,tick,true)`; `Runtime::entity` does so for live entity observation. The original C# getter and its negotiated inertial extension remain base-only. `RuntimeAnimations::layer_state(entity,slot,tick)` and `Runtime::animation_layer(entity,slot)` return one configured slot without constructing all layer summaries.

## Ownership, validation and rollback

Rig nodes and their descendants own animation-driven transforms. That subtree cannot contain physics bodies, character controllers or a camera controlled by a character controller. Ordinary animated cameras are allowed. A physics-controlled outer wrapper may carry an animated rig; this does not implement extracted root motion or animation-driven colliders.

Skinned mesh acoustics are rejected because the current acoustic geometry path does not follow vertex deformation. Use an explicit separate rigid collision/acoustic proxy. The authoring schema rejects combining `SkinnedMesh` and `AcousticMaterial`.

Base and layer clip clocks, transition state, immutable interruption poses, weight ramps and local transforms join the existing batch checkpoint for physics, gameplay state and sound. A failed sample—including a cubic curve with an invalid intermediate scale—or an edited runtime hierarchy whose matrix exceeds finite ±1e12 bounds rolls back the whole requested step. Matrices are validated during runtime hierarchy evaluation, including reparented trees. Retrying a valid command after failure starts from the prior committed tick and state.

Snapshots own their palette data. Mesh-local palettes derive from current entity transforms as `inverse(mesh_world) * joint_world * inverse_bind`; later stepping cannot mutate an earlier snapshot. The renderer uses these palettes through the existing [compute skinning pass](GPU_SKINNING.md). A GPU-only deformation failure remains a rendering error; capturing a bad blend does not retroactively roll back previously committed simulation ticks.

## Inertial transitions

To preserve recent output motion when changing actions, opt into the native inertial mode:

```json
{"entity":"<wrapper ID>","clip":1,"time":0,"speed":1,"loop":true,"playing":true,"blend_ticks":30,"transition_mode":"inertial"}
```

The command captures the outgoing evaluated pose and estimates its motion from the last two successfully sampled **distinct ticks**. Multiple samples at one tick replace the current pose without advancing the preceding tick. With no pair, outgoing velocity is zero. Native callers that sample sparsely use the actual tick interval; older samples are not extrapolated to the command tick. Ordinary runtime stepping samples every fixed tick.

Incoming translation, quaternion and scale derivatives are evaluated analytically at the destination clip time, then scaled by playback speed. Paused, rest and clamped endpoints have zero incoming velocity. STEP curves also have zero derivative; the first/interior key uses the segment to its right. No neighbouring time is sampled to estimate incoming motion.

The destination continues playing while one bounded correction decays over the duration. Translation uses an additive offset, positive scale uses logarithmic offsets, and rotation uses a shortest-arc quaternion offset with the target's angular motion expressed in the same parent frame. A finite-time quintic correction matches the initial displacement and estimated velocity difference; displacement and its first two derivatives reach zero at completion. The boundary returns the exact outgoing pose, and completion returns the exact destination. Interruptions replace the correction using the current output and its history, without retaining a recursive transition tree.

This preserves **estimated** output velocity at the boundary. It does not guarantee smooth STEP keys, looping seams, clip endpoint changes, foot contacts, matched locomotion phases or zero overshoot. Invalid finite values, scales or hierarchy matrices reject and roll back the batch. The initial correction acceleration is zero; this is not an overshoot-limited spring.

Active inspection adds `mode:"inertial"` to `animation.transition`. `weight` reports elapsed duration divided by total duration, rather than a two-clip pose contribution. `source_frozen` is true and source-clock fields are null: the captured pose supplies correction initialization, not a playing source clip. Legacy crossfade inspection retains its earlier shape.

Native checkpoints own immutable output history and correction buffers. For worlds without layers, after a rig has used inertial mode, nested animation saves use version 2 and retain both, including history after the transition completes. Crossfade-only saves retain version-1 bytes. Loading a version-1 save remains supported and starts without velocity history; subsequent distinct output samples establish it. The outer snapshot and durable slot format are unchanged. These broader save contracts remain in development.

At checkpoint 0.0.55 this mode was available through native C++ and the world protocol; C# and desktop controls selected crossfades. Version 0.0.56 adds opt-in C# access through `IInertialAnimationGame`, the enum-third-argument `SetAnimation` overload and `GetAnimationExtended`. It negotiates `animation_inertial_v1` in a separate 192-byte services-7 extension. The original numeric-time C# overload and its ABI structures remain unchanged and select crossfades. Desktop crossfade controls remain unchanged. See [the managed example and requirements](MANAGED_GAMEPLAY.md#control-animation-from-c).

## Compiled layer controls

Version 0.0.58 exposes configured layers to C# through `IMaskedAnimationGame`, `GetAnimationLayer` and `SetAnimationLayer`; see [the managed example](MANAGED_GAMEPLAY.md#control-masked-layers-from-c). The marker inherits inertial access and negotiates both animation features in a separate 208-byte services-7 prefix. Legacy getters and both `SetAnimation` overloads continue to select the base clock. Existing compiled 176/192-byte views remain unchanged.

Layer calls route into the same override/additive composition, unweighted per-clock history, linear fixed-tick weight ramps and atomic batch rollback described above. A missing valid slot on an existing entity returns null; a dead entity or invalid slot fails. Commands replace playback and target weight, without editing masks, references or layer mode. Compiled writes share the caller/native target conflicts and combined 64-command budget. The managed getter validates identity, slot, canonical missing/inactive data, finite playback/weights and clip/weight transition invariants. It returns summaries, not full pose history.

The addition changes neither animation math nor nested save versions. Compatible CoreCLR reload preserves native layer state; native libraries remain process-pinned, and saves remain bound to their actual gameplay artifact/content. Supported matched SDK/bridge cohorts negotiate before game construction; arbitrary mixed old bridge/new SDK cohorts and module/loader side effects are outside that guarantee.

## Native interfaces and bounds

`RuntimeEntityDefinition` adds optional `animation_rig`, `rig_node` and `skinned_mesh` bindings. `AnimationCommand` carries optional-duration `blend_ticks` with a zero default and `AnimationTransitionMode` (`Crossfade` or `Inertial`), plus optional layer routing and weight-ramp fields. Active `RuntimeAnimationTransition.mode` identifies the native mode. `Runtime::step` takes a trailing `std::vector<AnimationCommand>`, and `Runtime::animation(id)` returns optional `RuntimeAnimationState`. `RuntimeEntityState` exposes the local transform and optional playback state. C# retains `GameContext.GetAnimation` and the numeric-time, crossfade-only `SetAnimation` in the 176-byte services-7 baseline. Its marked-game extension adds typed mode selection and `AnimationStateExtended`, whose `State` is the legacy projection and whose active `Mode`/`Progress` are nullable. See [managed gameplay](MANAGED_GAMEPLAY.md#control-animation-from-c) for negotiation, queued-write timing, conflicts and reload behavior.

`validate_runtime_animation(definition)` is built without Jolt and shared by authoring/runtime validation. `CompiledAnimation` validates and copies source curves once, retains the parent traversal, and supports a supplied authored TRS baseline. Runtime rigs sharing a model share the compiled sampler. CPU sampling does not require a graphics device or scan every source key for validation on every tick. This has not yet established production animation throughput or crowd budgets.

Current per-world limits are 128 rigs, 10,000 mapped nodes, 65,536 channels counted across rig instances, two million keys across unique compiled models, and 32,768 palette-joint entries across skinned primitive instances. The existing 10,000-entity limit includes wrappers, nodes, primitive children and unrelated entities. Per-model import limits and renderer memory limits also apply. A single runtime step advances 1–600 ticks and accepts at most 64 complete animation commands.

`CompiledAnimation::sample_motion` returns a `ModelMotion` containing the ordinary validated `ModelPose` and node-ordered `NodeMotion` derivatives. Translation is local units per clip second; angular velocity is parent-frame radians per clip second; logarithmic scale velocity is inverse clip seconds. Playback speed is applied by the runtime, and this const sampler never retains output history. `runtime_inertial_transitions` and `runtime_animation_layers` in CLI capabilities are true only for simulation builds.

## Layer persistence and bounds

Configured layers use nested `poima.animation-state` v3. Every rig records its base clock and its complete ordered layer array, with histories, clip transitions and weight ramps. Each layer also binds the exact canonical mask and reference clip/time. Restoring against a changed slot, mode, mask or reference rejects atomically. Layered definitions reject v1/v2 instead of inventing missing clocks; unlayered definitions reject v3. Unlayered v1/v2 serialization and the outer snapshot/save formats remain unchanged. This does not provide animation-definition migration.

The parser rejects duplicate fields, unexpected shapes, malformed integers and inconsistent effective weights; nesting is limited to 32 and parse events to eight million. Nested legacy state retains its 16 MiB limit; layered v3 has a 64 MiB limit. Outer snapshots and durable saves also remain capped at 64 MiB, so maximum admitted configurations are not guaranteed to fit a save.

A rig admits up to four configured slots, and aggregate instanced layer work is limited to 20,000 full model nodes across the world. Existing limits of 128 rigs and 10,000 mapped nodes still apply. These are admission bounds, not measured throughput. Existing C# services-7 views remain 176/192 bytes and base-only; opt-in masked games receive the separately named 208-byte view. The native `RuntimeAnimations::state(entity,tick,true)` includes layer summaries; its default and `Runtime::animation(entity)` retain the base projection.

## Verification and remaining work

`tests/runtime_animation_native.cpp` covers analytic joint poses, independent instances, fixed-tick partitioning, authored baselines, immutable palettes, ownership checks and physics rollback after an invalid intermediate pose. `tests/runtime_animation_contract.py` exercises the authoring/runtime JSON protocol. `tests/runtime_animation_capture.py` compares live capture with the existing asset reference path. Executed platforms, results and hashes belong in [the runtime checkpoint evidence](evidence/m2-runtime-animation.json).

The 0.0.33 crossfade checks add `tests/runtime_animation_blend_contract.py` (seven protocol groups), `tests/runtime_animation_blend_capture.py` (live versus independently authored reference poses), and `tests/desktop_animation_controls.py` (typed authored and guarded live controls). All 32 runtime and 27 authoring-only Linux CTest suites passed. Windows native/protocol checks passed; two GPUs produced 24 exact-pixel reference comparisons from 48 captures and passed 101 scripted editor actions each. The [crossfade evidence](evidence/m2-animation-blending.json) records hashes and limitations. Windows was locked during that GUI qualification. A subsequent 0.0.34 [unlocked visual pass](evidence/m2-managed-animation-visual.json) passed 101 actions and produced an inspected [full-window capture](evidence/m2-managed-animation-visual.png) of the AnimationRig controls on NVIDIA at 125% scale. These fixtures do not measure production character throughput.

The [0.0.34 C# integration evidence](evidence/m2-managed-animation.json) additionally checks typed query/command transfer, queued-write timing, command conflicts and limits, native transition continuity through compatible code reload, and rollback shared with physics, sound and managed fields.

The [0.0.55 inertial checkpoint](evidence/m2-animation-inertial.json) adds independent analytic motion/pose references, noncommuting rotation checks, same-tick and sparse history, partitioning, malformed-state rejection, fresh-process saves and whole-runtime restoration with immediate re-interruption. Three new-mode protocol tests pass on each OS. Both laptop GPUs pass 24 exact inertial and 24 legacy reference pairs across 96 captures at 640×480 with 1×/4× MSAA. GPU checks cover translation/skinning; rotation and scale checks are native mathematics. The existing compiled C# crossfade/reload/rollback suite passes twelve checks on each OS, without extending its animation API. These are bounded correctness checks, not crowd throughput or character-quality benchmarks.

The [0.0.56 managed inertial record](evidence/m2-managed-inertial.json) records the separately negotiated C# extension and distinguishes executed CoreCLR, ABI guard and Native AOT checks. The extension uses the same native correction/history authority; it adds no layered graph, new rendering algorithm or implied GPU/performance qualification. Legacy version-1 saves still start without stored velocity history.

The [0.0.57 layer checkpoint](evidence/m2-animation-layers.json) records independent local-TRS and hierarchy mathematics, masked override/additive composition, weight interruptions, per-clock history isolation, strict validation, command limits and atomic restoration. Eight layer protocol checks pass per runtime OS; two actual validation/discovery checks also run in an authoring-only build. Both GPUs produce 36 exact reference pairs from 72 captures. Actual old/new executable comparisons preserve unlayered v1/v2 save bytes and fresh continuation, including immediate interruption. Existing CoreCLR and Native AOT cohorts still pass with base-only animation APIs. These are bounded correctness checks, without new desktop activation or game-scale performance qualification.

The [0.0.58 compiled-layer record](evidence/m2-managed-animation-layers.json) adds executed Windows/Linux CoreCLR and Native AOT layer controls, independent staged-read checks, target conflicts, invalid-later-sample rollback and fresh complete-payload restore followed by immediate compiled re-interruption. Separate mock ABI fixtures validate the 80-byte command, 200-byte state and guarded 208-byte service prefix. These checks reuse native layer authority and establish no new GUI, GPU, physical-input or throughput qualification.

Reproduce the native and protocol checks with a simulation build:

```sh
cmake --build build/runtime-headless --target poima poima-animation-layers-test poima-runtime-animation-layers-test
./build/runtime-headless/poima-animation-layers-test
./build/runtime-headless/poima-runtime-animation-layers-test
python3 tests/runtime_animation_layers_contract.py build/runtime-headless/poima --evidence build/layer-contract.json
```

For an authoring-only executable, pass `--runtime 0`. The Windows protocol runner additionally needs `--windows-interop` when launched from WSL. Rendered reference checks require a Windows Vulkan build:

```sh
python3 tests/runtime_animation_layers_capture.py build/windows-runtime/poima.exe --windows-interop --gpu 0 --output build/layer-captures
```

Repeat with `--gpu 1` and a separate output directory on a two-GPU system. The old/new compatibility runner requires a separately preserved 0.0.56 executable and its dependencies; it rejects identical images:

```sh
python3 tests/runtime_animation_legacy_compatibility.py --binary build/runtime-headless/poima --legacy-binary /path/to/preserved-0.0.56/poima --output build/layer-legacy
```

Desktop layer widgets, blend spaces/state graphs, events, IK, retargeting, root-motion extraction, ragdolls and animation compression/streaming remain unfinished. Single-sample backward UV motion and previous-frame skinning are documented separately in [Scene products](SCENE_PRODUCTS.md). [Direct FBX import](FBX_IMPORT.md) supports its documented bounded profile. Mixamo rigs/animations have not been qualified; separately rigged clips cannot be assumed compatible without retargeting. Native Linux graphics and console graphics remain unqualified.
