# Native C# gameplay artifacts

Poima's Native AOT route compiles one C# `Game<TState>` into a native shared library. It uses the same engine-owned state and service ABI as [development-time CoreCLR gameplay](MANAGED_GAMEPLAY.md). The development bridge remains the reloadable path; a native game library stays loaded for the lifetime of its player process.

The 0.0.35 integration has recorded Linux and Windows native execution, CoreCLR/native state comparisons and a relocated Windows Vulkan game replay. These are bounded fixture results; clean-machine deployment, consoles and production workloads remain unqualified. See the [checkpoint evidence](evidence/m2-native-gameplay.json) and [implementation status](IMPLEMENTATION_STATUS.md).

The 0.0.52 Windows collection checkpoint publishes the existing inventory fixture as a native shared library and passes its unchanged 151-RPC collection/save contract. A real exported player runs seven scripted ticks after relocation, with the owned source project removed and no managed PE, hostfxr or CoreCLR payload in the bundle. The same bundled runtime/artifact separately passes the headless contract in fresh fixture worlds; this is not in-player save UX. Qualification covers capacity-four entity/int32 component buffers and the recorded hardware capture, not general collections, clean-machine deployment, consoles or production performance. [Collection artifact and bundle evidence](evidence/m2-component-collections-native-windows.json).

The development compatibility baseline is services epoch 7 with a 176-byte prefix. Games declaring `IInertialAnimationGame` additionally require the named 192-byte animation extension. `IMaskedAnimationGame` inherits that marker and requests the named 208-byte masked-layer extension. `ICharacterInputGame` requests the independent 216-byte movement extension; `INavigationGame` requests the independent 224-byte navigation extension; `IHierarchicalInstancesGame` requests the independent 232-byte instance resolver; `IPlayerPreferencesGame` requests the independent 256-byte [player-preferences extension](COMPILED_PLAYER_SETTINGS.md). `IInputActionsGame` requests the independent 264-byte [named-input extension](INPUT_ACTIONS.md). `IHitRegionsGame` requests the independent 272-byte [hit-region extension](HIT_REGIONS.md). `ITwoBoneIkGame` requests the independent 288-byte [two-bone IK extension](TWO_BONE_IK.md). `IRootMotionGame` requests the independent 304-byte [root-motion extension](ROOT_MOTION.md). `ICameraLensGame` requests the independent 320-byte [camera-lens extension](CAMERA_LENSES.md). `ICharacterStanceGame` requests the independent 336-byte [stance extension](CHARACTER_STANCE.md). Earlier service epochs require rebuilding; the supported legacy artifact format is described below. Service compatibility does not migrate saved gameplay state.

The preference tail preserves all earlier layouts and uses three copied, bounded callbacks at offsets 232/240/248. Reads use the native owner's committed configuration and cached observations; staged mutations publish only after the complete callback/native boundary succeeds. It adds no settings storage or gameplay-save format. See [compiled settings](COMPILED_PLAYER_SETTINGS.md) for wire sizes, lifecycle and task instructions.

## Publish

Publishing needs a matching x64 operating system, a .NET 10 SDK and its supported native linker toolchain. On Windows this requires Visual Studio 2022 or later C++ build tools and a compatible Windows SDK; the recorded build used MSVC 14.44 and Windows SDK 10.0.26100.0. The engine can still be cross-built separately with LLVM-MinGW. See Microsoft's [Native AOT prerequisites](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/) and [native shared-library limitations](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/libraries). The Python script is build tooling; the engine does not invoke it when playing a game.

```sh
python3 scripts/publish_native_gameplay.py \
  --project examples/managed/DoorGame/Poima.DoorGame.csproj \
  --type Poima.Examples.DoorGame \
  --dotnet /path/to/dotnet \
  --output build/door-native
```

The output directory must be new. `--rid linux-x64` or `--rid win-x64` selects a matching host target; cross-OS Native AOT publication is not supported. `--work` retains build intermediates in a separate new directory. Project builds execute trusted build tasks and dependencies.

The publisher disables persistent compiler/build servers and MSBuild node reuse.
This allows a [native development worker](DEVELOPMENT_JOBS.md#per-job-environments)
to own its ordinary compiler descendants and enforce a deadline. Running the
publisher directly still requires the caller to supervise it. Builds receive
the publisher process's environment; the worker can supply a complete host-selected
replacement. Microsoft's [`dotnet publish` options](https://learn.microsoft.com/en-us/dotnet/core/tools/dotnet-publish#options)
document `--disable-build-servers`.

Games may reference the matching `Poima.Gameplay` SDK through a project reference
or a distributed DLL. The game build must copy `Poima.Gameplay.dll` into its
output; a binary reference with `Private=false` is rejected before generation.
Generated native bindings explicitly reference that copied SDK. This supports
both reference styles within a matched SDK/tooling cohort; it does not qualify
arbitrary mixtures of SDK versions.

To check both routes on a configured matching host, run the regression with a
built `poima-gameplay-compatibility-test` executable:

```sh
python3 tests/native_gameplay_sdk_reference.py \
  --dotnet /path/to/dotnet \
  --compatibility-test /path/to/poima-gameplay-compatibility-test \
  --output build/native-sdk-reference-check
```

The test publishes two actual Native AOT libraries, executes their compiled
Tick/Control callbacks, and verifies missing-copy rejection. Build products,
checksums and failed-command diagnostics stay in the new output directory.

Build-time inspection generates a direct typed factory and state-layout assertions. The native entry calls the selected game and state types directly. It does not discover or load managed game assemblies at runtime. The generator currently requires public, non-generic top-level game and state types, a public parameterless game constructor, a stateless game class and supported unmanaged state fields. Nested types are not supported. Arbitrary reflection, dynamic code, managed plugin loading and dependency publication are not supplied by this route. Unexpected runtime publish payloads reject until an explicit packaging policy exists.

Native AOT includes runtime services, including garbage collection. It is native compiled gameplay, not a guarantee of zero allocation or absence of a runtime. The module reports its actual dynamic-code support and compilation flags; the loader requires both to be false.

## Artifact contents

A dedicated artifact directory contains `native-gameplay.json`, one library, and the included dependency notices. Intermediate IL assemblies, debug symbols, generated source and build logs belong outside that directory.

The publisher emits descriptor version 2. `engine_version` records the producing build for diagnostics; compatibility instead requires call ABI 1/80 bytes and services epoch 7. Unmarked games declare `minimum_services_bytes:176` and `baseline_v7`. Games declaring [the inertial animation marker](MANAGED_GAMEPLAY.md#control-animation-from-c) declare `minimum_services_bytes:192` and additionally require `animation_inertial_v1`. Games declaring [the masked-layer marker](MANAGED_GAMEPLAY.md#control-masked-layers-from-c) declare `minimum_services_bytes:208` and both `animation_inertial_v1` and `animation_layers_v1`. Games declaring [the character-input marker](CHARACTER_INPUT.md) require `minimum_services_bytes:216` and `character_input_v1`, independently of either animation marker. Combined character/animation games retain the 216-byte prefix and declare each feature. Games declaring [the navigation marker](MANAGED_GAMEPLAY.md#plan-npc-routes-from-c) require `minimum_services_bytes:224` and `navigation_query_v1`; navigation is independent of movement/animation, and combined games declare each needed feature. Games declaring [the hierarchical-instance marker](MANAGED_GAMEPLAY.md#create-and-remove-runtime-instances) require `minimum_services_bytes:232` and `hierarchical_instances_v1`; this feature independently authorizes node resolution, and combined games require the largest named prefix of their declared features and retain every callback requirement. Games declaring `IPlayerPreferencesGame` require `minimum_services_bytes:256` and `player_preferences_v1`; they independently authorize preference reads/staging/result queries. Games declaring `IInputActionsGame` require `minimum_services_bytes:264` and `input_actions_v1`; games declaring `IHitRegionsGame` require `minimum_services_bytes:272` and `hit_regions_v1`. Games declaring `ITwoBoneIkGame` require `minimum_services_bytes:288` and `animation_ik_v1`. Games declaring `IRootMotionGame` require `minimum_services_bytes:304` and `animation_root_motion_v1`. Games declaring `ICameraLensGame` require `minimum_services_bytes:320` and `camera_lens_v1`; this independently authorizes Tick-only lens reads and staged controls. These independent markers authorize named-input reads, committed-pose hit tests, Tick-only two-bone IK controls and Tick-only root-motion controls respectively. Games declaring `ICharacterStanceGame` require `minimum_services_bytes:336` and `character_stance_v1`; this independently authorizes Tick-only stance reads and staged posture intent. Combined games retain every separately requested feature. These are the supported required prefix profiles; layer-without-inertial, unnamed extended profiles and arbitrary intermediate or larger required prefixes reject. The descriptor also declares the Linux/Windows x86_64 target, fixed `poima_gameplay_entry` export, game identity/type and complete state schema. Its payload inventory records relative paths, byte counts, SHA-256 hashes and roles (`library`, `dependency`, `notice`, `metadata`).

The generator writes service requirements to `requirements.json` separately from `schema.json`; the publisher validates them and merges the required feature names into the descriptor. The generated native binding also validates the offered host contract before constructing the game. A descriptor omitting its compiled game's named requirement therefore cannot make the game run against the baseline view. Service requirements are not part of the state schema or its saved-state fingerprint. Unmarked games omit the optional exported requirements sibling for legacy-host compatibility.

Games opting into [persistent field metadata](GAMEPLAY_PERSISTENCE.md) additionally require `gameplay_persistence_v1`. The publisher adds that feature only when the schema contains `persistent`; a descriptor cannot omit the requirement while carrying the metadata. Component schemas containing [bounded collections](COMPONENT_COLLECTIONS.md) require `component_collections_v1`. These two feature declarations add no service callbacks and do not enable save migration. Discovery and the selected runtime descriptor remain authoritative for available features, including the named 192-byte inertial, 208-byte masked-layer, 216-byte character-input, 224-byte navigation, 232-byte hierarchical-instance, 256-byte player-preference, 264-byte named-input, 272-byte hit-region, 288-byte two-bone IK, 304-byte root-motion, 320-byte camera-lens and 336-byte character-stance extensions. Older baseline-only artifacts retain their original requirement.

Unknown required features, incompatible call/service epochs and unsupported required prefix profiles reject before loading the native library. A larger advertised available runtime table can satisfy a supported smaller requirement. Gameplay receives its negotiated 176-, 192-, 208-, 216-, 224-, 232-, 256-, 264-, 272-, 288-, 304-, 320- or 336-byte view. Supported matched SDK/bridge cohorts validate marked-game host negotiation before the constructor, `Initialize` or `Tick`; arbitrary DLL/module initialization and loader side effects remain outside that guarantee. Arbitrary mixed old bridge/new SDK cohorts are unsupported and do not establish the layer pre-construction guard. Metadata does not enable unimplemented extensions.

The layer service is `PoimaGameAnimationLayerServicesV1`: its getter/setter occupy offsets 192/200, preserving the original 176-byte baseline and 192-byte inertial structs. Its version-1 command is 80 bytes; returned state is 200 bytes. Callers initialize output version/bytes/reserved. Native validation checks the known extent before reading tail fields, writes exactly the known state prefix, and returns canonical absent state for a missing valid slot. Layer playback and target weights use the same native staging, combined 64-command limit, rollback and version-3 animation save authority as the world protocol; the extension adds no save version or compatible native-library replacement.

The navigation service is `PoimaGameNavigationServicesV1`, with `navigation_path` at offset 216 and allocation size 224. Its version-1 request/result/point sizes are 64/128/12 bytes. It reads a live native character foot during Tick, checks baked clearance, and permits eight native attempts per Tick including failures. Native and SDK output staging preserve caller storage on rejection. Navigation queries do not move the character; `character_input_v1` is a separate requirement. The extension adds no save format. See [navigation binding, query bounds and saved-source closure](NAVIGATION.md).

The instance service is `PoimaGameInstanceServicesV1`, preserving the 224-byte
navigation prefix and appending `instance_node` at offset 224 for a 232-byte
allocation. Its callback accepts root/local `PoimaEntityId` pointers and an
output live `PoimaEntityId`; the SDK names that callback `InstanceNode` and exposes
`ResolveNode(EntityId, TemplateNodeId)`. Local IDs are canonical nonzero recipe
identities, not live entities or a supported persisted state-field kind. The
named `hierarchical_instances_v1` requirement is independent of every intervening
extension; a larger allocation does not authorize navigation or animation APIs.

During Tick, resolution accepts a committed instance or a noncanceled reserved
birth. During Control, only committed instances resolve. It does not publish
reserved members: entity, liveness, component and animation reads still see
committed state. Valid member initializers, animation commands and separately
declared character input can be staged for the birth Tick. Native rejection
clears the resolver output, and the SDK rejects errors or a zero-success result.
Follow [runtime instances](RUNTIME_INSTANCES.md) for complete imported graph
constraints, remapped references, cancellation, whole-instance deletion and
exact version-6 restoration. The external host can also select an
[explicit save upgrade](SAVE_UPGRADES.md) preserving the native graph and
authorizing custom field/default/capacity changes. The extension does not add
native-library reload or a visual prefab authoring tool.


The two-bone IK service is `PoimaGameTwoBoneIkServicesV1`: its `regions` member
preserves the full 272-byte hit-region prefix, followed by `ik_get` at offset
272 and `ik_set` at offset 280. Total size is 288 bytes, services epoch remains
7, and the outer call remains 1/80. `animation_ik_v1` is independent of every
intermediate feature. A larger allocation does not authorize unrequested
callbacks; matched SDK/bridge negotiation checks the marker before game
construction and validates the extent before reading either callback.

`PoimaGameTwoBoneIkCommandV1` is 96 bytes: version/bytes at 0/4, entity at 8,
slot/enabled/space/reserved at 24/28/32/36, weight at 40, target at 48 and pole
at 72. `PoimaGameTwoBoneIkStateV1` is 160 bytes: version/bytes at 0/4,
present/slot at 8/12, entity at 16, root/middle/end/enabled/space/reserved at
32/36/40/44/48/52, weight at 56, target/pole/end-position at 64/88/112,
target error at 136, clamped/pole-fallback at 144/148 and reserved tail at 152.
Vectors contain three doubles, weight/error are doubles, identifiers are
16 bytes, the final reserved tail is a uint64, and all other scalar fields are
uint32. Command and state version are 1. Space 0 is World and 1 is Rig.

Callers provide the known minimum extent and zero reserved fields. Native
state success writes exactly 160 bytes; absence is all zero except version
1/bytes 160. Getter rejection preserves caller output. Presence binds the
requested entity/slot and validates bounded model indices, booleans, finite
controls and diagnostics. Both callbacks are Tick-only, with 1,024 admitted
reads and 64 admitted writes; reads return committed cached state, while
setters stage complete controls. Existing animation commands have a separate
64-command budget. Combined caller/game IK writes have their own 64-command
limit and cannot target the same slot. The extension uses the native solver,
rollback and configured-IK animation-v4 save rules; old no-IK save output and
service prefixes remain unchanged. It adds no compatible native-library
replacement. See [the solver contract](TWO_BONE_IK.md) for chain and scale bounds.

The root-motion service is `PoimaGameRootMotionServicesV1`: its `ik` member
preserves the full 288-byte prefix, followed by `root_motion_get` at offset 288
and `root_motion_set` at offset 296. Total size is 304 bytes; services epoch
remains 7 and call ABI remains 1/80. `animation_root_motion_v1` is independent
of IK and every intermediate feature. A larger readable allocation grants no
undeclared operations.

`PoimaGameRootMotionCommandV1` is 32 bytes: version/bytes at 0/4, entity at 8,
enabled at 24 and reserved at 28. `PoimaGameRootMotionStateV1` is 176 bytes:
version/bytes/present/reserved at 0/4/8/12, entity/actor at 16/32,
node/mode/enabled/yaw at 48/52/56/60, from/to tick at 64/72, local delta at 80,
yaw delta at 104, world requested/applied at 112/136, applied yaw at 160 and
translation error at 168. Vectors are three doubles; yaw/error are doubles,
ticks are uint64, identifiers are 16 bytes and the other scalar fields are
uint32. There is no reserved tail. Command/state version is 1; mode 0 is
Extract and 1 is Character.

Callers provide the known minimum extent and zero reserved fields. Getter
success writes exactly 176 bytes; absence is all zero except version 1/bytes
176, and rejection preserves caller output. Presence binds the requested
entity and validates frozen policy, booleans, finite diagnostics and an
interval of zero or one tick. Both callbacks are Tick-only: 1,024 admitted
reads return committed cached state and 64 writes stage enable controls.
Combined caller/game writes have the same 64-command budget and reject duplicate
rig targets. Binding changes, source selection and actor/yaw changes remain
outside the setter. The independent extension uses the native planar physics,
rollback and animation-v5 save rules without changing previous service layouts
or unconfigured animation serialization. See [root motion](ROOT_MOTION.md)
for accepted content, disabled/Extract semantics and input ownership.

The independent character-stance service is `PoimaGameCharacterStanceServicesV1`:
its camera-lens member retains the 320-byte prefix, followed by
`character_stance_get` at offset 320 and `character_stance_set` at offset 328.
Its negotiated size is 336 bytes, services epoch remains 7 and the call stays
1/80. Stance-only games do not receive other marker rights.

The version-1 command is exactly 32 bytes: entity at 8, Boolean crouch at 24 and
zero reserved at 28. The state is 64 bytes: entity at 8, present at 24, flags at
28, ground at 32, radius/standing-height/crouched-height/effective-height/speed/
camera-drop floats at 36/40/44/48/52/56, and zero reserved at 60. Flag bits are
requested crouch, actual crouch, blocked stand and configured profile. Ground is
0 on-ground, 1 steep-ground, 2 unsupported or 3 in-air. Absent state is zero
except version/bytes; rejection preserves output. The SDK validates complete
canonical replies before exposing them. [Stance](CHARACTER_STANCE.md) defines
clearance, bounds, quotas, staging and snapshot-v8 behavior.

Descriptor version 1 remains supported for the known 0.0.39 call-1/services-7 baseline. Current native calls provide the exact 176-byte view expected by those older compiled consumers, retaining callback pointers and context while excluding any host tail. New bridge/native entry code accepts at least that prefix and never reads unknown tail fields. This is a bounded service contract, not compatibility with all historical SDK versions or arbitrary changes to public C# APIs.

Read-only inspection validates the schema, file inventory and native image headers without executing code. It accepts foreign-target metadata for export workflows. Actual loading additionally requires the running engine's platform, generated schema and native diagnostics to agree. Hashes check integrity against the supplied inventory; they are not signatures or an authenticity guarantee.

The artifact inventory is bounded to 256 files, 256 MiB per payload, 1 GiB total and a 1 MiB descriptor. The state schema allows at most 128 supported fields and 64 KiB of state. Portable path rules reject traversal, symlink/reparse content, case collisions and unlisted files. Native image-header checks do not prove that a library can execute or that all system dependencies are available.

## Load through the world service

After `runtime.start`, submit:

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "runtime.gameplay.load_native",
  "params": {
    "session_id": "<runtime session ID>",
    "request_id": "<fresh request ID>",
    "expected_tick": 0,
    "expected_revision": 0,
    "descriptor": "gameplay/native-gameplay.json",
    "values": {}
  }
}
```

The descriptor resolves relative to the world document. `values` is optional and defaults to an empty object. An optional `expected_descriptor_sha256` binds loading to previously inspected descriptor bytes. Game launch passes that guard automatically. Invalid supplied values are rejected before mapping the native module.

The existing runtime receipt pool, exact retry matching, tick/revision guards and gameplay inspection/edit operations apply. `runtime.gameplay.inspect` with `include_schema: true` reports `backend: "native_aot"`, the library hash, state schema, values and native diagnostics. Simulation uses the same callback queue timing, native animation authority and whole-batch rollback as CoreCLR gameplay.

The [guard and retry contract](MANAGED_GAMEPLAY.md) requires integer JSON tick
and revision tokens even for retained retries. Floating-token guards reject
before receipt recovery; a valid typed retry preserves its original result.

Only one canonical native library path and image are selected per process. The loader never unloads it, including after export or generated-schema validation fails. Replacing a loaded native game is rejected. Stopping a world releases its game instance; starting a new world can create another instance from the same pinned artifact. Selecting a different artifact requires restarting the process. This is separate from development-time compatible CoreCLR reload.

## Package a game

Project version 1 remains the format for projects without gameplay. Version 2 requires a `gameplay` object:

```json
"gameplay": {
  "descriptor": "gameplay/native-gameplay.json",
  "values": { "UseDistance": 3.5 }
}
```

Other project fields retain their existing contracts. Inspection validates initial values against the artifact schema. Export requires a matching target runtime with `features.native_gameplay: true`; older runtime descriptors lacking this feature are treated as false.

For version-2 gameplay artifacts, `runtime.json` must explicitly advertise `gameplay_call_version:1`, `gameplay_call_bytes:80`, `gameplay_services_version:7`, its available `gameplay_services_bytes` and `gameplay_features` including `baseline_v7`. A baseline artifact needs at least 176 available bytes; an inertial artifact additionally needs at least 192 and `animation_inertial_v1`; a masked-layer artifact needs at least 208 and both animation features; character input needs at least 216 and `character_input_v1`; navigation needs at least 224 and `navigation_query_v1`; hierarchical resolution needs at least 232 and `hierarchical_instances_v1`; compiled preferences need at least 256 and `player_preferences_v1`; named input needs at least 264 and `input_actions_v1`; hit-region queries need at least 272 and `hit_regions_v1`. The call/size/feature fields form one complete group. Project inspection exposes the artifact's descriptor version and requirements; export and bundle inspection use the same compatibility predicate.

A navigation-bound world additionally requires `features.navigation:true`, its checked `.pnav` in content closure, and `share/poima/licenses/RecastNavigation/License.txt` in the runtime inventory. Export and bundle verification enforce these gates. Runtime module negotiation also requires a frozen binding, so advertising an available callback cannot make an unbound world run a navigation-marked game.

A legacy 0.0.39 runtime descriptor declaring only service version 7 can serve version-1 baseline artifacts. It cannot serve version-2 artifacts without an explicit contract. Legacy runtime descriptors remain usable for bundles without gameplay. The exporter and selected runtime still require an exact engine build version; that package-cohort check is separate from a game's service requirements. Saved-state schema/content/image checks also remain unchanged.

Export copies the descriptor and exact payload closure into the bundle's `gameplay/` directory and emits game manifest version 2. Game launch verifies the bundle, starts its read-only runtime, loads the compiled module at tick zero, applies supplied values, then enters the player. Its result includes full gameplay inspection. Source projects, development IL and hostfxr are not part of this artifact route.

A native-only engine configuration uses `POIMA_ENABLE_SIMULATION=ON`, `POIMA_ENABLE_NATIVE_GAMEPLAY=ON` and `POIMA_ENABLE_MANAGED_GAMEPLAY=OFF`; hostfxr headers are unnecessary. The native option defaults to the simulation option when first configuring a build. A graphical game bundle still requires the renderer and its platform dependencies. Path-based inspection and subsequent OS loading do not constitute a filesystem-race-free execution boundary.

## Compatibility qualification

`poima-gameplay-compatibility-test` checks the requirement predicate and bounded legacy view. With no arguments it checks policy only. To execute compiled callbacks, supply either `HOSTFXR BRIDGE ASSEMBLY` or `--native DESCRIPTOR`; the game fixture must be `Poima.Tests.ManagedUiGame` from `tests/managed_ui_gameplay`. The fixture verifies Tick/Control state, callback counts, rejection before mutation, and unchanged opaque host bytes.

For an old-binary compatibility test, retain the old bridge/SDK/game closure and native descriptor/payload before rebuilding. Run the new harness against those exact bytes, then repeat with the old game and new bridge/SDK. Rebuilding the fixture from old source is not evidence that the original shipped bytes survived an upgrade. Record hashes before and after each run.

The independent managed guard suite is documented in `tests/managed_service_abi/README.md`; package/export coverage lives in `tests/project_gameplay_contract.py`. [Recorded results](evidence/m2-gameplay-compatibility.json) distinguish actual Native AOT execution from production native-entry code exercised under CoreCLR.

The [0.0.56 managed inertial record](evidence/m2-managed-inertial.json) separates CoreCLR, ABI guard and actual Native AOT cohorts. A native library remains pinned until process exit; the extension does not add compatible native reload. No new clean-machine, console, physical-input or GPU qualification follows from service negotiation alone.

The [0.0.58 compiled-layer record](evidence/m2-managed-animation-layers.json) adds actual Windows/Linux Native AOT masked-layer execution, declared 208-byte requirements, descriptor-omission rejection before game callbacks, complete save restoration and immediate compiled re-interruption. Independent mock ABI checks cover SDK/bridge guards and transfer separately from real Runtime callback execution. Preserved 176/192-byte compiled cohorts retain their bounded views. No new GUI, GPU, physical-input, clean-machine or console qualification is implied.


To reproduce that actual native fixture, publish `tests/fixtures/gameplay_layers/Poima.LayerGameplay.csproj` with type `Poima.Verification.LayerGame` through the command above. Then run `tests/runtime_gameplay_layers_native.py` with `--binary`, the resulting `--descriptor`, a separately preserved pre-layer `--legacy-binary` and a new `--output`; add `--windows-interop` when launching Windows from WSL. The verifier consumes a published artifact and does not perform publication itself.

The [0.0.64 runtime-navigation record](evidence/m2-runtime-navigation.json) records the separately published [navigation follower fixture](../tests/managed_navigation_gameplay/README.md) executing as Native AOT on Linux and Windows. Linux passes ten checks over 1,009 RPCs with five clean owner exits; Windows passes nine checks over 998 RPCs with four clean exits. The additional Linux check uses a navigation-disabled simulation host. Both check a real native cover detour and arrival, Tick quota/reset and failed-attempt accounting, rollback, save/fresh continuation and immediate replan; replacement rejects. CoreCLR has separate route runs and compatible-reload checks on both operating systems. This does not establish in-player navigation/save UX, general raw Runtime ABI validation, crowds, dynamic obstacles, GUI/GPU behavior, performance or clean-machine deployment.
