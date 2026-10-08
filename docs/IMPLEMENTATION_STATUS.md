# Implementation status

Updated 2026-10-08 for **0.0.62**. Capability discovery reports what is available in each build. The desktop editor and CLI are first-class interfaces over shared native authoring, a Vulkan renderer with forward and optional deferred opaque paths, a configurable physics runtime and a continuous native player. Headless builds run without the editor or its managed runtime. The gameplay SDK and broader production qualification remain incomplete.

## Current limits

Version 0.0.62 adds a compiled CPU recipe baker for tileable brick and plaster.
Versioned recipes produce correlated base-color, tangent-normal and packed
metallic/roughness images with existing mip cooking. Bounded jobs support
inspection, cancellation and forgetting; generation leaves authored revisions
and history unchanged. Applying the suggested material uses the ordinary guarded
transaction API. Runtime bundles retain image dependencies, while recipe
manifests remain authoring data.

Windows and Linux runtime builds pass six detailed protocol groups each, covering
channel encoding, job retention, reopening, guarded application, undo, references,
export closure and shared/read-only sessions. Native checks cover independent
flat-channel and slope oracles, periodic surfaces, odd mips, failed publication,
UTF-8 diagnostics and worker cleanup. AMD and NVIDIA each pass seven rendered
captures and eight fixture assertions, including a flat normal control, generated
normal products, repeated pixels, two viewing distances and a curved tangent
fixture, with zero reported NVRHI errors.

This is a small built-in material generator, not a general graph or an
artist-replacement benchmark. UVs and tangent frames must be authored; normal
mips are not variance-aware. Recipe inspection validates descriptor/package
coherence rather than authenticating derivation. Same-profile regeneration
checks actual generated output; universal cross-compiler byte identity and
power-loss durability are not claimed. Discovery advances to revision 56;
protocol 1, authoring-core v1, gameplay service profiles and save formats are
unchanged. [Material workflow](PROCEDURAL_MATERIALS.md),
[evidence](evidence/m2-procedural-materials.json).

Version 0.0.61 adds camera-free CharacterControllers and compiled per-tick
movement, look and jump through `ICharacterInputGame` and `SetCharacterInput`.
Commands stage before physics, do not appear as caller inputs and do not change
same-tick reads. Caller input owns its target for the whole requested batch;
conflicts and later failures restore the batch's complete observed state.
Interactive player selection still requires a camera-bound controller.

Windows and Linux each pass six actual CoreCLR checks over 237 RPCs and five
published Native AOT checks over 213 RPCs. They cover wall contact, neutral
ticks, jump, invalid commands, second-tick rollback and durable fresh-process
continuation; CoreCLR also covers compatible and failed reload. Each OS passes
65 SDK/bridge ABI guard groups using protected allocations and mock callbacks.
These guard tests do not qualify arbitrary malformed Runtime callback storage.

The independent named 216-byte services-7 profile retains the original
176/192/208-byte profiles. Preserved 176-byte Native AOT Tick/Control fixtures
and original 208-byte layer artifacts execute on both OSes. Actual 0.0.60 versus
0.0.61 player input and save comparisons pass 108 RPCs per OS. Discovery advances
to revision 55 for nullable camera bindings; protocol 1, selected authoring-core
v1 and save formats remain unchanged. Navigation, acceleration and production
locomotion remain separate work. Native AOT libraries remain process-pinned.
[Character API](CHARACTER_INPUT.md), [evidence](evidence/m2-character-input.json).

The accompanying Patrol Room sample passes four native-input groups over
1,483 RPCs on each OS, with two clean owned exits per cohort. Checks exercise
actual capsule patrol, physics-ray occlusion, noise investigation, visual chase,
earned extraction, capture, compatible reload and exact fresh-process save
continuation. AMD and NVIDIA Vulkan cohorts each pass four rendered readbacks
over 1,531 RPCs, with zero reported NVRHI errors. These are primitive-room
gameplay checks, not general navigation, physical-device or performance evidence.
[Sample](../examples/managed/PatrolGame), [evidence](evidence/m2-patrol-game.json).

Version 0.0.60 adds `world.asset.references`: typed model/image/audio bindings
by authored entity or template, with inverse asset lookup and revision-pinned
pages. Queries can inspect missing or corrupt package references without loading
assets. They read the current authored document while gameplay retains its frozen
content, and support shared and read-only sessions.

Windows and Linux runtime builds, plus an authoring-only Linux build, each pass
eight protocol groups with clean owned exits. Selected native/MCP/core checks
and stable authoring request, response and identity gates pass. Discovery moves
to revision 54; protocol 1 and selected authoring-core v1 are unchanged. The new
query is outside that stable contract. Effective material graphs, live spawned
references, source-file tracking and editor reference panels remain separate work.
[Reference API](ASSET_REFERENCES.md), [evidence](evidence/m2-asset-references.json).

Version 0.0.59 isolates generated build metadata in one native translation unit.
Version-only dependency probes schedule one compile instead of seven; ordinary
source changes and relinking retain their costs. Compiler optimization, warnings
and debug settings are unchanged. Native metadata checks verify typed CLI data
against actual ELF/PE targets and match MCP initialization and exported profiler
trace versions on Windows, Linux and an authoring-only Linux build.

Selected native/protocol checks pass after a stale profiler schema pin is
corrected. Preserved Native AOT layer fixtures pass eight groups over 501 RPCs
with four clean exits on each OS. Stable discovery/identity and response gates
pass. Protocol 1, discovery revision 53, authoring-core v1, gameplay service
profiles and save formats remain unchanged. The dependency probes are scheduled
compile counts, not general elapsed-time or game-performance measurements.
[Build workflow](BUILD.md#incremental-version-builds),
[evidence](evidence/m2-build-metadata.json).

The 0.0.58 checkpoint exposes masked animation layers to compiled C# gameplay.
`IMaskedAnimationGame` opts into the named `animation_layers_v1` extension;
`GetAnimationLayer` reads committed state and `SetAnimationLayer` stages complete
clip/weight commands. Override/additive definitions, masks and reference poses
remain frozen at runtime start. Commands share the existing 64-command tick
budget with base animation and participate in whole-batch rollback.

Windows and Linux each pass eight real CoreCLR checks over 513 RPCs and eight
published Native AOT checks over 501 RPCs, with four clean owned exits per
cohort. Checks cover analytic layered motion, conflict/budget rejection, active
transition save/restore, immediate compiled re-interruption and complete-payload
rollback. CoreCLR additionally covers compatible/failed reload. Native AOT
libraries remain process-pinned. Both platforms pass 59 SDK/bridge ABI guard
groups; these use mock native callbacks and do not qualify malformed raw Runtime
callback storage.

The original 176-byte and inertial 192-byte services-7 tables remain intact;
layer access uses a separate 208-byte named extension and dedicated versioned
command/state records. Existing 192-byte Native AOT animation artifacts pass
435 RPCs per OS; an unchanged 176-byte Windows inventory artifact passes 151
RPCs. Actual 0.0.57-versus-0.0.58 unlayered comparisons pass 1,036 RPCs per OS.
Twenty-two selected Linux groups and sixteen Windows groups pass. Authoring-only
checks and all three stable discovery/identity gates pass. Protocol 1, discovery
revision 53, authoring-core v1 and animation save formats remain unchanged.

Requirement validation precedes game construction in supported matched
bridge/SDK cohorts. Arbitrarily mixing an older bridge with a newer SDK is not
supported and is outside that guarantee. Desktop layer widgets, IK, root motion,
retargeting and game-scale performance remain unfinished. This checkpoint adds
no installed desktop, GPU, physical-input, console or browser qualification.
[Managed layer API](MANAGED_GAMEPLAY.md#control-masked-layers-from-c),
[evidence](evidence/m2-managed-animation-layers.json).

The 0.0.57 checkpoint adds up to four frozen masked animation layers per rig,
with ordered override/additive composition, independent playback and motion
history, clip transitions and linear weight ramps. Native commands and joined
observations expose the layers; rollback and nested animation-state v3 saves
retain their clocks, transitions, weights and exact mask/reference identity.
Unlayered v1/v2 saves retain their previous bytes and behavior.

Windows and Linux each pass eight layer protocol checks over 146 RPCs and nine
clean owned exits. An actual 0.0.56-versus-0.0.57 comparison passes 1,036 RPCs on
each OS, including byte-identical unlayered saves and fresh-process continuation
with immediate re-interruption. The unchanged compiled C# fixtures pass ten
CoreCLR checks (486 RPCs) and seven Native AOT checks (435 RPCs) per platform;
these fixtures remain base-only and do not qualify C# layer access.

Both laptop GPUs pass 36 exact reference pairs across 72 captures at 640×480
with 1×/4× MSAA and zero reported NVRHI errors. Sixteen selected Windows checks
and seven authoring-only Linux groups pass. The initial Linux cohort passed 17
of 19 groups; two legacy discovery-shape failures were fixed, and all six affected
groups passed their final rerun. A fresh final layer and legacy comparison also
pass. This is not a fresh simultaneous rerun of all nineteen groups.

Discovery advances to revision 53. Protocol 1, selected authoring-core v1 and the
176/192-byte gameplay services-7 tables remain unchanged. Bounds of four slots,
128 rigs and 20,000 aggregate full-model layer nodes are admission limits, not
measured throughput. Maximum configurations and the 64 MiB outer-save ceiling
are not jointly qualified. At that checkpoint, compiled C# layer controls,
desktop layer widgets, IK, root motion and retargeting remained unfinished.
That checkpoint added no physical-input or game-scale performance qualification.
[Layer contract](RUNTIME_ANIMATION.md#masked-animation-layers),
[evidence](evidence/m2-animation-layers.json).

The 0.0.56 checkpoint adds opt-in C# inertial animation through
`IInertialAnimationGame`, typed mode selection and extended committed-state
queries. A named 192-byte services-7 extension preserves the original 176-byte
baseline and seven-parameter animation setter. Numeric-time calls, including
`0` and `default`, still compile against the old overload. Negotiated requirements
remain outside the state schema and its save fingerprint. Unsupported declared
requirements reject before game construction, `Initialize` and `Tick`; arbitrary
assembly/module initialization and native loader side effects are outside that
guarantee.

Windows and Linux each pass 54 SDK/bridge ABI guard groups, ten real CoreCLR
checks over 486 RPCs with six clean owned exits, and seven actual Native AOT
checks over 435 RPCs with four clean owned exits. These cover analytic motion,
staged writes, limits/conflicts, whole-batch rollback and fresh durable restore
with immediate compiled re-interruption. CoreCLR additionally checks compatible
and failed reload, preserved older compiled gameplay and old-host rejection.
Native AOT checks descriptor omission and retains process-pinned libraries;
compatible native reload is not supplied. An unchanged 0.0.52 Windows inventory
artifact also passes its 151-RPC contract on the new runtime.

Eighteen selected Linux native groups and four authoring-only groups pass. Stable
authoring discovery/identity gates pass for Windows/Linux runtime and Linux
authoring-only builds; selected response baselines remain compatible. Protocol 1, discovery revision 52 and authoring-core
v1 remain unchanged. Mock ABI fixtures do not qualify malformed raw Runtime
callbacks. There is no new GPU, installed desktop, physical-input, clean-machine
or production performance qualification. Desktop animation controls still select
crossfades; layers, IK, root motion and retargeting remain unfinished.
[Managed example](MANAGED_GAMEPLAY.md#control-animation-from-c),
[native artifact contract](NATIVE_GAMEPLAY.md#artifact-contents),
[evidence](evidence/m2-managed-inertial.json).

The 0.0.55 checkpoint adds opt-in native inertial animation transitions. Recent
output motion and analytic incoming clip derivatives initialize bounded,
finite-time translation, quaternion and positive-scale corrections. Same-tick
samples retain distinct-tick history; checkpoints and nested animation-state v2
preserve it through rollback, restoration and immediate re-interruption.
Crossfade-only saves retain version-1 bytes, and an actual 0.0.54 interrupted
crossfade save continues unchanged on the new Windows host.

Native/math, save and protocol checks pass on Windows and Linux. The Linux
regression cohort passed nineteen groups initially; a stale discovery-revision
assertion was updated and passed its focused rerun. Seven authoring-only groups
also pass. Across both laptop GPUs, 96 captures produce 24 exact inertial reference
pairs and 24 legacy crossfade pairs at 640×480 with 1×/4× MSAA and zero reported
NVRHI errors. Existing compiled C# crossfade/reload/rollback checks pass in twelve
checks per platform. Discovery advances to revision 52; authoring-core v1 and the
176-byte gameplay services ABI 7 remained unchanged. At that checkpoint C# and
desktop controls selected crossfades; inertial access used C++ or the world protocol.
Measured secant continuity does not guarantee smooth clip discontinuities, foot
contacts, phase matching or zero overshoot. No game-scale performance, new
installed desktop or physical-input qualification is claimed.
[Animation contract](RUNTIME_ANIMATION.md#inertial-transitions),
[evidence](evidence/m2-animation-inertial.json).

The 0.0.54 checkpoint adds `runtime.observe`: a bounded, exact-tick read joining
selected live entity state and native custom-component fields in one serialized
owner dispatch. Metadata is returned once per selected type; optional revisions
reject stale observations and guard query continuations. Nine protocol tests pass
on both Linux and Windows, alongside shared/read-only scope checks and an actual
authoring-only build. Discovery advances to revision 51; the selected stable
authoring-core v1 contract, gameplay ABI and save formats remain unchanged.

One provider-free replay comparison uses the same compiled Workshop Relay game
and final Windows host for both read paths. Native RPCs decrease from 2,549 to
1,292 (49.3%); compact reserialized request/result JSON bodies decrease by 11.7%
and 16.9%, respectively. Thirteen game checks, thirteen route events and six
complete checked snapshots match, with only opaque runtime session IDs
normalized. Each mode closes both owned native processes cleanly. This measures
request count and defined JSON-body size, not provider tokens, latency or game
performance. Byte-budget overflow, stale nonzero compiled gameplay/control pins and
physical input remain unqualified.
[Observation protocol and reproduction](RUNTIME_OBSERVATION.md),
[evidence](evidence/m2-runtime-observation.json).

The 0.0.53 checkpoint retains a third external-agent game exercise: Codex authors,
compiles and playtests Workshop Relay with generated capacity-three int32
inventory, six native spawned parts, ordered recipes, deferred Drop, typed styled
UI and compiled Pause/Resume/Save/Load controls. Independent original-binary
verification passes 2,550 requests; a fresh Windows public-source rebuild passes
2,549 requests on the final 0.0.53 host. Both restore a nonempty `[A,B]` checkpoint,
complete reflected state, remaining native identities/poses and logical modal UI
through compiled Load and a fresh process, then finish the second delivery.
Two inspected NVIDIA readbacks show the wine-styled checkpoint menu and completed
HUD. This is one bounded exercise with primitive geometry, not general autonomy,
physical controls or performance qualification. The generated game's fixed Drop
position lacks wall/overlap validation, and its Save label acknowledges acceptance
before storage completion; independent tests check actual durable completion.
True interaction occlusion, nonzero pending-intent saves and Linux/Native AOT
replay of this game remain unqualified.
[Retained source and replay](evidence/fixtures/agent-workshop/README.md),
[evidence](evidence/m2-agent-workshop.json).

The 0.0.52 checkpoint qualifies the existing generated capacity-four entity/int32
component buffers on Windows Native AOT. Publication uses the real fixture and
extracts the same component manifest as the CoreCLR build. Its unchanged 151-RPC
contract passes compiled mutation, capacity rejection, byte-exact rollback and two
fresh-process save continuations. Exported games run seven ticks after relocation
outside the checkout on AMD and NVIDIA, with zero reported NVRHI errors and an
unchanged 56-file bundle. Separate save checks also pass through each bundled
runtime; these use their own fixture world and do not demonstrate in-player save
UX. Native PE payload checks and disabled dynamic-code flags are recorded; the
host has .NET installed, so clean-machine deployment remains unqualified. No
runtime implementation, gameplay ABI or catalog change is introduced. [Native collection evidence](evidence/m2-component-collections-native-windows.json).

The 0.0.51 checkpoint separately qualifies these buffers on Windows CoreCLR with
matching SDK/generator/bridge builds, compiled writes and two fresh-process save
continuations. Global-state collections, arbitrary managed arrays and capacity
migration remain unqualified. [Collection contract](COMPONENT_COLLECTIONS.md),
[CoreCLR evidence](evidence/m2-component-collections-windows.json).

The 0.0.50 checkpoint adds bounded typed runtime UI layout/style authoring and
`world.ui.layout` for virtual-viewport geometry inspection without a window or GPU.
Explicit layouts support anchored HUDs and responsive flex menus; optional colors,
font/border settings and state colors override the compatibility fallback.
Frozen metadata participates in save content identity; absent metadata stays absent.
Presentation input follows visual order and rejects confirmation through an
occluding panel. Theme inheritance, images, widgets, animation, accessibility and
visual UI authoring remain unfinished. These UI interfaces are outside the selected
stable authoring contract. [UI contract](GAME_UI.md#authored-layout-and-styling).
Fifteen selected Linux headless and ten native-layout groups, seventeen Windows
check commands and both GPU capture suites pass. Seven old/new default UI
capture pairs on AMD are pixel-identical to 0.0.49; eighteen styled captures
verify three extents at density 1 on AMD and NVIDIA. Existing compiled Windows
CoreCLR/Native AOT control fixtures also pass. This is native readback and
synthetic-input evidence, without physical-device qualification. [Evidence](evidence/m2-authored-ui.json).

The 0.0.49 checkpoint releases a selected **authoring-core v1** boundary: nine world/entity methods, entity create/rename/reparent/delete and Transform edits. Native discovery identifies the contract and mode across full/focused views. Canonical request and response artifacts retain the earlier candidate schemas; their paths and hashes are pinned separately. A dedicated output-compatibility gate proves recognized producer-to-consumer restrictions, while native conformance covers RPC IDs/errors, retained retries after edits/restart, true hierarchical shear, query boundaries and failed undo/redo storage. Read-only scopes reject even valid mutations and retained receipts before replay. This does not freeze runtime/rendering/gameplay/save APIs or world formats, or establish alpha readiness. Windows/Linux service and installed-client qualification are recorded in [release evidence](evidence/m2-authoring-core-v1.json). GUI/GPU qualification is outside this checkpoint. [Contract](AUTHORING_API_COMPATIBILITY.md).

The 0.0.48 discovery view selects a mutation operation and optional component type, keeping the full transaction envelope and applicable session metadata. Native tests compare every advertised operation and component selection with full discovery, reject malformed/unavailable selectors without editing state/history/files, preserve mixed transactions, and enforce read-only restrictions in standalone and both shared scopes. MCP exposes these selectors; the updated Python client validates selection echoes, writable metadata and envelope shape. The candidate baseline stays unchanged. A conservative closed-object extension proof qualifies additive discovery requests, and reference-bearing schemas retain their full operation unions to preserve indexed targets. Windows wire measurements show a Transform mutation response of 1,318 bytes versus 27,779 for the complete method, and MCP 2,910 versus 60,276. Counts exclude line delimiters and do not measure provider tokens or development speed. Rebuilt Windows/Linux hosts and installed Python wheels have bounded qualification; broader API stability, fresh provider turns and GUI/GPU presentation are outside this checkpoint. [Discovery contract](WORLD_SERVICE.md#focused-discovery-development), [client](PYTHON_CLIENT.md), [evidence](evidence/m2-scoped-mutation-discovery.json).

The 0.0.47 headless owner waits for local transport progress after dispatch instead of repeatedly sleeping. Windows servers use persistent overlapped operations; Linux servers use eligible socket readiness. Native regressions cover fragmented frames, synchronous completion, budget continuation, deferred pipelined requests, all eight occupied client slots, slow-reader fairness and pending-operation destruction/reuse. Windows shared-host, SDK, MCP and ten native desktop ABI checks pass; seven selected Linux contract groups and both candidate request gates pass. Four matched Windows trial pairs reduce shared query median from 16.03 ms to 0.159 ms (p95 16.59 ms to 0.256 ms). Single five-second idle intervals record Windows host CPU time 0.046875 s before and zero delta after, and Linux 0.07 s before and 0.01 s after. Zero accounting deltas do not prove zero CPU work or energy use. The unchanged compiled defense game passes 3,208 calls in 2.28 s, preserving all seven tick and gameplay-value summaries and exact checkpoint restoration and clean exits from six owned processes. This is bounded same-machine evidence, not game FPS, GUI responsiveness, battery life or general agent success. Windows cancellation/reaping can exceed wait deadlines; the headless owner retains a 100 ms maintenance ceiling. Desktop dispatch cadence is unchanged. [Transport and probes](SHARED_SESSIONS.md#transport-and-limits), [checkpoint evidence](evidence/m2-host-readiness.json).

The 0.0.46 Windows client waits on overlapped pipe completion instead of sleeping and retrying during exchanges. One absolute deadline covers partial reads and writes; failure cancels and reaps pending I/O before releasing buffers and closes the connection without replay. Kernel cancellation cleanup can exceed that deadline. Native Windows tests identify pending read/write timeouts directly and cover disconnects, fragmented responses and fresh endpoint recovery; shared-host, Python SDK, MCP and ten native desktop ABI checks also pass. Four matched Windows trial pairs measure shared read median 31.4 ms before versus 15.9 ms after, with an old-build repeat at 32.2 ms. The unchanged compiled defense game passes 3,208 shared-transport calls and reproduces every previously recorded checkpoint and win/loss outcome; all six owned processes exit cleanly. These are one-machine authoring reads, not game FPS or GUI responsiveness. Seven selected Linux transport/client/core-contract test groups and the Windows/Linux candidate request gates pass. Server polling, startup retries, Linux socket waits and editor dispatch cadence are unchanged. [Transport contract](SHARED_SESSIONS.md#transport-and-limits), [checkpoint evidence](evidence/m2-client-completion-waits.json).

The 0.0.45 checkpoint attempts bounded reply delivery immediately after an owner dispatch, retaining partial writes for later polls. Native transport checks pass on Windows and Linux, including immediate small/empty frames, slow-reader progress and pipelined ordering. A new regression rejects the old queued-only backend. Four alternating paired Windows authoring trials measure shared query median 47.6 ms before versus 31.8 ms after; this is one-machine read latency, not game FPS or GUI responsiveness. SDK, shared-host and MCP checks pass; the native desktop ABI passes ten checks without GPU attachment. Client retry sleeps and editor dispatch cadence remain unchanged. [Transport contract and benchmark](SHARED_SESSIONS.md#transport-and-limits).

The 0.0.44 checkpoint adds a second external-agent game exercise: Claude Code creates, compiles and headlessly playtests a three-wave defense game through native MCP authoring. Independent native controller inputs check cover occlusion, two-hit drones, finite ammunition, cooldown, reload, victory, unattended loss and complete checkpoint continuation in a separate process. The [public fixture](evidence/fixtures/agent-defense/README.md) retains the generated C# source unchanged and provides a Python replay runner. This is Windows CoreCLR headless qualification with primitive geometry and prescribed kinematic lanes; graphical presentation, physical input, navigation, Native AOT and Linux execution of this fixture remain unqualified. [Exercise and replay evidence](evidence/m2-agent-defense.json).

The 0.0.43 checkpoint adds an installable [Python automation client](PYTHON_CLIENT.md), with explicit owned-process/endpoint transport, guarded mutation helpers and revision-pinned pagination. A separate candidate response manifest validates the nine core authoring methods without rejecting legacy retained receipts or equating a replay revision with current state. Native standalone/shared-session behavior, synthetic transport failures and independent malformed-response checks pass on Windows and Linux. Local installed-wheel checks qualify packaging; this does not qualify GUI/GPU/provider integration or freeze broader runtime/gameplay/save contracts. [Evidence](evidence/m2-python-client.json).

The 0.0.42 checkpoint adds a [candidate authoring-core API gate](AUTHORING_API_COMPATIBILITY.md). Its nine selected methods and Transform preserve the schema46 request baseline on rebuilt schema47 Windows and Linux hosts. Eleven compatibility tests and nine scope-projection tests pass, including overlapping exclusive branches and unresolved references. Native checks cover identity defaults, isolated previews, guarded revisions and retry behavior. This is preparation for a stable authoring contract, not a released guarantee for the full engine. [Evidence](evidence/m2-authoring-api-compatibility.json).

The retained [agent-built escape game](evidence/fixtures/agent-escape/README.md#rendered-checkpoints) now has three inspected Windows hardware Vulkan readbacks: locked exit, completed escape and restored checkpoint. Controller input and compiled save callbacks pass, with zero reported NVRHI errors and unchanged capture state. The slideshow is recorded agent authoring followed by fresh rendered replay, not live authoring or a real-time video. Primitive geometry and mid-word HUD wrapping remain visible limits. [Evidence](evidence/m2-agent-game-rendered.json).

[Structured compiler feedback](DEVELOPMENT_JOBS.md#structured-compiler-feedback-development), introduced in 0.0.41 with discovery schema47, now passes real SDK success/failure checks through rebuilt native world hosts on both platforms. Twelve focused Linux test groups pass; Windows world/history/UI checks and native read-only/shared-scope checks pass. All six development methods are hidden and rejected in read-only worlds before parameter validation, without modifying bundle files. Newly exported player qualification is not implied. [Host evidence](evidence/m2-development-host.json); [earlier standalone evidence](evidence/m2-development-diagnostics.json). Earlier changelog entries retain their original contents and qualification scope.

The [0.0.40 changelog](../CHANGELOG.md) collects development since the earlier 0.0.39 editor package. That source checkpoint and its editor project metadata identified 0.0.40. The full Linux rebuild passes after header dependency cleanup; 71 of 73 Linux test groups pass, including gameplay packaging. At that checkpoint, `local_session_native` and `shared_session_contract` could not bind sockets under the restricted environment. Both subsequently pass after filesystem/process restrictions were lifted; this is a focused rerun, not a new full-suite result. After rebuilding all optional baseline objects and confirming a clean Ninja dry run, renderer-header dependencies fall from 77 to 17 across the same Linux objects (60 removed). This measures dependency fanout; no compile-time speedup is claimed. [Dependency evidence](evidence/m2-build-dependencies.json). The Windows engine/desktop-bridge rebuild and optional graphics fixtures compile successfully. Its renderer-header dependency count falls from 85 to 31 objects (54 removed), with a real Ninja no-op build confirming freshness. Experimental [ambient occlusion](AMBIENT_OCCLUSION.md) has passing Linux API/schedule contracts and bounded hardware capture, reconstruction lifecycle and UI checks on both laptop GPUs under Vulkan synchronization validation. The default forward renderer matches 16 saved reference captures per device exactly; general quality and performance remain unqualified. [AO evidence](evidence/m2-ambient-occlusion.json). Earlier evidence below retains its original checkpoint scope and does not constitute a complete 0.0.40 release qualification.

The subsequent [focused discovery](WORLD_SERVICE.md#focused-discovery-development) change passes Linux protocol and native authoring/read-only scope tests. Clients can list names or request one schema/contract section; full discovery remains the default. Its Windows engine, desktop bridge and native test fixture rebuild also passes; Windows execution remains unqualified. [Evidence](evidence/m2-focused-discovery.json).

A fresh external Codex-authored escape-room exercise now passes compiled C# gameplay and independent headless replay: locked exit, three raycast pickups, pause/resume, checkpoint restoration and a second completion. The portable source fixture separately passes Windows CoreCLR replay and durable checkpoint continuation in a fresh native process, using its rebuilt DLL and cached compatible engine. This is one game with logical UI; rendering, physical input, Linux/Native AOT fixture replay and performance remain unqualified. [Original exercise](evidence/m2-agent-game.json), [public replay and source](evidence/m2-agent-game-replay.json).

The development [native compile-worker foundation](DEVELOPMENT_JOBS.md) passes standalone process contracts on Linux and Windows, plus standalone adapter retry/diagnostic contracts on both operating systems and real Linux SDK builds. World-service source integration is staged; full host and packaged read-only execution remain pending. [Initial worker evidence](evidence/m2-development-jobs.json), [current adapter and diagnostic evidence](evidence/m2-development-diagnostics.json).

The [native MCP interface](MCP.md) passes Linux protocol, real stdio authoring/persistence and CLI/world regression checks. Real stdio authoring and shared headless endpoint tests now also pass on Windows and Linux, including cross-client receipts and safe detachment. Bounded real Codex/Claude CLI authoring exercises now pass, with independent trace and persisted-state verification. [Agent-client evidence](evidence/m2-agent-authoring.json). A candidate Windows desktop Agent window now passes one Codex chat turn that creates exactly one guarded entity through the editor-owned endpoint, with missing-executable recovery and unchanged existing entities. Physical input, approval-card interaction, interrupt completion, Claude chat and broad game creation remain unqualified. [Workspace evidence](evidence/m2-agent-workspace.json). [Initial evidence](evidence/m2-native-mcp.json), [cross-platform endpoint evidence](evidence/m2-mcp-shared.json). Opt-in PNG image content now passes Linux codec/MCP tests and archived SDL capture conversion; fresh Windows GPU images now reach both agent CLIs, which pass a simple undisclosed-color identification exercise. This does not establish general visual reasoning. [Observation evidence](evidence/m2-agent-observation.json). [Image evidence](evidence/m2-mcp-images.json).

Optional [single-sample deferred rendering](DEFERRED_RENDERING.md) passes bounded analytic lighting, shadows, clustered-light fallback, sky, skinning, motion, reconstruction and UI checks on both laptop GPUs under strict Vulkan validation. The default forward path retains exact saved-reference image comparisons. Deferred adds 32 bytes of material storage per render pixel; expanded products remain allocated. These checks establish integration correctness, not a performance gain, GI or game-scale readiness. [Evidence](evidence/m2-deferred-rendering.json).

Optional [FSR 3.1.4 reconstruction](RECONSTRUCTION.md) is implemented for the Windows Vulkan renderer, disabled by default. Native AA and three fixed upscaling ratios pass bounded HDR, reset/resize, independent-view, capture API and UI-composition checks on both laptop GPUs under strict Vulkan validation. Existing rendering with reconstruction disabled retains exact reference comparisons. The separate SDK-disabled renderer builds and rejects unavailable FSR requests. Static edge improvement coexists with persistent near-edge color residue in moving scenes; temporal quality and performance are not qualified. Frame generation, DLSS and live-player timing qualification remain unfinished. [Evidence and limitations](evidence/m2-reconstruction.json).

Single-sample development rendering provides [depth, world-space shading normals and backward UV motion](SCENE_PRODUCTS.md), with explicit surface/correspondence validity and up to 64 raw pixel probes per capture. Per-view history follows accepted submissions, including camera/object movement and prior skinned positions; source replacement, cuts and changed renderable lifetimes invalidate correspondence. Strict Vulkan synchronization validation passes on both laptop GPUs: each passes 80 native captures, including interleaved views, plus four agent-service captures. Nine Linux contract groups, 221 valid schedule configurations and 328 rejection checks pass. This establishes bounded geometric correspondence, not occlusion rejection, temporal antialiasing, upscaling or game-scale performance. Multisample products and full floating-point image export remain unfinished. [Evidence](evidence/m2-scene-motion.json).

Reconstruction timing now separates live players/editor views from frozen capture and input replay. The clock commits dispatch timestamps only after accepted submission, clamps live intervals and restarts on history reset. Synthetic clock tests pass on Linux and Windows; live-player image quality and pacing remain unqualified. [Timing checks](evidence/m2-temporal-clock.json).

The earlier [depth/normal checkpoint](evidence/m2-scene-products.json) records 26 analytic display captures per GPU, exact single-sample color comparison and continuous resize/recreation checks. Its quantized BMP checks are separate from the new raw pixel probes.

The development renderer now executes a [typed pass schedule](RENDER_SCHEDULE.md) that validates actual resource identities, initialization, usage and frame-slot ownership before recording. Portable checks cover 204 valid variants and 250 rejection checks. On both laptop GPUs, the triangle and 16 scene captures match the preceding renderer exactly under Vulkan synchronization validation; ten renderer, eight UI/player/hosted-view and four legacy editor layout groups pass. This retains the existing rendering order and does not add GI or automatic pass parallelism. [Evidence](evidence/m2-render-schedule.json).

The development renderer now uses bounded one- or two-frame submission with per-frame completion, query and readback ownership. Strict Vulkan and synchronization validation pass on both laptop GPUs: 16 captures per GPU preserve exact reference parity, all 49 changing-frame results per mode retain submission attribution, and warmed draw phases add no device-wide idle waits. Ten renderer regression groups, eight UI/player/hosted-view groups and eight Linux headless contract suites pass. GPU scratch remains ordered, uploads can still stall, and presentation cleanup differs by device; this is correctness qualification, not a performance gain. [Contract](RENDER_DIAGNOSTICS.md#submission-and-presentation-lifetime), [evidence](evidence/m2-frame-retirement.json).

The development [scene HDR composition path](HDR_COMPOSITION.md) now passes a full Windows build, 174 scene captures and eight UI/player/native-viewport integration groups across both laptop GPUs. Floating-point scene color resolves MSAA before one exposure/output transform; UI remains independent of exposure. This is bounded correctness evidence, not performance qualification. HDR monitor output, GI and post-processing effects remain unfinished. [Evidence](evidence/m2-hdr-composition.json).

[Explicit scalar save upgrades](SAVE_UPGRADES.md) now connect stable-ID mapping, authored-content validation and snapshot transformation to guarded external `save.load` in development. Linux CoreCLR integration tests cover real old-save/new-game continuation, component changes, rejection isolation, retry and recovery; Windows and Native AOT integration qualification remains pending. Automatic gameplay/editor upgrade selection is unfinished. Ordinary restores remain exact. The earlier cross-platform [primitive evidence](evidence/m2-save-upgrade-primitives.json) covers mapping/plan guards only.

The latest development [C# lifecycle checkpoint](GAMEPLAY_LIFECYCLE.md) passes five compiled integration groups under both CoreCLR and Native AOT on Linux and Windows. Agent-service tests cover guarded births, durable restoration and continued gameplay; the CoreCLR path also verifies compatible reload with spawned entities. This is root-prop support, not general entity/component lifecycle. [Qualification evidence](evidence/m2-managed-lifecycle.json).

[Custom components](CUSTOM_COMPONENTS.md) provide native-owned per-entity data with stable IDs and generated C# accessors. Spawned root props can carry template-defined components during Play; development [bounded collections](COMPONENT_COLLECTIONS.md) now pass Linux native and real CoreCLR gameplay/save tests. Windows CoreCLR and Native AOT capacity-four entity/int32 buffers are now qualified separately. Arbitrary component addition/removal on existing entities and general schema/save migrations remain unfinished. Services ABI 7 now has a bounded compatibility contract; upgrading from earlier epochs still requires matching rebuilt artifacts.

The development [standalone template catalog](RUNTIME.md#standalone-template-catalog-development) adds transactional root-prop recipes without requiring live prototype objects. Runtime catalog inspection uses a frozen authored revision, and template-only models/textures participate in content dependencies. A guarded world-service transaction can create and remove spawned root props at paused boundaries, with session-scoped retry receipts. Version 3 snapshots preserve those spawned objects and generated-ID history. Native hosts can schedule guarded structural edits within an atomic multi-tick batch. The [C# lifecycle API](GAMEPLAY_LIFECYCLE.md) reserves IDs, initializes registered components, starts kinematic movement and removes spawned props within Tick. It uses the services ABI 7 baseline (176 bytes); earlier service epochs require coordinated rebuilding. RPC tick scheduling, arbitrary authored-entity removal, desktop template authoring and a dedicated generated-prop hierarchy/Inspector remain unfinished. [Service and desktop lifecycle evidence](evidence/m2-runtime-lifecycle-service.json). Tick-guarded RPC mutations also require the observed structure revision after membership changes; editor component, gameplay and animation drafts retain it. Linux and Windows pass the focused guards and save regressions, and Windows passes five native desktop checks. This checkpoint does not qualify the full GUI or activate a new desktop package. [Structure-guard evidence](evidence/m2-runtime-structure-guards.json).

[Keyboard/mouse profiles](INPUT_PROFILES.md), [gamepad profiles](GAMEPADS.md), configurable bindings, device discovery and hosted editor assignment are implemented. Physical gamepad qualification remains outstanding. General settings remain unfinished. The experimental [save-slot service](RUNTIME.md#durable-save-slots) adds durable generations, explicit recovery and guarded runtime replacement to the native snapshot foundation. The [editor Save/Load window](EDITOR_SAVES.md) now uses that service. [Typed gameplay requests](GAMEPLAY_SAVES.md) use a post-batch native owner. General migrations and asynchronous save scheduling remain unfinished. Authored document recovery and internal runtime rollback are separate contracts.

Cinematic authoring, comprehensive post-processing and a general package manager are not implemented. Direct FBX import and Mixamo character workflows are not qualified; the supported asset formats and limits are documented in [Assets](ASSETS.md) and [Animation assets](ANIMATION_ASSETS.md).

The [native profiler](PROFILER.md) now provides bounded shared captures, an editor CPU timeline, subsystem summaries, separate GPU duration samples and trace export. Worker-thread tracing, allocation/GC and process/VRAM tracking, GPU clock correlation and a sustained benchmark workflow remain unfinished. Existing [render diagnostics](RENDER_DIAGNOSTICS.md) also retain capture/player aggregates and draw counters. Scene transform handles are implemented; orientation/axis-view controls, camera frustums and light/component icons remain planned. The current Console is a log tab beside Project.

Planned editor workflow improvements include configurable hierarchy/folder styling, component isolation/search/copy-paste, focused object/asset tabs, persistent favorites, fullscreen panels, configurable smooth navigation, hover highlighting and overlap selection, a live preferences inspector, clickable breadcrumbs/back-forward history/context locking, and integrated Git/GitHub workflows. Existing striping, automatic icons, basic geometry picking and dock panels cover only the initial subset.

Runtime collision supports boxes, capsule controllers and explicit [static triangle meshes](MESH_COLLISION.md), preserving openings present in source geometry. Moving/deforming mesh colliders, distinct movement versus weapon-query shapes, finite-radius projectile sweeps and texture-cutout collision masks remain unfinished. Contacts use triangle front faces; rays hit both sides. Numeric/resource bounds and synthetic fixture results do not establish exact arithmetic or game-scale collision performance.

Animation supports bounded two-pose crossfades, opt-in native inertial transitions and ordered masked override/additive layers. Legacy fade interruption freezes the current pose. Inertial mode adds decaying corrections that preserve estimated distinct-tick output motion; it does not guarantee smooth clip discontinuities or foot contacts. Compiled C# layer controls, state machines and transition-history scrubbing, IK, root motion and retargeting remain unfinished. Internal batch rollback is not retained simulation history or a time-travel debugger.

## Implemented

- **Persistent gameplay metadata (development):** opt-in C# global-state fields carry stable IDs and literal defaults through CoreCLR, Native AOT, inspection and exact snapshots. Native artifacts declare a metadata capability that export and bundle inspection enforce. Both platforms pass malformed-schema, managed parity, compiled default/initialization and exact-save continuation checks; preserved old binaries still run. This supplies identities for explicit upgrades, not automatic save migration. [Contract](GAMEPLAY_PERSISTENCE.md), [evidence](evidence/m2-gameplay-persistence-metadata.json).

- **Save source validation (development):** native hosts can validate complete source snapshots without loading their old gameplay executable. Exact restore shares that validation and still requires the trusted backend/image/type/schema match. Windows and Linux pass native data checks and compiled CoreCLR/Native AOT restore regressions, including initialization guards, 16 rejection cases and 40 restore iterations per backend. This prepares explicit save evolution; cross-version migration remains unfinished. [Contract](RUNTIME.md#portable-runtime-snapshot-foundation), [evidence](evidence/m2-save-source-validation.json).

- **Gameplay compatibility (development):** service epoch 7 preserves its 176-byte baseline when a host appends opaque fields. Native artifact v2 declares required call layout, service prefix and features instead of requiring an exact producing-engine version; known 0.0.39 v1 artifacts remain supported. Windows and Linux tests execute preserved old CoreCLR and Native AOT binaries, including an old game with the new bridge/SDK. Managed prefix-validation tests pass on Windows and Linux; 37 Windows package/protocol checks and compiled UI control regressions pass. Saved schema/content/image checks remain exact, and future tail-dependent services require explicit negotiation. [Contract](NATIVE_GAMEPLAY.md), [evidence](evidence/m2-gameplay-compatibility.json).

- **Native UI input (development):** the player and Game viewport route pointer, keyboard and assigned-gamepad events through the displayed native layout into compiled controls. Interactive menus may omit a CharacterController; hosted input supports a camera-only binding. Stale visible UI consumes its own hits and held releases without executing callbacks or taking over clicks outside nonmodal controls. Focus, resize, reassignment and runtime replacement cancel gestures. Focused Linux document/presenter tests and Windows compiled desktop checks pass; physical devices and full Avalonia interaction remain unqualified. [Input contract](GAME_UI.md#player-and-game-view-input), [evidence](evidence/m2-ui-input.json).

- **Collection Room sample (development):** a small C# game combines actual capsule movement, camera raycasts, template-spawned collectibles, removal, native HUD controls and durable checkpoints. Windows CoreCLR and Native AOT checks collect all three items, restore the player and remaining objects, and continue in a fresh process. Both laptop GPUs produce initial/completed Vulkan readbacks. An exported bundle completes the scripted room from an unrelated working directory with zero reported Vulkan errors and unchanged bundle inventory. This is primitive sample geometry, not a performance benchmark; physical input, Linux sample execution and cross-version save migration remain unqualified. [Source and reproduction instructions](../examples/collection-game), [evidence](evidence/m2-collection-room.json).

- **Compiled UI controls and native presentation (development):** guarded button activation invokes C# `Control` at an unchanged physics tick, stages UI/gameplay changes and save intentions atomically and retains retry receipts across Load replacement. Successful controls advance gameplay and control revisions, protecting older Inspector/save drafts. This checkpoint introduced the services ABI 7 baseline; older service epochs require matching rebuilt artifacts. Snapshot v5 preserves the control sequence and reads older v4 UI saves with sequence zero. Native default layout feeds named-camera rendering; desktop owners apply Pause/Resume intents once. This checkpoint preceded live input routing; general layout authoring and deployment qualification remain unfinished. [Contract](GAME_UI.md), [bounded qualification](evidence/m2-ui-control-presentation.json).

- **Authoritative UI state (development):** native panel/label/button definitions now support world transactions, history, frozen runtime inspection and atomic text/visibility/enabled/modal edits at an unchanged simulation tick. Snapshot v4 preserves this state alongside spawned props and custom components; UI-free saves retain versions 1–3. Linux passes ten selected runtime suites and five authoring-only suites. Windows passes three native suites and 27 protocol test cases; the linked C# Save model verifies same-tick UI freshness and retained retry guards. At this checkpoint logical state and RmlUi presentation were separate, without automatic binding, action execution or C# UI callbacks. Live input routing was not part of that checkpoint. [Contract](GAME_UI.md#authoritative-controls), [evidence](evidence/m2-native-ui-state.json).

- **Native game UI presentation foundation:** optional pinned RmlUi/FreeType layout produces owned geometry and glyph atlases for the common Vulkan composition pass. Registered labels/buttons support inspection, literal text updates, visibility/enabled state, focus and returned semantic actions. Native checks pass on Windows/Linux; both laptop GPUs pass numeric captures at 1×/4× MSAA and the text HUD capture. The separate logical UI service supports authored definitions, same-tick guarded state edits and portable saves. That checkpoint did not include automatic presentation binding, C# callbacks, paused action execution or live player/desktop input routing. The first backend rejects unsupported effects and requires sRGB composition. [Contract and bounds](GAME_UI.md), [evidence](evidence/m2-native-game-ui.json).

- **0.0.39 editor audio:** opt-in Game-camera sound output uses committed snapshots, bounded worker DSP, owner-thread SDL submission, guarded Audio controls and asynchronous shutdown. All 51 runtime and 42 authoring-only Linux suites pass; Windows passes native audio/lifetime, playback/input and packaged C# checks. Audio controls pass 67 actions on each laptop GPU, with 232 general editor actions plus four restart checks. First device initialization remains synchronous: one Windows poll took 666 ms on the shared test machine. Physical listening and end-to-end latency remain unqualified. [Usage](EDITOR_AUDIO.md), [evidence](evidence/m2-editor-audio.json).

- **0.0.38 editor gamepads:** hosted SDL input now shares native profiles and fixed-tick evaluation with the player, without dispatching the editor’s Windows message loop. Stable evaluator ownership and staged device acquisition preserve existing controls on failed changes. Game Input settings expose explicit defaults/profile choice, assignment and neutral-gate status; Start releases capture only. All 49 runtime and 40 authoring-only Linux suites pass. Windows passes 18 SDL groups, seven direct bridge groups/66 RPCs, and existing keyboard/mouse and playback regressions. The final package passes 55 focused actions on each laptop GPU, 232 general actions plus four restart checks, and exact-package C# ABI/integration checks with 100 reloads. Virtual devices, hidden HWND capture-timeout tests and synthetic GUI controls do not qualify physical controller behavior. [Usage](GAMEPADS.md#desktop-editor), [evidence](evidence/m2-editor-gamepads.json) and [attached settings render](evidence/m2-editor-gamepads.png).

- **0.0.37 custom C# components:** native-owned scalar data now has stable type/field IDs, generated C# accessors, guarded authoring and paused live Inspector editing. Compatible reload retains data; staged writes and sparse journals join rollback, and snapshots preserve fixed membership. Native AOT artifacts bind verified component metadata. All 49 runtime and 40 authoring-only Linux suites pass. CoreCLR and Native AOT pass 1,170 RPCs across both OSes, including 100 compatible reloads per CoreCLR run and actual gameplay save/load. The final editor passes 133 focused actions on each laptop GPU, 232 general actions plus four restart actions, and 129 Save/Load actions. A relocated 40-file Windows game bundle runs compiled component updates and saves with its source project unavailable. That checkpoint covered scalar types, fixed membership and exact-schema saves. Subsequent lifecycle work adds template-based root props; arbitrary component membership changes and general migrations remain unfinished. [Usage](CUSTOM_COMPONENTS.md) and [evidence](evidence/m2-custom-components.json).

- **Per-tick editor playback:** automatic Play now uses guarded native one-tick commits without per-tick JSON requests or public retry receipts. A later failed tick preserves earlier successful ticks and saves; explicit multi-tick agent requests remain atomic. Input commits without allocation after success, and a bounded exact frame trace preserves mouse backlog and one-shot edges for replay. All 46 Linux runtime and 38 authoring-only suites pass, including typed/RPC state parity and real physics rollback. Windows passes native units, real C# save/load/failure checks and playback/input tests on both laptop GPUs. The final package passes 232 editor actions, four restart actions and 43 Profiler actions. That checkpoint preceded the later hosted editor gamepad and audio integrations. [Playback contract](DESKTOP_EDITOR.md) and [evidence](evidence/m2-editor-tick-boundary.json).

- **Shared native profiler:** bounded opt-in captures record native CPU phases, failed/rolled-back work and session/tick context. The editor timeline, source filters, summaries and trace export use the same guarded service as agents. GPU durations remain separate observations; counters distinguish tracked bytes from counts. All 45 runtime and 37 authoring-only Linux suites pass. Real CoreCLR/Native AOT save/load and rollback traces pass on both OSes. Both laptop GPUs preserve exact profiled/unprofiled pixels and replay state. The final editor passes 43 profiler actions and the full 232-action regression plus four restart actions; its attached-content image is readable. Short paired native timing trials retain overhead distributions and equal logical state without a game-performance claim. [Contract](PROFILER.md), [evidence](evidence/m2-native-profiler.json) and [tool image](evidence/m2-profiler-window.png).

- **Typed gameplay saves and safe player replacement:** C# callbacks queue save/load requests; the native owner services them after a committed simulation batch. Failed batches roll back requests without writes. Loads create fresh sessions/epochs, preserve retry outcomes and reset player input, timing and audio without retaining the old runtime. Standalone games accept an external `--save-root`. That checkpoint introduced services ABI 4; current builds use ABI 7 and require matching rebuilt hosts and game artifacts. Real CoreCLR and Native AOT requests pass six groups/85 RPCs per OS, with a separate CoreCLR-disabled Linux run. All 42 runtime and 34 authoring-only Linux suites pass; 100 compatible C# reloads still pass. The final editor package passes 232 actions plus four restart actions and 129 Save/Load actions. Both laptop GPUs pass native replacement fixtures; a relocated compiled game saves through the final runtime. Storage remains synchronous and snapshots require exact supported content/schema. [Contract](GAMEPLAY_SAVES.md) and [evidence](evidence/m2-gameplay-save-requests.json) distinguish final-package checks from earlier player binaries and synthetic input from physical qualification.

- **Editor Save/Load window:** folder configuration and named checkpoints use the existing native service. Observed guards remain fixed until explicit inspection, failed operations retain their request IDs, and recovery choices are bound to the inspected slot. Loading restores real typed C# state without changing the authored scene; unapplied Inspector/gameplay drafts block replacement. Save drafts and pending operations prevent accidental close. The packaged editor passes 129 Save/Load actions across three fresh processes on each Windows GPU, including state continuation, corruption recovery and close guards. The unchanged full editor regression also passes 232 actions plus four restart actions. Six attached-window content renders were inspected; these are not OS screenshots or physical-input qualification. [Workflow](EDITOR_SAVES.md) and [evidence](evidence/m2-editor-saves.json).

- **Durable save-slot service:** explicit external storage roots, generation guards and 32 persisted write receipts support retryable saves and fresh-process loads. The service binds the frozen authored definition and complete referenced asset inventory, stages a candidate runtime, then activates it under a fresh session ID without changing current authoring. Two retained payloads, explicit prior-generation recovery and preservation of a failed generation cover bounded corruption handling. The editor service requires paused save mutations and clears old input/session state on replacement. Seven service groups pass on each OS, including fresh-process retry and recovery; CoreCLR and Native AOT each pass four integration groups per OS. Windows desktop service qualification passes four groups/47 RPCs. Storage tests cover nine exception and nine process-exit boundaries per run on both OSes; all 40 Linux runtime and 33 authoring-only regression suites pass. General migrations, asynchronous scheduling and power-loss qualification remain unfinished. [Contract](RUNTIME.md#durable-save-slots) and [evidence](evidence/m2-save-slots.json).

- **Native runtime snapshot foundation:** an experimental C++ API exports bounded versioned logical state and stages a separate restored runtime. It preserves body velocities/sleep, controller view state with reconstructed support, in-progress kinematic motion, interrupted animation fades, logical sounds and exact-schema typed C# values. Trusted content/module bindings reject incompatible data; restore skips gameplay `Initialize`. Three new native suites pass on Linux and Windows. The 4,012-byte fixture restores in fresh processes and across both OS directions with exact canonical bytes. CoreCLR and Native AOT each pass 16 rejection cases and 40 restore/release cycles on each OS. The full Linux regressions pass 36 runtime and 30 authoring-only suites, with the subsequently added checked-in version-1 fixture test also passing. This foundation milestone did not include storage or service activation; the save-slot service extends it. General migrations and asynchronous capture remain unfinished. [Contract](RUNTIME.md#portable-runtime-snapshot-foundation) and [evidence](evidence/m2-runtime-snapshot.json).

- **0.0.36 editor C# iteration:** session-local launch profiles load a prebuilt `Game<TState>` before the first Play tick. The C# Gameplay window exposes typed live fields and paused compatible assembly reloads through the same guarded native operations as agents. Configuration generations, runtime/tick/gameplay revisions and bounded retry receipts prevent stale replacement; invalid drafts remain available. Failed startup stops the fresh runtime without authored writes, and failed reload preserves the existing module. Windows native qualification passes eight groups/193 RPCs; the final self-contained editor passes 192 focused actions, including 100 compatible reloads with zero retired contexts left alive, and the full 232-action editor regression. All 33 Linux runtime and 28 authoring-only suites pass. Three inspected Avalonia visual-content images show readable controls; Windows was locked, so they are explicitly not desktop screenshots. Compilation remains external and profiles are not persisted. The 0.0.37 custom component checkpoint extends this initial gameplay SDK. [Workflow](EDITOR_GAMEPLAY.md) and [evidence](evidence/m2-editor-gameplay.json).

- **0.0.35 native compiled C# gameplay:** a build-time generator binds the existing `Game<TState>` source into a Native AOT shared library. The engine verifies its inventory, target and generated schema, then uses the existing state/service/rollback contracts. Project/game manifest v2 packages the native artifact; v1 remains unchanged. Ten real-module groups and 12 exact CoreCLR/native comparison checkpoints pass on each OS. A separate Linux build with CoreCLR disabled passes the native suite. A relocated Windows bundle reproduces a 480-tick replay's gameplay state and exact pixels with all 38 files unchanged. CoreCLR regression passes 100 reloads per OS. All 33 Linux runtime and 28 authoring-only CTest suites pass. The final packaged editor passes 232 actions with an inspected screenshot. One earlier GUI run lost input capture while other Windows tests were active; an unchanged isolated rerun passed, and the cause remains an inference. Native library replacement requires a process restart. Clean-machine deployment, Linux graphics and consoles remain unqualified. [Contract](NATIVE_GAMEPLAY.md) and [evidence](evidence/m2-native-gameplay.json).

- **0.0.34 unlocked visual follow-up:** the AnimationRig editor fixture passed 101 semantic actions on NVIDIA, with an inspected full-window screenshot at 125% scale. Controls were readable in the captured layout. This closes the previously unavailable capture for that fixture; physical input and general DPI/accessibility coverage remain unqualified. [Visual evidence](evidence/m2-managed-animation-visual.json) and [screenshot](evidence/m2-managed-animation-visual.png).

- **0.0.34 C# animation API:** typed `GetAnimation` and `SetAnimation` expose the native playback/transition authority to handwritten gameplay. Commands stage until the callback returns; caller/gameplay conflicts and duplicate targets reject the whole batch. Native animation state survives compatible code reload, while failed ticks restore it together with gameplay fields, sounds and physics. That checkpoint introduced ABI v3; current builds require services ABI 7 with the save/component/lifecycle extensions and a matching rebuilt managed bridge. Twelve integration groups and 13 independent ABI checks passed on each of Linux and Windows, including the checked-in sample, interrupted-fade reload and rollback of animation/physics/sound/gameplay state. The existing gameplay suite passed 100 reloads per OS; both Windows GPUs passed a 480-tick C# door replay with exact reference-image agreement. All 32 Linux runtime CTest suites and three selected authoring-only compatibility checks passed. The final packaged editor passed 101 scripted actions on NVIDIA; its full-window screenshot remained unavailable under the locked Windows session. [C# contract](MANAGED_GAMEPLAY.md#control-animation-from-c) and [recorded evidence](evidence/m2-managed-animation.json).

- **0.0.33 animation crossfades:** optional `blend_ticks` blends advancing outgoing and destination clips over 0–3600 fixed ticks, including transitions to and from the authored rest pose. Missing channels use the authored baseline; rotation interpolation follows the shortest quaternion arc. Interruptions retain one immutable local-pose buffer rather than a recursive blend tree. Active transition clocks/weights are inspectable, completion removes the transition, and invalid source/target poses roll back the batch with physics and gameplay state. The typed AnimationRig Inspector exposes named authored clips and guarded live controls that advance a paused runtime by one tick. Native tests and seven protocol groups passed on Linux and Windows; 48 captures across two Windows GPUs produced 24 exact-pixel comparisons against independently authored reference poses. The final packaged desktop passed 101 semantic actions per GPU. A locked Windows desktop prevented a fresh full-window screenshot, so those semantic tests do not establish visual UI qualification. No crowd-throughput or full character-system claim is made. [Animation contract](RUNTIME_ANIMATION.md) and [recorded evidence](evidence/m2-animation-blending.json).

- **0.0.32 static mesh collision:** explicit `MeshCollider` references an imported, unweighted model primitive independently of its rendered mesh. Indexed triangle acceleration preserves geometric gaps, and raycasts report original triangle ordinals. Static hierarchies and positive nonuniform scale are supported; incompatible bodies, animation-owned transforms, shear and invalid geometry reject. The Inspector attaches/removes collision in one guarded undoable edit and exposes typed primitive/material fields while retaining invalid or conflicted drafts. Windows native mesh tests, nine public-protocol groups and runtime/animation regressions (7/11 groups) passed; both laptop GPUs passed 76 semantic GUI actions with doorway ray queries and Scene/Game captures. These tests use synthetic fixtures. Collision-only bundle export and all 11 project/export regression groups also passed with a fresh runtime installation. [Collision contract](MESH_COLLISION.md) and [recorded evidence](evidence/m2-mesh-collision.json).

- **0.0.31 independent Scene and Game panels:** two native HWND/renderers share one authoring session and fixed-tick simulation. Cameras, capture targets, renderer errors and detach/reattach lifetimes remain pane-specific; drawing never advances the clock. Modified Tall is the default workspace, with a vertically split Scene/Game variant. Version 2 layout preferences retain all six panels and migrate existing version 1 arrangements. Native tests pass three headless groups and seven groups for each GPU at 1×/4× MSAA; isolated sky/player regression passes 88 captures. Layout storage passes 28 cases on each OS. The final desktop passes 97 scripted actions and a three-action fresh-process restart on each GPU. [Desktop guide](DESKTOP_EDITOR.md) and [independent-viewport evidence](evidence/m2-desktop-dual-view.json).

- [Editor Game input](DESKTOP_EDITOR.md): keyboard/mouse bindings control runtime characters from Game view through the same evaluator as the player. Native atomic event batches have session-scoped retry receipts, focus/lifecycle clearing, frozen profiles and inspectable tick delivery. The 0.0.29 checkpoint passed 227 GUI actions on each laptop GPU plus native replay/state checks. Physical raw mouse acquisition, cursor restoration and input feel remain unqualified by these synthetic tests. [Game-input evidence](evidence/m2-desktop-game-input.json).

- [C#/Avalonia desktop editor](DESKTOP_EDITOR.md) over a native C ABI and the shared world service: Vulkan GUI composition, directly hosted native Vulkan Scene and Game panels, floating docks, typed Transform/Camera/mesh/material/light/environment inspectors, folder browser, hierarchy stripes and component icons. This is the new human-editor prototype, not a production-complete editor. Scene fly/orbit/pan/frame controls, CPU geometry picking, guarded transform gizmos and saved dock layouts are implemented. Native real-time Play/Pause/Resume/Step and independent Scene/Game cameras share agent-accessible commands. Game view connects keyboard/mouse gameplay through native bindings, atomic retryable event batches and explicit focus; assigned gamepads and opt-in [audio output](EDITOR_AUDIO.md) share the owner lifecycle. Physical input and multi-display qualification remain distinct from HWND message tests. Headless engine builds retain no GUI dependency. [Desktop foundation evidence](evidence/m2-desktop-editor.json) and [navigation/layout evidence](evidence/m2-desktop-navigation.json), [gizmo evidence](evidence/m2-desktop-gizmos.json) and [playback evidence](evidence/m2-desktop-playback.json).

- [Shared local authoring sessions](SHARED_SESSIONS.md): `serve`/`connect`, Windows same-user named pipes and Linux Unix sockets, multiple clients over one writer/revision/history, and live native-editor attachment. Inspector drafts retain their original revision across external edits; stale Apply is rejected. Fresh viewport captures report authored/runtime revision and tick separately. This is local authoring transport, not multiplayer or a finished MCP integration.

- Shared native `WorldSession` API, immutable external-camera snapshots and bounded session-local undo/redo, with revision guards, durable retry receipts and identity-preserving restoration. The retained desktop editor and earlier [ImGui frontend](EDITOR.md) use these same operations. Package management, production accessibility and large-project responsiveness remain unfinished.

- Native keyboard/mouse and gamepad profiles with alternate bindings, sensitivity/inversion, radial stick deadzones/response, trigger hysteresis, atomic profile edits, persistent retry receipts and isolated event evaluation. SDL gamepad discovery, single-player device assignment and attachment/focus/disconnect handling feed the same evaluator as headless traces; semantic replay remains independent of bindings. General action maps, in-game rebinding UI, haptics and general settings menus remain unfinished. [Input profile contract](INPUT_PROFILES.md), [gamepad contract](GAMEPADS.md).

- glTF skin/curve packages, paginated key/joint/pose inspection and CPU reference deformation with isolated Vulkan pose capture. A [compute skinning pass](GPU_SKINNING.md) feeds the shared material and shadow renderer. Editable rig/node bindings connect compiled curve sampling to fixed-tick runtime playback, independent instance clocks, atomic commands, bounded crossfades, pose/physics rollback and immutable live palettes. Native masked layers add independent clocks, transitions and persistent weights. Compiled C# layer controls, retargeting, IK and root motion remain unfinished. [Runtime animation contract](RUNTIME_ANIMATION.md), [animation asset contract](ANIMATION_ASSETS.md).

- Native mono WAV import and content-addressed audio packages, editable acoustic materials/emitters, and optional Steam Audio direct-path/HRTF capture from authored or live poses. Linux/Windows tests measure actual PCM, dynamic-door obstruction, delayed arrival, directional cues and mixing; the C# use action changes the observed acoustic path. Native logical voices, rollback-safe C# play/stop, persistent direct/HRTF streams, temporal replay recording and optional SDL3 player output and a separate bounded [editor DSP worker](EDITOR_AUDIO.md) are now implemented; reflections, production scheduling and the full environmental system remain unfinished. [Event/output contract](AUDIO_EVENTS.md). [Native audio contract and evidence](AUDIO.md).

- Optional C# gameplay integrated with the native runtime: one `Game<TState>`, typed native-owned fields, compact agent inspection/editing, same-tick use/raycast/kinematic interaction, compatible-field reload and joint gameplay/physics batch rollback. Linux and Windows pass 100 reloads plus real failure and retained-context checks; both laptop GPUs pass the scripted door replay. Full component/event APIs and production deployment qualification remain unfinished. [C# API and evidence](MANAGED_GAMEPLAY.md).

- Native collider raycasts with stable entity hits and ignore filters, timed kinematic targets that persist across input batches, live progress inspection and replay. Door collision, child poses, dynamic-body pushing and real failure rollback are checked; The optional C# module now uses this surface for scripted use-button behavior. [Physics interactions and evidence](PHYSICS_INTERACTIONS.md).

- Conservative object frustum culling for the camera and each shadow view, draw counters and optional CPU/64-bit GPU timestamps on capture/play. Exact pixel comparisons retain offscreen casters and moving runtime shadows on both laptop GPUs. Timings describe these small fixtures, not qualified game performance. [Visibility and render diagnostics](RENDER_DIAGNOSTICS.md).

- Optional cascaded directional, six-face point and spot shadow maps, bounded resolution/storage, receiver-plane filtering and moving runtime casters. GPU occlusion and continuous shadowed replay are checked against independent observations. Caching, occlusion culling, ray-traced/virtual shadows and performance qualification remain outstanding. [Shadow contract and evidence](SHADOWS.md).

- Default procedural sky in new projects: editable linear colors, horizon falloff and directional Sun disk, shared by captures, player and desktop. Inspector settings use the native schema and guarded transactions; existing worlds retain their background. This is an artistic background, without atmospheric scattering, IBL, clouds or day/night simulation. [Sky contract](LIGHTING.md#procedural-sky) and [0.0.30 evidence](evidence/m2-procedural-sky.json): 44 captures and 59 GUI actions per laptop GPU.

- Authored directional/point/spot lights, ambient fill and exposure, resolved headless inspection, and frozen runtime settings with moving hierarchical light poses. The development renderer supports 1,024 enabled lights with conservative clustered assignment and complete all-light fallback on overflow. Seventeen clustered/reference capture pairs match exactly on each laptop GPU; HDR, lighting, profiler, player and native viewport regressions pass. Eight affected Linux headless contract suites also pass. GI and representative game-scale performance remain unfinished. [Clustered-light evidence](evidence/m2-clustered-lighting.json). [Lighting contract and evidence](LIGHTING.md).

- Static glTF/GLB import and content-addressed cooked models, editable hierarchy instances, StaticMesh/PbrMaterial components, indexed geometry and a direct-light GGX path with PNG/JPEG base-color, metallic/roughness, emissive and occlusion textures, cooked mip chains and glTF samplers. Normal maps use authored or MikkTSpace-generated tangents. Independent image imports, PbrTextures overrides and effective material inspection are available; broader glTF features remain excluded explicitly. [Asset contract](ASSETS.md), [material authoring and evidence](MATERIAL_AUTHORING.md), [texture evidence](evidence/m2-textures.json) and [captured material example](evidence/m2-textured-grid.png).

- Continuous native player: fixed-step human input, deterministic visual replay, a persistent Vulkan context, resize/minimize handling, retry receipts and final-state capture. Actual Windows replay/lifecycle checks pass; the automated desktop could not grant foreground input, so physical controls remain unqualified. [Player contract](PLAYER.md).

- Optional EnTT/Jolt fixed-step runtime: frozen authored revisions, stable identities, static/dynamic/kinematic box collision, static triangle mesh collision, capsule locomotion/jumping/look, guarded input batches, live inspection/capture and internal failure rollback. [Runtime contract](RUNTIME.md).

- Initial M2 observation path: Vulkan depth-tested box rendering from an immutable authored snapshot, camera/hierarchy evaluation, preview or authored lighting, 1×/4× MSAA and revision-tagged BMP capture. Actual image checks pass on NVIDIA and AMD. [Scene capture](SCENE_CAPTURE.md).

- Persistent authored-world service: stable entity IDs, hierarchy queries, Transform/Camera/MeshRenderer inspection/editing, previewable atomic transactions, revision guards, persisted retry receipts and prior-snapshot recovery. [Protocol and limitations](WORLD_SERVICE.md).

- C++20 core library and native CLI, with bounded CMake/Ninja build presets.
- Machine-readable command discovery, request schemas, version information and errors.
- Native host inspection, plus optional Vulkan loader/device enumeration using pinned headers.
- A separate headless build with no downloaded dependencies or graphics requirement.
- Black-box CLI contract tests and local installation with license notices.
- Optional SDL3/NVRHI Vulkan window, bounded presentation, explicit GPU selection and final-frame BMP readback.
- DXC compilation of embedded HLSL vertex/pixel shaders to SPIR-V; no runtime shader-file dependency.
- Pinned workspace-local Linux-host LLVM-MinGW toolchain producing a native Windows executable.
- Isolated native-module lab: host-owned state, a versioned C ABI, staged replacement/migration, canonical checkpointing and machine-readable interactive commands. This is an extension/iteration experiment; the production `hot_reload` capability remains false.
- Native Windows launcher hosting CoreCLR, with collectible C# game assemblies, native entity buffers, staged updates/migration, checkpoint parity and unload/allocation diagnostics. This remains a separate development experiment, not the production C# gameplay SDK.
- Linux x64 CoreCLR/Native AOT shipping experiment using the same C# game, with independently verified full-state equality, cross-runtime checkpoint resume/migration, error handling and runtime/native-allocation diagnostics.

See the [build instructions](BUILD.md). The 0.0.33 Linux configurations pass all 32 optional-runtime and 27 authoring-only CTest suites. [Animation evidence](evidence/m2-animation-blending.json) records this checkpoint. The earlier 0.0.32 Linux qualification has passing results for 26 authoring-only and 31 optional-runtime CTest suites. Initial runs exposed one stale discovery-schema assertion in each configuration; it was updated from revision 23 to 24 and the affected suite rerun successfully. [Collision evidence](evidence/m2-mesh-collision.json) records that checkpoint. Earlier Windows qualification covers the native shared-session test, ten history cases and seven applicable CLI cases; unbuilt-editor/renderer tests are skipped in that configuration. Earlier bootstrap evidence also covers the Linux Vulkan-inspection variant. Three explicit Windows renderer failure tests cover a missing driver, an unavailable selected GPU and an unwritable capture path. These are bounded checks, not the planned game workload or agent-task qualification suite.

## Findings

The inspected host exposes 16 logical CPUs and about 7.4 GiB of RAM to WSL. Its Vulkan loader enumerates llvmpipe (a CPU software device), not the laptop's discrete GPU. Enumeration success therefore does not qualify hardware rendering. The strict hardware check exits with the documented unavailable result.

The cross-compiled Windows x64 executable runs on Windows through WSL interoperability, using Windows Vulkan drivers. Both the RTX 4070 Laptop GPU and AMD Radeon integrated GPU presented 12 frames and produced a verified 960×540 BMP. The automated image check observes approximately 24.7% triangle coverage and all three vertex colors. The NVIDIA image was also visually inspected. No NVRHI errors were reported. This is one laptop with two GPU vendors, not broad hardware qualification.

The bootstrap used a serialized FIFO loop and NVRHI validation. Later development adds bounded submission/retirement and qualification with the Khronos validation layer, including synchronization validation; see the current [render diagnostics](RENDER_DIAGNOSTICS.md). Resize and minimize/restore are supported. Device-loss recovery, representative game performance and broad hardware qualification remain unfinished. The bootstrap triangle and box captures do not establish those properties. A Windows-hosted MSVC engine build also remains unverified.

The [Windows rendering evidence](evidence/m0-windows-render.json) records binary/source hashes, GPU reports, capture checks and test output; [the captured frame](evidence/m0-windows-triangle.png) is available for inspection. The [earlier WSL evidence](evidence/m0-wsl-bootstrap.json) remains a historical record of the 0.0.1 bootstrap.

The [native-module lab](NATIVE_MODULE_LAB.md) preserves 1,000 entities across behavior changes and 100 reloads on Linux and native Windows. It verifies rejected ABI/migration replacements, failed compilation while old code keeps running, rollback of a partially written update, canonical checkpoint round trips, corruption rejection and a 32→40-byte entity layout migration. The [baseline evidence](evidence/m0-native-module.json) records 40 actual source-edit timings per platform and resident-memory samples. These are small C++ fixture results, not C# results or full-game iteration qualification; jobs, callbacks and GPU/audio resource retirement remain untested.

The [managed gameplay lab](MANAGED_GAMEPLAY_LAB.md) tests C# development inside a native C++ Windows host using SDK 10.0.204 and CoreCLR 10.0.11. Its 40 real source edits reached the first verified logical update at p95 **1.876 seconds**, including build, reload and protocol/interop overhead; diagnostic GC was measured separately. All 100 repeated reloads collected their retired contexts. An intentionally retained game object was detected, then successfully released. Native buffers reached zero bytes on shutdown. Checkpoint bytes match the C++ Windows/Linux fixture. [Recorded C# evidence](evidence/m0-managed-gameplay.json)

These results describe one small 1,000-entity development fixture, not a game performance target or a complete C# SDK. The measured hot loop allocated zero managed bytes, but staging/snapshots/JSON transport allocate outside that interval. The separate [Linux shipping experiment](MANAGED_SHIPPING_LAB.md) now passes full-state CoreCLR/Native AOT equality, save/resume and schema migration, malformed-input rejection and twenty process restarts per runtime. The native executable reports no JIT support. [Shipping evidence](evidence/m0-managed-shipping.json)

The integrated 0.0.35 native gameplay route qualifies bounded Linux/Windows fixtures and a relocated Windows Vulkan replay. Complex jobs, general event lifetimes, clean-machine production deployment and console C# support remain unqualified. Native AOT game libraries remain process-lived; see the [native gameplay contract](NATIVE_GAMEPLAY.md).

## Qualification boundaries

| Area | Current support and limits |
| --- | --- |
| Build/resource inventory | Linux and cross-compiled Windows executables tested. Workspace tools pinned. Representative whole-engine build/reload/resource measurements and Windows-hosted engine compilation are not qualified. |
| Vulkan rendering | Presentation and readback passed on NVIDIA/AMD Windows GPUs. Full API validation, device-loss recovery and representative game workloads are not qualified. |
| Gameplay-language iteration | C++ and C# CoreCLR Windows/Linux reload fixtures tested; Linux CoreCLR/Native AOT lab parity passes. C# runtime integration also passes on both OSes. Bounded Native AOT modules are tested on Windows and Linux; production workloads, clean-machine distribution and console C# deployment remain unqualified. |
| Shader compilation | Pinned DXC compiles embedded HLSL to SPIR-V. Representative shader workloads and incremental compilation performance are not qualified. |
| Upscaling/frame generation | Optional experimental FSR 3.1.4 Native AA and fixed-ratio reconstruction; quality/performance qualification remains open. No DLSS or frame generation. |
| Acoustics | Native direct-path/HRTF and dynamic-door fixtures pass, including C# use. Optional player output exists; reflections/pathing and production resource/listening qualification are incomplete. |

The initial world suite contains nine cases, with one Windows-interop skip: abrupt Windows process termination remains unqualified by that WSL harness. See [initial world evidence](evidence/m1-world-service.json) and [expanded scene evidence](evidence/m2-scene-capture.json). The authored document is a bounded JSON model. The separate EnTT/Jolt runtime provides fixed ticks and live render snapshots; custom scalar components and exact-schema persistent simulation saves are supported, while general migrations and broader content/shader reload remain unfinished. [Native project creation/export](PROJECTS.md) is implemented; bounded Native AOT gameplay deployment is supported, while general incremental packaging remains unfinished. Integrated C# supports compatible-field reload. Game UI and production audio scheduling remain incomplete.
