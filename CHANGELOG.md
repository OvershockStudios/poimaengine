# Changelog

Version numbers identify source checkpoints. Feature qualification and packaged editor availability are recorded separately in [Implementation status](docs/IMPLEMENTATION_STATUS.md). During prototype development, APIs and file formats can change between checkpoints.

## 0.0.174

- Extend glTF import with the existing narrow collapsed-UV tangent fallback for geometry without normal maps, preserving valid frames and reporting repaired corners. [Asset profile](docs/ASSETS.md).
- Support 16,384 animation channels across a model's takes while retaining the independent key, joint and package limits. Add exact-boundary, malformed-package and retargeting-budget checks. [Animation profile](docs/ANIMATION_ASSETS.md).

## 0.0.173

- Add bounded Windows authored-world publication retries with exact primary/prepared-byte guards. Preserve public errors, once-only commit ordering and shared storage callers; expose actual retry facts on stderr. [Policy and recovery](docs/WORLD_STORAGE_DIAGNOSTICS.md).
- Pass six controlled service-level retry/conflict cases and twenty fresh runs of the original rapid-write test, including 24 recovered renames. Retain earlier failed checkpoints as separate evidence. [Results and scope](docs/evidence/m2-world-storage-retries.json).

## 0.0.172

- Add Windows world-storage stderr diagnostics with the failed native operation, publication stage and immediately captured error code. Preserve existing JSON-RPC errors, exception messages and commit ordering. [Storage and recovery](docs/WORLD_STORAGE_DIAGNOSTICS.md).
- Verify controlled primary/backup publication failures, unchanged state on rejection, identical-request recovery, receipt replay and cold reopening. Capture a failure in the original write-stress workload at backup publication with Windows error 5; keep the failed regression and the separate successful protocol cohort distinct. [Results and scope](docs/evidence/m2-world-storage-diagnostics.json).

## 0.0.171

- Restore focused mutation schemas through standard nested conditionals, preserving complete selected branches, transaction guards and reserved runtime-contract metadata. Keep conservative handling for references, unfamiliar schemas and branch-level ambiguity. [Discovery guide](docs/WORLD_SERVICE.md#focused-discovery-development).
- Qualify native Windows/Linux discovery and CLI/MCP comparisons: fourteen smaller replies and two restored AnimationRig selections per platform. Pass the corrected sixteen-test Linux cohort with unchanged engine bytes; preserve earlier failed runs and the separate open Windows snapshot-publication issue. [Results and scope](docs/evidence/m2-conditional-discovery.json).

## 0.0.170

- Add opt-in FBX material errors with original source-node/local-slot witnesses, feature labels, bounded names and animation-donor attribution. Preserve existing acceptance, omitted/false errors and successful cooked content. [Guide](docs/FBX_IMPORT.md#locate-an-unsupported-material).
- Qualify Windows native checks and separate Linux diagnostic/old-binary parity, including all seven reason families and seven complete cooked-file byte pairs per comparison. Retain the broader Linux gate's 19/21 result and two discovery assertion failures for a separate follow-up. [Results](docs/evidence/m2-fbx-material-profile.json).

## 0.0.169

- Qualify the full 2,394-entity CoreCLR workflow through twelve save/load cycles, six compatible code reloads, sixteen live scalar/PNG adoptions and fresh-process continuation. Verify over 31 minutes of unpaused play, 76 complete snapshots, 31 exact whole-file/state pairs and 22 complete edit forecasts. [Workflow](docs/PERFORMANCE_YARD_MIXED_RELIABILITY.md).
- Independently join 3,927 original native calls, clean owned process closure and 2,018 OS memory queries and sixty fixed analysis windows. Preserve the separate rehearsal and earlier failures; memory-growth causes, matched recording overhead and the wider Alpha gates remain open. [Results](docs/evidence/m2-mixed-coreclr-reliability.json).

## 0.0.168

- Qualify CoreCLR compilation, failed-build recovery, compatible reload and complete warm/cold saves in the full 2,394-entity Performance Yard. Verify 391 native calls, eight complete checkpoints and five exact saved-file/state pairs. [Workflow](docs/PERFORMANCE_YARD_CORECLR.md).
- Integrate callback counters and a 41-query process-memory timeline through five fixed play intervals. Bound cadence with original request/response envelopes and preserve earlier startup, observer and verifier failures. The longer reliability and Alpha gates remain open. [Results](docs/evidence/m2-coreclr-performance-yard.json).

## 0.0.167

- Add opt-in CoreCLR callback allocation and GC observations with fixed counters, strict bridge negotiation and read-only inspection. Keep diagnostic attempts outside authoritative state, saves and rollback; preserve default loads and the gameplay C ABI. [Guide](docs/MANAGED_CALLBACK_METRICS.md).
- Pass 171 managed helper checks, six Windows native suites and five Linux headless CTests. Independently verify 357 native calls, twenty complete checkpoints and eight exact state pairs, including callback failures, reload and older backends. Preserve failed collectors and full-scene prerequisites without qualifying the wider reliability gate. [Results and scope](docs/evidence/m2-managed-callback-metrics.json).

## 0.0.166

- Add background elapsed-time native play for agents, with a nonfocusable window, no hardware input routed to gameplay, explicit pause/replacement fences and continued simulation while minimized. Preserve existing interactive/replay defaults and foreground measurement eligibility. [Workflow](docs/LIVE_PLAYER.md#background-play-for-agents).
- Pass thirteen Windows CPU executions, 161 background protocol calls and thirteen Linux CPU tests. Verify guarded renderer capture, compiled controls, full save restoration and a separate full Performance Yard run with fresh-process continuation, seven exact saved-file pairs and 29 actual memory queries. Preserve earlier source-preparation and reader failures. [Results and limits](docs/evidence/m2-background-player.json).

## 0.0.165

- Inspect original skin joints, inverse binds and animation-channel metadata through paged source inspection before cooking. Preserve original fingerprints, legacy response shapes and cooked package bytes. [Retarget workflow](docs/ANIMATION_RETARGETING.md#inspect-the-original-sources).
- Add opt-in development-job process observations and Python helper support: retain raw Windows/POSIX termination and cleanup requests while preserving default status, exit projection and retry behavior. [Process semantics](docs/DEVELOPMENT_JOBS.md#process-and-cleanup-observations).
- Pass eight Windows native executions, four new topology groups, legacy retarget checks and three real C# compiler cases. Match 289 complete legacy response pairs and four cooked packages against 0.0.164. All twelve Linux CTests pass; retain the enclosing collector failure caused by oversized negative-test output and separately audit the records. [Results and limits](docs/evidence/m2-source-topology-process-inspection.json).

## 0.0.164

- Add optional CoreCLR resources to the installed authoring pack, with one exact SDK shared by the bridge, game reference and native binding generator. Preserve default v1 packs, native profile fingerprints, guarded publication and complete-inventory validation. [Installed authoring](docs/AUTHORING_TOOLS.md).
- Qualify actual installed compilation, structured-error recovery, compatible reload and full warm/cold saves with the engine checkout pathname unavailable: 697 native calls, seventeen checkpoints and seventeen naturally closed processes. A separate v2 NativeAOT publish/adopt/export/relocated-game route passes 865 calls and eighteen checkpoints. Pass six Windows native suites, eleven Linux headless tests and portable pack checks on both systems; preserve failed preflights and the reader correction. [Results and scope](docs/evidence/m2-installed-coreclr-authoring.json).

## 0.0.163

- Reconcile deferred remote selection after Inspector or live-component drafts resolve. Preserve original-target Apply guards and failed drafts, keep accepted local selections through stale polls, safely clear deleted targets, and block Scene manipulation when selection reads fail. Automatic errors retain a visible Reload recovery path without recursive retries.
- Pass 34 production-linked model scenarios and 302 assertions on Linux and Windows, compile the full Windows editor, and pass four actual managed-editor/native-bridge cases with full authored-state and history checks. Displayed and physical interaction remain separate. [Behavior](docs/DESKTOP_EDITOR.md#current-authoring-controls), [results and scope](docs/evidence/m2-editor-selection-reconciliation.json).

## 0.0.162

- Package the binary C# SDK, component analyzer, binding generator and NativeAOT publisher as relocatable [authoring tools](docs/AUTHORING_TOOLS.md). Native build profiles pin and revalidate the complete pack while preserving existing checkout workflows and retries.
- Qualify an external component game through actual compilation, native callbacks, full save continuation and relocated export with the original checkout path unavailable: 894 calls, eighteen checkpoints and 32 naturally closed processes. Pass Windows/Linux policy checks; preserve earlier failed attempts and unchanged production binaries through the fixture correction. [Results and limits](docs/evidence/m2-installed-authoring-tools.json).

## 0.0.161

- Import supported separate FBX roughness and metalness maps into linear packed channels with independent scalar factors, shared-image reuse and bounded transient/packed image storage. [Supported profile](docs/FBX_IMPORT.md#roughness-and-metallic-textures).
- Pass seven Windows native executions, eighteen Windows protocol tests and fourteen Linux headless CTest entries. Verify exact packed data, rejection without partial edits, repaired imports and cooked reopening with sources absent; preserve 26 older accepted cases across 12 byte-identical packages and all 58 decoded errors. [Results and scope](docs/evidence/m2-fbx-scalar-maps.json).

## 0.0.160

- Add opt-in process-memory query intervals and bounded reliability-harness phase recording in one shared integer clock. Preserve default sampling and native checks; retain partial errors and leave boundary-crossing queries unassigned. [Recording guide](docs/MEMORY_TIMELINE.md).
- Verify typed clocks, identities, limits, legacy behavior and failure handling with portable checks; pass seven actual Windows self-process queries with five assigned intervals and two deliberately crossed boundaries. Game-memory growth and integrated recording overhead remain separate qualification work. [Results](docs/evidence/m2-memory-timeline.json).

## 0.0.159

- Qualify twenty live PNG texture edits at **0.909 s p95**, including native decoding, mip cooking, adoption and fresh rendered verification on the full Performance Yard. Report verifier preparation separately; the preparation-through-verification p95 is 1.426 s.
- Preserve complete game state, original scalar materials and inherited maps through every adoption. Verify missing-package failure, exact state preservation and successful retry after restoration. [Results and scope](docs/evidence/m2-live-texture-iteration.json).

## 0.0.158

- Reserve a bounded full-frame buffer for exclusive BMP captures, avoiding SDL's repeated small reallocations while preserving the serializer, exclusive creation and durable writes.
- Qualify twenty live scalar-material edits in the full Performance Yard at **0.660 s p95**, with exact rendered references and complete saved-state preservation. Pass seven Windows native executions and three cooperative Vulkan window groups. Keep the earlier missed timing budget and report verifier preparation separately. Texture timing and broader Alpha gates remain open. [Results and scope](docs/evidence/m2-live-material-iteration.json).

## 0.0.157

- Qualify twenty real compatible C# edits through the first changed gameplay tick at **2.206 s p95**, meeting the selected three-second warm-iteration target. Preserve all 50 gameplay values and complete current native save state through every reload.
- Retain a separate six-edit guard pilot, actual compiler/image joins, capture checks and natural process closure. The measured native engine is 0.0.156 with retained 0.0.88 managed tooling; broader Alpha gates remain open. [Results and scope](docs/evidence/m2-relay-yard-iteration-156.json).

## 0.0.156

- Let shared CLI requests interrupt voluntary idle-player waits while retaining paced background presentation, owner maintenance and capture invalidation. Keep standalone and desktop polling on their existing paths.
- Pass seven Windows native executions, three real cooperative-window groups and eight Linux headless tests. Compare matched paused shared-host requests with exact saved-state checks; report higher quiet CPU separately. [Results and limits](docs/evidence/m2-cooperative-player-poll.json).

## 0.0.155

- Identify contained external FBX texture-read failures through structured import and source-inspection diagnostics: relative dependency, failure reason, actual size when available, effective byte limit and animation-donor index. Preserve existing path confinement, image budgets and decoding rules. [FBX diagnostics](docs/FBX_IMPORT.md#repair-an-external-texture-dependency).
- Cover missing, empty and oversized dependencies, unchanged authoring/publication on rejection, repair with the original fixture PNG and source-independent cooked-model reopen. Keep unsupported materials, unsafe paths and decode failures under their existing admission rules.

## 0.0.154

- Add opt-in GPU-memory recording to the Performance Yard reliability harness using existing player reports, without extra native requests or changes to gameplay, focus, frame-time and save checks. [Recording guide](docs/PERFORMANCE_YARD_RELIABILITY.md#optional-gpu-memory-recording).
- Retain bounded per-owner/device observations, cache consistency checks, unavailable states, session transitions and terminal reports. Keep diagnostic errors and omissions separate from gameplay results; sampled heap estimates do not qualify peak or resident VRAM. [Checks and scope](docs/evidence/m2-gpu-memory-observations.json).

## 0.0.153

- Qualify a fresh Codex-authored management game through native publication and Windows export, a capacity-four FIFO of real work-unit entities, and retained V1→V2 save upgrades. Publish the original [Beacon Foundry source and world](docs/evidence/fixtures/agent-beacon-foundry/README.md); preserve the earlier failed workflows.
- Independently check complete checkpoint restoration, queue/progress and allocator continuation, warm and cold new-unit completion in source and relocated exports, plus three readable Vulkan captures. The selected three-family agent matrix now has bounded evidence; the broader Alpha gates remain open. [Results and limits](docs/evidence/m2-agent-beacon-foundry.json).

## 0.0.152

- Expose optional Vulkan heap allocation estimates, dynamic budgets, device identity and sample age through native player/capture diagnostics, CLI smoke reports and desktop viewport reports. [GPU memory diagnostics](docs/GPU_MEMORY.md).
- Cache queries on the renderer owner thread with a minimum sampling interval; distinguish unavailable data from measured zero and retain stopped observations. Keep GPU memory telemetry separate from saved state, allocation policy and residency measurements.

## 0.0.151

- Give the generated Windows launcher a native per-user save location keyed by the project identity, with explicit storage overrides and forwarded launch arguments. Keep direct/service launches explicit. [Game saves](docs/GAMEPLAY_SAVES.md).
- Validate the default location before creating its directories and preserve immutable game bundles across play and relocation. Save-image compatibility and explicit upgrade rules remain unchanged.

## 0.0.150

- Expose typed, read-only component and liveness queries through C# `ControlContext`, reusing existing native services. Keep component and lifecycle writes in Tick. [Control reads](docs/CUSTOM_COMPONENTS.md#reads-in-compiled-controls).
- Add a compiled fixture for scalar/array reads, exclusive query pages, same-tick edits, immediate queue refusal, historical receipts and callback rollback. Retain the failed management-game requirement separately. [Results and scope](docs/evidence/m2-control-component-reads.json).

## 0.0.149

- Release [runtime game-session v1](docs/RUNTIME_GAME_SESSION_V1.md), a selected 19-method boundary for runtime state, typed components, spatial queries, compiled logical controls and external saves. Add truthful discovery qualification, frozen request/result manifests, compatibility gates and opt-in packaged Python validation. Keep the existing authoring and prospective workflow contracts separate.
- Qualify unchanged scalar/collection consumers against new source and exported runtimes, three actual discovery contexts, six Windows native suites and 174 portable tests on each OS. Add focused paging, eligibility, recovery-consent and cross-owner retry checks; preserve failed attempts and the supported limits. [Results](docs/evidence/m2-runtime-game-session-v1.json).

## 0.0.148

- Record a fresh Claude-authored repair adventure through native publication, adoption and Windows export; preserve the interrupted earlier trial and all repair attempts. [Original source and task](docs/evidence/fixtures/agent-pumphouse/README.md).
- Independently verify real inventory/mechanism interactions, complete checkpoint restoration before callbacks, cold Welcome Load and New Game retaining saves through source and relocated bundled executables. Join three Vulkan captures to logical UI and retain documented limits. [Evidence](docs/evidence/m2-agent-pumphouse.json).

## 0.0.147

- Require explicit Boolean recovery consent when joining a recovered `save.load` result to its original request in the opt-in Python validator. Preserve response-only checks and historical receipt semantics. [Result contract](docs/WORKFLOW_RESPONSE_CONTRACT.md#request-dependent-results).
- Qualify one genuine retained-game recovery and global-state upgrade through source and actual exported executables, including denied loads, absent-plan retries, an actual pickup and cold continuation. Keep original failed attempts and distinguish exact restoration from checked owner-lifecycle updates after callbacks. [Workflow and evidence](examples/relay-yard/UPGRADING.md#focused-recovery-and-exported-game-consumer).

## 0.0.146

- Finish explicitly rejected stale-generation saves as failed even after durable receipt history rolls over, allowing gameplay to continue. Keep receipt resolution for uncertain publication. [Save behavior](docs/GAMEPLAY_SAVES.md#commit-and-rollback).
- Add compiled missing-load/stale-write checks through the source and actual exported game, including repeated queries, historical retries and subsequent save/restore continuation. Qualify the native repair on Windows and Linux, retain the original defect reproduction and failed preflight. [Results](docs/evidence/m2-save-publication-rejection.json).
- Define Alpha 1 acceptance across three compact game families and both agent clients, with a separate fixed performance workload. [Alpha criteria](docs/ALPHA_ROADMAP.md#qualification-target).

## 0.0.145

- Add the compiled collection fixture and an explicit collection preparation profile, preserving the original scalar workflow. Exercise all five supported capacity-two array element kinds through the actual exported executable. [Workflow](docs/COMPONENT_GAME_WORKFLOW.md#bounded-collection-profile).
- Qualify length changes, typed edits, historical retries, rejected writes, compiled rollback and same-owner/cold restoration over 1,299 native calls. Re-run the original 742-call scalar consumer and preserve the failed pre-compiler publisher attempt. [Evidence and limits](docs/evidence/m2-collection-workflow.json).

## 0.0.144

- Add a reproducible compiled-component workflow: publish the original C# fixture, prepare its typed source project, adopt gameplay and export, then launch the actual bundled executable. [Guide](docs/COMPONENT_GAME_WORKFLOW.md).
- Qualify nonempty scalar components, Tick-driven save/load, cold restoration and four failed-callback rollback checks across source and readonly game services. Validate 715 selected successful results from 742 native calls; preserve failed preparation and evaluation attempts. [Results and limits](docs/evidence/m2-compiled-component-workflow.json).

## 0.0.143

- Add opt-in Python result validation for 21 runtime/component/UI/save methods, with strict wire types, dynamic schema context, compact selectors, exclusive pagination and historical retry/replacement boundaries. Preserve the distinction between canonical runtime schemas and original embedded gameplay declarations. [Contract](docs/WORKFLOW_RESPONSE_CONTRACT.md).
- Qualify the candidate against 65,293 selected recorded successes and 31 new portable consumer tests on Linux and Windows. Four focused native captures cover missing result families, nineteen expected errors and active/stopped save restoration, with natural Windows Job closure. Broader stable API and unobserved variant gates remain open. [Results](docs/evidence/m2-workflow-response-contract.json).

## 0.0.142

- Add a prospective compatibility checker for 21 runtime/component/UI/save request schemas in three discovery scopes. Preserve complete selected schemas, exact metadata and explicit reference context; reject malformed projections cleanly. This does not release a broader stable API. [Contract and scope](docs/WORKFLOW_API_COMPATIBILITY.md).
- Validate integer gameplay tick/revision guards before retry lookup, rejecting float-equal malformed retries while preserving valid historical results. Verify retained Native AOT gameplay and freshly compiled CoreCLR components, including rollback, saves and 100 reloads. [Qualification](docs/evidence/m2-workflow-request-compatibility.json).
- Correct the skinning guide's stale animation, motion-history and frame-execution limits, linking the existing qualified profiles.

## 0.0.141

- Add optional authored shadow-filter selection with per-capture/player overrides and effective diagnostics. Existing worlds retain point filtering when the field is absent; new project starters explicitly use fractional filtering, 1024-pixel maps and lower Sun bias.
- Compare 25 NVIDIA and 10 AMD character captures across forward/deferred paths, with ten exact-pixel comparisons including two retained-engine baselines. Visual inspection finds smoother edges and closer contact in this controlled scene. Forward timing stays within the fixture ceiling; deferred timing remains inconclusive.
- Verify the retained compiled Switchback game with the authored preset through thirteen gameplay groups in its source project and exported readonly service, including complete save comparisons. Preserve two failed evaluation-wrapper attempts. [Results and scope](docs/evidence/m2-shadow-environment-preset.json).
- Verify seven NVIDIA native-presentation captures across three paused player generations and one same-HWND hosted viewport, including filter inheritance/overrides, frozen runtime images and exact authored restoration through explicit transactions.

## 0.0.140

- Retain Python client and process-transport close failures in a read-only `cleanup_error` property so callers can inspect cleanup diagnostics without replacing an active body exception. Successful later closes preserve the last observed failure; process closure remains a separate observation.
- Add thirteen cleanup tests to the existing client contract suite and update the [owned-host cleanup recipe](docs/PYTHON_CLIENT.md#explicitly-close-a-host-you-own) to keep completion data outside exceptions. All 49 client tests pass on Linux and Windows with native 0.0.134; the Windows Job closes naturally without forced cleanup. [Results and limits](docs/evidence/m2-python-cleanup-diagnostics.json).

## 0.0.139

- Record a second fresh agent-authored Switchback workflow, preserving nine naturally closed failed script attempts and every previous workflow verdict. Independently verify thirteen gameplay groups, full checkpoint continuation and a relocated readonly game service.
- Verify separate Ready/Idle/Run Vulkan observations with declared original source paths unavailable. Distinguish native 0.0.134, Python client/source references 0.0.138 and this evidence checkpoint; retain bounded scope and remaining Alpha limits. [Results](docs/evidence/m2-agent-switchback-fresh-repeat.json).

## 0.0.138

- Add explicit Python `shutdown_host()` for authorized shared headless hosts, using the native empty-parameter request. Client close still detaches; host acknowledgement and process exit remain separate observations.
- Verify nineteen client/lifecycle tests on Linux and Windows, including rejected shutdown parameters, standalone scope rejection, usable clients after errors and natural shared-host exit. Add an [owned-host cleanup recipe](docs/PYTHON_CLIENT.md#explicitly-close-a-host-you-own). [Results](docs/evidence/m2-python-host-shutdown.json).

## 0.0.137

- Preserve [another independently authored Switchback implementation](docs/evidence/fixtures/agent-switchback-repeat/README.md) and verify thirteen gameplay groups against its source project and actual relocated readonly game service, with complete save continuation and cold owners. The original fresh workflow remains failed; the repeated-workflow Alpha gate stays open.
- Qualify Ready/Idle/Run Vulkan captures while the original provider case and evaluator project paths are unavailable, then restore both trees byte-for-byte. Preserve the failed relocation-gate attempt and distinguish the native 0.0.134 runtime from source checkpoint identities. [Evidence and limits](docs/evidence/m2-agent-switchback-repeat.json).

## 0.0.136

- Add [retired build storage maintenance](docs/BUILD_STORAGE.md) with report-first inspection, exact reviewed plans and guarded removal of generated compiler objects/PCH. Preserve sources, assets, reports and binaries; require explicit retirement and no live consumers.
- Verify 24 Linux and 21 Windows temporary-filesystem tests, with three Windows symlink-privilege skips covered on Linux. Register the bounded `retired_build_storage` CTest for Python 3.11+. [Results and limits](docs/evidence/m2-build-storage.json).

## 0.0.135

- Rebuild the published fresh Switchback C# and project unchanged with the current SDK and Native AOT publisher. Verify the new module through thirteen independent gameplay groups, 21,990 native RPCs and complete save continuation using native 0.0.134. [Results](docs/evidence/m2-agent-switchback-fresh-rebuild.json).
- Retain the host processor architecture in isolated qualification profiles so `.NET --info` can initialize correctly. Preserve the failed diagnostic and rejected non-clean SDK preflight alongside successful compiler, profile and gameplay receipts.

## 0.0.134

- Explain native-player readiness and pause conditions through cached state, nullable focus/drawable observations and retained terminal snapshots. Read without polling, device queries or playback changes. [Contract](docs/LIVE_PLAYER.md#readiness-and-pause-conditions).
- Keep initialization and control acknowledgements coherent with their readiness snapshots. Retried control commands preserve their original state, including after the player stops; update the stale first-poll status detail after initialization/presentation.
- Verify five Windows native suites, three Linux non-rendering suites and authoring-core identity on both platforms. Qualify the actual compiled UI/save-load workflow and 32 advancing replay observations. [Results](docs/evidence/m2-player-readiness.json).

## 0.0.133

- Retain method, correlation and detached request context on Python `RpcError`, preserving native code/message/data and existing exception formatting. Keep recovery explicit without automatic retries, discovery or parameter rewriting.
- Verify 32 client tests on both Windows and Linux, including actual process/native error recovery. Build the 0.0.133 SDK wheel with exact qualified modules/resources and check isolated native use. [Diagnostics contract and results](docs/evidence/m2-python-rpc-diagnostics.json).

## 0.0.132

- Add [live displayed UI inspection](docs/PLAYER_UI_OBSERVATION.md) with native pixel bounds, ancestor clipping, computed font size and sampled hittability. Keep immutable last-presented frame tags distinct from current runtime state, with exact ownership and optional optimistic Tick/UI guards.
- Share retained UI control storage across the document and presenter. Verify native DPI/clipped targets, immutable old frames and storage lifetime, plus discovery/MCP checks on Windows and non-rendering Linux.
- Qualify actual AMD compiled UI controls/save-load and NVIDIA unattended replay observation: 32 unpaused reads advance with native C# UI updates, reject stale guards and close cleanly. [Results and limits](docs/evidence/m2-player-ui-observation.json).

## 0.0.131

- Add a [fresh agent-authored Switchback implementation](docs/evidence/fixtures/agent-switchback-fresh/README.md) with original C# bytes, selected CC0 content and portable build/adopt/export instructions. Its original supervisor passes after recorded repairs in one provider session; retain the earlier failed workflow verdicts.
- Pass thirteen independent gameplay groups and 21,990 native RPCs with nine complete version-9 checkpoints and five typed-exact state comparisons. Check the actual exported game service with the original project unavailable using the retained Windows 0.0.128 engine. [Evidence](docs/evidence/m2-agent-switchback-fresh.json).
- Support separate pickup/hit cue metadata and actual native save-status UI in the verifier. Qualify source-unavailable Ready/Idle/Run Vulkan observations with native clip/time/body/GPU checks, preserving failed presentation/observer/plan attempts and strict complete snapshot comparisons.

## 0.0.130

- Publish [Switchback Range](docs/evidence/fixtures/agent-switchback/README.md), an agent-authored first-person game combining crouch clearance, camera-based shots, a weighted animated target, independent world/viewmodel ADS, reloads, compiled menus and checkpoints. Preserve the original C# source, licensed cooked assets and portable build/adopt/export instructions.
- Add an [independent native verifier](tests/agent_switchback_oracle.py). Rebuild the public fixture and pass thirteen gameplay groups, complete save restoration and cold-owner continuation using the retained Windows 0.0.128 engine. Preserve failed fresh-agent workflow verdicts separately from the corrected receipt audit and independent game checks. [Results](docs/evidence/m2-agent-switchback.json).
- Run all thirteen groups against the relocated bundle through its actual read-only game service after removing owned build-source paths; verify Vulkan skinning/captures and retain the failed authoring-route relocation check. Add fifteen portable evidence/content regressions.

## 0.0.129

- Include SDL’s bundled yuv2rgb BSD notice and the selected HIDAPI BSD terms in installed runtimes and exported games. Retain the original upstream notice bytes and reject missing or changed pinned notices during configuration.
- Verify actual CMake installation and Windows native game export/inspection with the additional notices. [Results](docs/evidence/m2-sdl-runtime-notices.json).

## 0.0.128

- Add [native project gameplay adoption](docs/PROJECT_GAMEPLAY_ADOPTION.md): verify and copy compiled artifacts, update manifests with exact hash guards, and retain durable receipts for restart recovery and safe historical retries.
- Expose profile-scoped project inspection and adoption through the world service and separate MCP build consent. Preserve the existing gameplay ABI and keep running-module activation separate.
- Support long Windows adoption paths and bounded directory-publication retries with ownership/content checks and no replacement. Verify compiled game/save continuation, relocated export inspection and actual sharing-denial recovery. [Results](docs/evidence/m2-project-gameplay-adoption.json).

## 0.0.127

- Add [Traversal Yard](examples/traversal-yard): a first-person C# game combining steps, crouch clearance, a moving lift, three beacon objectives, camera zoom, an imported animated courier and checkpoints.
- Complete the course through semantic inputs on CoreCLR and Native AOT, preserve full save continuation and compatible development reload, and verify a relocated Windows export with rendered playback and cooked-world Save/Load. [Results](docs/evidence/m2-traversal-yard.json).
- Correct installed runtime declarations to the current 336-byte services prefix and available gameplay features; reject stale metadata with fifteen file-only regressions. Document building the desktop bridge and stripping shipping symbols without changing executable code or loaded data.

## 0.0.126

- Add opt-in native character locomotion for steps, walkable slopes, floor following and supported kinematic platforms. Preserve the existing controller backend when the profile is omitted. [Contract](docs/CHARACTER_LOCOMOTION.md).
- Report resolved movement independently of integration intent; retain obstruction-safe stance, planar root-motion controls, query-body ownership and complete batch rollback.
- Preserve logical motor pose and both velocity states in strict version-9 snapshots, with shape-first restoration and cold continuation checks. Use existing C# character services without a new ABI extension.

## 0.0.125

- Add opt-in crouch/stand with foot-preserving capsule changes, physical clearance before standing, persistent blocked-stand retries and controlled camera height. [Contract](docs/CHARACTER_STANCE.md).
- Expose guarded stance reads and staged C# controls through schema revision 93 and the append-only 336-byte epoch-7 services prefix. Preserve actual posture and capsule shape through version-8 saves and complete batch rollback.
- Pass 25 native suites, 92 compiled ABI checks, CoreCLR/Native AOT gameplay and 40 AMD/NVIDIA captures with 20 pixel-identical authored-camera comparisons. [Results](docs/evidence/m2-character-stance.json).

## 0.0.124

- Add opt-in native FOV presentation between committed ticks, with independent world/viewmodel lenses, precise reset boundaries and exact frozen rendering receipts. [Contract](docs/PLAYER_LENS_PRESENTATION.md).
- Add atomic `player.capture_current` for unpaused observation, preserving ownership/revision guards, immutable results and retries; retain required exact-tick `player.capture`.
- Pass 24 native CPU suites and qualify 48 AMD/NVIDIA forward/deferred images with 24 exact reference pairs, including eight unpaused fractional captures. [Results](docs/evidence/m2-player-lens-presentation.json).

## 0.0.123

- Add stateless C# `CameraFovTransition` for saved, fixed-tick ADS easing, exact endpoints and position-continuous interrupted transitions. [Contract](docs/CAMERA_LENSES.md#fixed-tick-ads-easing).
- Allow guarded camera-lens reads while the interactive player runs; retain pause requirements for direct runtime mutation.
- Pass 22 native suites and 88 compiled SDK/ABI checks; qualify rollback, mid-transition reload, fresh saved continuation and 32 exact AMD/NVIDIA interactive/reference captures. [Results](docs/evidence/m2-camera-zoom.json).

## 0.0.122

- Add persistent world/viewmodel lens controls for gameplay zoom, with atomic tick staging, rollback and camera-instance lifecycle handling. [Contract](docs/CAMERA_LENSES.md).
- Expose guarded agent reads and independent C# `camera_lens_v1` through schema revision 91 and the append-only 320-byte epoch-7 services prefix.
- Compose player FOV preferences with gameplay magnification; identify effective lenses in renderer receipts and preserve them through version-7 camera snapshots and explicit save upgrades.
- Pass 22 Windows native suites, 83 compiled ABI checks, CoreCLR/Native AOT gameplay and 16 exact AMD/NVIDIA reference captures. [Results](docs/evidence/m2-camera-lenses.json).

## 0.0.121

- Add independent foreground MSAA with explicit arithmetic HDR/coverage resolve before tone mapping, preserving default one-sample output.
- Expose strict `viewmodel_samples` capture/player options and native CLI controls through schema revision 90; validate device support, resource ownership and budgets. [Contract](docs/VIEWMODELS.md).
- Pass nineteen Windows native suites, four API groups, 349 valid render graphs and 632 rejected mutations.
- Qualify 54 AMD/NVIDIA captures across forward, deferred and FSR Quality, including quarter-coverage edges, empty foregrounds and 24 byte-identical previous-version images. [Results](docs/evidence/m2-viewmodel-msaa.json).

## 0.0.120

- Add optional Camera viewmodel lenses and explicit CameraDraw bindings for first-person arms/weapons and camera-hidden bodies, with atomic graph validation and instance remapping.
- Render foreground with separate FOV, self-depth and clustered lighting; composite after world resolve/reconstruction in linear HDR before tone mapping. Preserve world-only products and body shadows.
- Retain frozen lens, membership and resource identities across animation, saves and live material/texture edits; include exact camera-draw descriptors in rendering receipts. [Contract](docs/VIEWMODELS.md).
- Pass nineteen Windows native suites, four API groups and 130 GPU captures covering both adapters, forward/deferred, explicit world MSAA/FSR recipes and paused live-player adoption. [Results](docs/evidence/m2-first-person-viewmodels.json).

## 0.0.119

- Add frozen planar root-motion bindings with Extract diagnostics or Character locomotion through native collision, gravity and jump handling.
- Unwrap loop intervals, blend transition velocities and strip composed planar movement before IK; preserve mounted pivots, instance remapping and complete rollback.
- Expose committed observations and staged C# enable controls through independent `animation_root_motion_v1`, appending the 304-byte epoch-7 services prefix. Root-configured worlds use nested animation-state v5. [Contract](docs/ROOT_MOTION.md).
- Pass sixteen Windows native suites, ten root-motion groups, three API groups, 80 compiled ABI checks, actual CoreCLR/Native AOT consumers and four exact GPU reference comparisons. Record the durable contact-cache limitation separately from accepted reconstructed-state continuation. [Results](docs/evidence/m2-root-motion.json).

## 0.0.118

- Add frozen native two-bone IK chains with world/rig targets, pole controls, weights and deterministic reach handling after base animation and masked layers.
- Expose committed IK observations and staged C# controls through independent `animation_ik_v1`, preserving earlier epoch-7 services prefixes; extend the required prefix to 288 bytes. [Contract](docs/TWO_BONE_IK.md).
- Preserve instance remapping, corrected skin/HitRegion poses, complete rollback and fresh durable continuation with nested animation-state version 4 only for IK worlds.
- Pass fifteen Windows native suites, nine IK groups, three API groups, 79 compiled ABI checks, actual CoreCLR/Native AOT consumers and four exact GPU reference comparisons on AMD and NVIDIA. [Results](docs/evidence/m2-two-bone-ik.json).

## 0.0.117

- Add frozen query-only HitRegion spheres, boxes and capsules with self/ancestor owners and explicit names, including remapped hierarchical instances.
- Add composite committed-pose hit tests with exact affine intersections, physics occlusion, independent owner-proxy filtering and deterministic ties; preserve existing raycasts and collision response.
- Negotiate independent `hit_regions_v1` for C# Tick queries through a 272-byte epoch-7 services prefix, preserving earlier ABI prefixes. [Contract](docs/HIT_REGIONS.md).
- Pass twelve Windows native suites, seven hit-region groups, four API groups, 78 compiled ABI checks and actual CoreCLR/Native AOT animated-query, rollback and fresh-save continuation. [Results](docs/evidence/m2-hit-regions.json).

## 0.0.116

- Add optional explicit `node_map` correspondence to `reference-rotation-v1` for differently named imported character and animation nodes.
- Require 1–1,024 bijective original-index pairs covering skin and selected-channel ancestry with exact direct-parent/root correspondence; preserve omitted-map behavior and original-model fingerprint guards.
- Preserve target geometry, defaults and inverse binds through target-shaped conversion; selected position deltas continue to use donor IDs. [Contract](docs/ANIMATION_RETARGETING.md#map-differently-named-nodes).

- Pass five Windows native suites, eleven retarget groups and eight import tests with no skips, including original licensed character regressions. [Results](docs/evidence/m2-animation-node-map.json).

## 0.0.115

- Add optional named digital actions to v3 input profiles, scripted input and replay, preserving legacy bindings and defaults.
- Deliver held state and first-tick press/release edges to marked C# gameplay; preserve quick taps, neutral absence and complete batch rollback.
- Negotiate independent `input_actions_v1` through the SDK, bridge, generator and native publisher with a 264-byte services prefix, preserving epoch 7 and earlier prefixes.
- Pass ten Windows native CPU groups, 77 compiled ABI checks, actual CoreCLR/Native AOT hitscan and save continuation, and forward/deferred player replay matching the full headless checkpoint. [Results](docs/evidence/m2-input-actions.json).

## 0.0.114

- Add guarded live adoption of all five PBR texture maps, samplers and normal/occlusion strengths on existing authored drawables, preserving gameplay and original geometry/rig owners.
- Preserve imported-map inheritance, explicit null clears and component removal; admit only selected texture dependencies and precisely associated provenance changes.
- Share scalar material epochs, exact retry receipts, save baselines and renderer acknowledgements through schema revision 83.
- Pass nine Windows native checks, 106 forward/deferred hardware captures, continuous-player replacements and retained C# game completion with exact fresh restore. [Results](docs/evidence/m2-live-textures.json).

## 0.0.113

- Add explicit guarded adoption of authored PBR scalar changes on selected existing drawables, preserving the running owner, supported live state and immutable asset owners.
- Separate native acceptance from exact renderer observation/submission/presentation receipts; bind captures and saves to the material epoch through schema revision 82.
- Preserve durable adopted baselines and exact retries; reject unrelated authoring, pending saves and stale guards.
- Pass eight Windows native checks, 52 scalar-material captures across both render paths, same-window paused/running edits and retained C# game continuation through fresh restore. [Results](docs/evidence/m2-live-materials.json).

## 0.0.112

- Add explicit version-3 save-upgrade plans for new custom component types, defaulted only on approved original authored entities still live in the checkpoint.
- Bind new types to exact target fingerprints; preserve existing state and reject unlisted types, unsupported membership changes and nondefault target values. Expose separate addition reports through schema revision 81.
- Pass eight native checks and a retained Windows CoreCLR game upgrade: verify defaults before callbacks, finish the objective, save and restore in a fresh process. Retain 4,292 lossless native request/result/error rows. [Results](docs/evidence/m2-defaulted-component-additions.json).

## 0.0.111

- Add independent cast/receive shadow controls to primitive, static and skinned drawables, preserving default behavior and frozen runtime values.
- Exclude cast-disabled objects from shadow candidates while retaining camera visibility; expose strict optional Boolean fields through schema revision 80.
- Pass four native suites and 108 GPU captures across both render paths and filters; preserve 32 prior default images and 12 explicit-default controls exactly. [Results](docs/evidence/m2-renderer-shadow-controls.json).

## 0.0.110

- Use the normal-biased receiver position consistently when selecting point-light shadow-map faces, preventing light leaks at cube-face boundaries.
- Verify both filters through 64 before/after captures, preserving 24 control bitmaps exactly; pass 49 forward shadow-regression captures and retain a public mirrored-seam test. [Results](docs/evidence/m2-point-shadow-face.json).

## 0.0.109

- Add a bounded Python evidence journal that appends settled records and reconstructs the complete evidence once, with strict typed equality and finite storage limits. [Usage](docs/EVIDENCE_JOURNAL.md).
- Retain pending replies, errors, duplicate records and failed-run metadata; keep persistence success separate from the recorded experiment's outcome.
- Pass 25 journal regressions on Linux and Windows; verify the fresh Windows build, native development contracts and real SDK success, failure and repair. [Checks](docs/evidence/m2-evidence-journal.json).
- Record one fresh native 0.0.108 Relay Yard cohort with 20 real compatible edits, 25 build jobs and complete state/first-Tick checks. End-to-end p95 is 3.004 seconds, still above the three-second target; this is not a matched performance comparison. [Qualification](docs/evidence/m2-iteration-journal.json).

## 0.0.108

- Add Python client helpers for inspecting and waiting for accepted build jobs, with compact progress reads and finite polling budgets.
- Preserve native terminal outcomes and compiler metadata; retain detached timeout context without implicit cancellation, retries or reconnection.
- Check strict lifecycle, deadline and error contracts on Linux and Windows. Verify checkout and installed-wheel clients through six real Windows SDK success, syntax-failure and repair builds against native 0.0.107. [Qualification](docs/evidence/m2-python-development-jobs.json).

## 0.0.107

- Add compact single-job inspection with strict optional `include_output`, preserving full responses by default and keeping errors, verified results and compiler ownership metadata.
- Avoid copying raw diagnostic tails during native summary polling; keep worker lifecycle, cancellation and retry behavior intact.
- Verify seven Windows contract/SDK cases and focused discovery guards, including exact full-to-compact projections. [Qualification](docs/evidence/m2-compact-development-poll.json).
- Record complete 0.0.106 iteration comparisons: batched observation reaches p95 3.759 seconds while preserving the checked state; the three-second target remains open. [Results](docs/evidence/m2-iteration-comparison.json).

## 0.0.106

- Handle ordinary verbose compiler invocation and response-file lines in structured diagnostics, with finite 128 KiB stream and 64 KiB line limits.
- Preserve explicit incomplete reports for truncated, malformed or over-limit input; check boundaries and errors following verbose output.
- Verify Windows/Linux parser regressions and fresh owned-compiler success, syntax failure and repair. [Qualification](docs/evidence/m2-diagnostics-budget.json).

## 0.0.105

- Add explicit owned Windows compiler sessions to C# build jobs, preserving isolated defaults and ordinary MSBuild project/generator behavior.
- Report process reuse separately from correlated owned-compiler/Csc evidence, with bounded diagnostics and worker-owned lifetime.
- Complete compiler retirement before publishing cancellation, timeout or failed verification; preserve retry identity and already-written artifacts.
- Recover a naturally expired compiler cache before launching the next build, after verifying cleanup and unchanged tools.
- Verify ten real SDK jobs, queued and late cancellation, healthy shutdown and five native contract groups. [Compiler contract and evidence](docs/DEVELOPMENT_JOBS.md#optional-owned-windows-compiler).

## 0.0.104

- Add an explicit dependency-restore option to C# compile jobs, preserving ordinary builds by default and allowing prepared projects to select `--no-restore`.
- Guard the option's type and retry identity, and expose it through schema revision 77.
- Verify native contracts and five real Windows SDK builds, including genuine missing-restore and syntax failures. [Build policy and qualification](docs/DEVELOPMENT_JOBS.md#dependency-restore-during-c-iteration).

## 0.0.103

- Add `--save-write-mode synchronous|background` to native game launch and serving, with strict admission and matching CLI discovery.
- Pump background save completion during player polling, including paused polls, without advancing gameplay or waiting for storage.
- Report any pending gameplay save at the final blocking-play boundary; retain synchronous defaults and original retry outcomes.

## 0.0.102

- Add opt-in background gameplay save writes, capturing one immutable checkpoint and publishing storage results on the owning engine thread.
- Continue fixed ticks and ordinary UI callbacks while the write is pending; retain the original save boundary, Busy admission, mutation guards and receipt recovery.
- Keep synchronous writes as the default and reject background mode in zero-worker reference configurations.

## 0.0.101

- Add Python SDK pagination for registered runtime components, preserving all runtime guards, source boundaries, native membership and selected fields across pages.
- Reject boolean and floating-point read guards in otherwise successful core replies, preserving request context and transport usability.
- Check the SDK on Linux and Windows and verify checkout/wheel queries against 65 real native components, including stale-read rejection without retry. [SDK contract and qualification](docs/PYTHON_CLIENT.md#traverse-registered-runtime-components).

## 0.0.100

- Fix development-job cleanup deadlocks by releasing verifier captures outside the worker mutex. Keep capture construction and failed-admission cleanup outside the mutex too.
- Add a bounded regression using real children and a gated verifier, checking reentrant inspection, cancellation and suppression of successful results.

## 0.0.99

- Add opt-in fractional shadow filtering with observable mode selection across captures and native play. Preserve the default filter, map allocation and receiver bias.
- Compare 33 captures per mode with actual skinned runtime poses: all reference images match the previous engine, and shadow-disabled images remain exact between filters. [Comparison and reproduction](docs/SHADOWS.md#controlled-character-comparison).
- Verify both modes through real paused player windows, including pose preservation, capture retries and clean shutdown.
- Pass selected native contracts and record the remaining GPU-cost, moving-scene and broader light-type qualification limits. GI, RTAO, caching and physical soft shadows remain separate work.

## 0.0.98

- Record 1,801 cumulative seconds of native-clock Performance Yard play through 40 exact save/load cycles and a separate fresh-process continuation. Preserve the earlier failed attempt and its unknown cause.
- Add bounded paused and terminal diagnostics to the reliability verifier, with raw frame/memory evidence and exact retry checks. [Results and reproduction](docs/PERFORMANCE_YARD_RELIABILITY.md).
- Qualify 20 real compatible C# edits, complete state preservation, rejected-code recovery, explicit scene restart and original-image save restoration. End-to-end p95 is 7.60 seconds, exceeding the three-second target. [Results and reproduction](docs/RELAY_YARD_ITERATION.md).
- Record exact tested-engine and tooling lineage separately from the source checkpoint; the full Alpha qualification remains open.

## 0.0.97

- Retain a bounded native slow-poll history with current and preceding CPU stages, clock observations, viewport resize work and separate audio device timings.
- Add guarded, read-only `player.diagnostics.inspect` with terminal observations retained after the native window closes. Preserve interactive clock policy, GPU timestamp admission and gameplay/save formats.
- Add the compiled Performance Yard reliability harness with complete checkpoint comparisons, immediate retries, current preferences and fresh-process continuation checks.
- Record the first longer attempt as failed: 23 exact restore cycles precede a clock stall in cycle 24. Preserve raw timing/memory observations and the unknown viewport-change cause. [Reproduction and scope](docs/PERFORMANCE_YARD_RELIABILITY.md).
- Clarify planned character-shadow quality, cache invalidation, per-instance controls, GI and optional RTAO.

## 0.0.96

- Read current native camera and lighting values without rebuilding geometry or animation skin palettes for every player report. Preserve cached report boundaries, rendering and the JSON contract.
- Share camera validation with full scene snapshots and test moving lights, independent lenses, owned observations, checkpoint continuation and invalid cameras.
- Compare the unchanged interactive Performance Yard workload on both laptop GPUs, retaining raw frame/memory observations and timing-budget results. [Results](docs/evidence/m2-camera-observation.json), [reproduction](docs/CAMERA_OBSERVATION.md).
- Recheck native runtime, authoring, shared-player, live-preference and repeated checkpoint behavior.

## 0.0.95

- Reduce GTAO work with a direct front-facing integral and removal of unused neighbor normal reads under the renderer-owned G-buffer producer contract. Preserve resolution, quality settings, sample counts and filtering.
- Add paired reference/candidate captures with fixed image/probe budgets, pinned geometry/content and exact AO-disabled image checks on both laptop GPUs.
- Measure lower AMD AO and total GPU cost in the unchanged Performance Yard workload. Preserve the remaining p95 budget miss, NVIDIA cadence results and earlier candidates without meaningful AMD gains. [Results](docs/evidence/m2-ao-optimization.json).
- Recheck native contracts and the original AO integration, reconstruction and UI fixtures without relaxing their visual assertions. [Guide and reproduction](docs/AMBIENT_OCCLUSION.md).

## 0.0.94

- Add Performance Yard: an API-authored workload retaining Relay Yard's compiled gameplay, imported character, navigation, UI and licensed audio, with additional rigs, geometry, lights and physics bodies.
- Add an opt-in compiled navigation circuit with saved counters for committed movement, path plans and complete laps; run on the normal native clock without per-tick RPC or replay.
- Add a Windows workload benchmark that records raw frame/GPU/memory data, actual focus, owning-monitor mode and power source. Keep functional integrity and timing budgets separate. [Workload and reproduction](examples/performance-yard/README.md).
- Qualify genuine Windows Native AOT gameplay and one-minute combined workload measurements on NVIDIA and AMD. Preserve the AMD timing misses, raw observations and short-run limits. [Evidence](docs/evidence/m2-performance-yard.json).

## 0.0.93

- Add guarded native frame captures with immutable paging, raw present-return cadence, overlapping CPU wall stages and per-submission GPU outcomes.
- Drain actual GPU work before freezing; retain explicit overflow, failed samples and capture/save/resize/pause interventions without changing existing profiler behavior.
- Add a Windows process-memory sampler with pinned process identity, separate resident working-set/private-commit counters and bounded partial results.
- Qualify one/two-slot rendering, resize, captures, overflow and memory observations on NVIDIA and AMD GPUs. Representative gameplay performance remains a separate gate. [Guide](docs/FRAME_PERFORMANCE.md), [evidence](docs/evidence/m2-frame-performance.json).

## 0.0.92

- Publish DEPOT RUN: retained agent-authored C#, native scene/UI, licensed cooked character/audio and portable exercise rules.
- Add an independent verifier for ordered controller gameplay, complete same/fresh-process checkpoints, native artifact/bundle inventories and compiled Begin resumption.
- Qualify Windows Native AOT gameplay and relocated source-free export using the measured 0.0.91 engine; repeat against the public fixture/verifier.
- Preserve the incomplete first attempt, explicit owner recovery and verifier failures alongside successful checks. [Reproduction](docs/evidence/fixtures/agent-depot/README.md), [evidence](docs/evidence/m2-agent-depot.json).

## 0.0.91

- Add owner-configured native C# publication and game export jobs, with fixed tools, complete child environments, guarded relative paths and retry receipts.
- Verify artifacts and bundle inventories on the worker before reporting success; return compact hashes and target information without blocking world queries.
- Require the separate MCP `poima_build` tool for compile, publish and export submissions. World editing permission does not authorize build jobs. [Contract](docs/DEVELOPMENT_PROFILES.md).
- Qualify genuine Native AOT publication and compiled callbacks on Windows and Linux, Windows game export, standalone/shared profile startup and native contract regressions. Preserve failed attempts and platform limits. [Evidence](docs/evidence/m2-development-profiles.json).

## 0.0.90

- Native development jobs can receive a complete per-job environment, including an explicitly empty one, without changing the host environment. Validate Unicode, duplicate names and size bounds before accepting work. [Contract](docs/DEVELOPMENT_JOBS.md#per-job-environments).
- Native gameplay publishing disables persistent compiler and MSBuild servers for supervised publication.
- Qualify real matching-host SDK builds, Native AOT publication and compiled callbacks through the worker with selected linker/cache variables. [Evidence](docs/evidence/m2-development-environment.json).

## 0.0.89

- Fix Native AOT publication for games referencing the distributed gameplay SDK DLL: generated bindings use the SDK copied by the selected game build.
- Reject SDK references that disable copying with an actionable error before native generation or artifact publication.
- Add matching-host regression checks for DLL and project references, genuine native Tick/Control execution and missing-copy rejection. Document the [publishing contract](docs/NATIVE_GAMEPLAY.md#publish). [Evidence](docs/evidence/m2-native-sdk-reference.json).

## 0.0.88

- Add licensed Relay Yard pickup, denial, arrival, completion and displacement-driven footstep cues, using permanent emitters and saved scalar cadence components.
- Preserve original Kenney CC0 sounds, licenses and hashes; add pinned offline conversion and validated native import/provenance/export closure.
- Verify genuine Native AOT playthroughs, active-cue fresh-process saves and continuous offline/WASAPI mixer parity on both laptop GPUs; verify source-free exported continuation with exact audio state.
- Keep existing global persistence IDs and native gameplay services unchanged; document the separate audio scene/save cohort and [runnable checks](examples/relay-yard/README.md#optional-licensed-audio). [Evidence](docs/evidence/m2-relay-yard-audio.json).

## 0.0.87

- Fix saves after removing played runtime sound emitters: preserve surviving voices and the monotonic allocator through gaps or completely retired history.
- Verify real spawn/play/despawn, relocated fresh-process restoration, rejected sound-batch rollback and continued handle allocation on Windows and Linux; retain the previous-build failure reproduction.
- Recheck native audio output on both laptop GPUs and retained Relay Yard save upgrades. Document [emitter lifetime and saves](docs/AUDIO_EVENTS.md#removing-emitters-and-saving). [Evidence](docs/evidence/m2-sound-retirement.json).

## 0.0.86

- Add persistent IDs to Relay Yard's 49 existing state fields and an explicitly defaulted checkpoint version, preserving its native services prefix.
- Verify actual retained 0.0.85 checkpoints against separately compiled targets on Windows and Linux: exact-source restoration, denied updates, explicit hash-bound upgrade, complete native-state preservation, real game completion and fresh-process target saves.
- Compare exact same-tick source/target Vulkan pixels with positive imported weighted geometry on both laptop GPUs; re-qualify the updated source-free exported game on both GPUs.
- Document the [gameplay/content/save boundary](docs/ALPHA_GAMEPLAY_PROFILE.md) and [retained-game update workflow](examples/relay-yard/UPGRADING.md), with explicit compatibility limits and preserved failed attempts. [Evidence](docs/evidence/m2-relay-yard-upgrade.json).

## 0.0.85

- Add Relay Yard: a complete small C# game with native first-person pickups, imported Idle/Run courier navigation, compiled menus, current preferences and checkpoints.
- Add genuine Native AOT playthrough checks for Windows and Linux, including negative interaction, repeated saves, exact partial restoration, stale guards and objective completion.
- Qualify source-free Windows export and fresh-process continuation on both laptop GPUs, with native frame observations, retained failed attempts and a documented [playbook](examples/relay-yard/README.md). [Evidence](docs/evidence/m2-relay-yard.json).

## 0.0.84

- Keep the interactive native player alive beyond 32 checkpoint restores while preserving input release, paused continuation, current preferences and restored-controller validation.
- Share replacement diagnostics across initialized and deferred windows; saturate the count without limiting the player lifetime. Recorded replay still stops at Runtime replacement.
- Add a real 40-restore graphical verifier with movement, exact controller/camera restoration, stale guards, receipt retries and same-context captures. Retain the reproduced 0.0.83 failure separately from the corrected runs. [Evidence](docs/evidence/m2-player-checkpoint-cycles.json).

## 0.0.83

- Add `game serve` for verified immutable bundles, automatically starting the declared native Runtime and optional Native AOT gameplay before serving the existing agent API.
- Reuse the owner-thread local host loop for CLI and MCP clients; preserve guarded operations, reconnect/retry behavior, player presentation and external checkpoints.
- Keep whole-bundle read-only protection and acquire endpoint ownership before bundle/runtime initialization; malformed startup releases ownership cleanly.
- Expose the bundled input-profile path through `game inspect` and add simulation-aware service capability discovery without changing authoring-core v1 or the gameplay ABI.
- Verify source-free native menu/settings/save replacement and captures on both laptop GPUs, plus a separate no-window service/MCP contract. Publish the [service task](docs/GAME_SERVICE.md) and [recorded evidence](docs/evidence/m2-game-service.json).

## 0.0.82

- Qualify compiled settings, native menu input and checkpoint restore inside a genuinely exported Windows Native AOT game, using the retained 0.0.81 gameplay artifact.
- Verify removed owned source, relocation, system-only launch environment, unchanged runtime/artifact/bundle inventories and external durable save storage.
- Observe initial/modal/restored UI pixels before dependent input; retain strict compiled callback, acceptance-ticket, preference application and final Vulkan image checks. Keep physical-input and game-scale qualification separate.
- Document stripped runtime installation and the exporter's per-file budget; retain the failed debug-symbol export and input-verifier attempts in the qualification record.
- Publish the [exported-player playbook](docs/EXPORTED_PLAYER_SETTINGS.md) and [recorded evidence](docs/evidence/m2-exported-player-settings.json), and connect them to the documentation task index and coverage inventory.

## 0.0.81

- Add independently negotiated [compiled player preferences](docs/COMPILED_PLAYER_SETTINGS.md) to C# Tick/Control: typed snapshots, guarded staging and bounded acceptance tickets over the existing native owner.
- Publish one patch only after the whole native step or Control boundary succeeds. Roll back failed batches without consuming preference revisions/tickets, and flush accepted intent before queued save/load replacement.
- Preserve services epoch 7 and all earlier prefixes; advertise the new named 256-byte extension in gameplay, discovery and installed-runtime metadata. Keep preferences outside world and gameplay-save authority.
- Add a rendered native settings-menu fixture for FOV, pointer tuning, UI scale, output gain, sparse reset and next-launch graphics. Qualify Windows CoreCLR/Native AOT on both GPUs, Linux headless callbacks and retained original compiled consumers. [Evidence](docs/evidence/m2-compiled-player-settings.json).
- Publish the API, menu playbook, recovery reference and inspected capture; compile the guide example against the matching SDK and connect the shared documentation index and coverage inventory.

## 0.0.80

- Add a shared native [live player-preference owner](docs/PLAYER_SETTINGS.md), guarded inspection/patches, preview/reset, independent revisions and retry recovery.
- Apply FOV, pointer sensitivity/inversion, UI scale and output gain without changing authored cameras, gameplay snapshots or stored profiles. Preserve held/queued input and invalidate scaled UI hit regions until redraw.
- Report accepted, applied and presented state separately; retain graphics samples/frame slots as explicit next-player intent and reject invalid combined renderer choices before publication.
- Preserve existing launch-report outcomes and compiled gameplay interfaces. Qualify both GPUs, real SDL numeric gain, retained Windows Native AOT/CoreCLR character games and same-window save continuation. [Evidence](docs/evidence/m2-live-player-settings.json).
- Publish and execute the live-settings playbook, update documentation coverage and register device-free native/protocol checks with CTest.

## 0.0.79

- Extend [explicit save upgrades](docs/SAVE_UPGRADES.md) to complete hierarchy recipes, version-6 checkpoints and bounded component collections while preserving native graph state and allocator history.
- Preserve fields by stable ID across renames and layout changes; require explicit retirement and added defaults. Add version-2 capacity approvals that retain array order and reject overflow. Keep version-1 plans and scalar-only mapping compatible.
- Validate the complete source before retiring fields, reconstruct the mapped target before publication, and preserve ordinary exact restores and guarded retry recovery.
- Qualify separately compiled Windows/Linux CoreCLR and Native AOT retained-save consumers, continued collection edits, hierarchy retirement/rebirth and rejection isolation. Preserve old scalar workflows and original artifacts. [Evidence](docs/evidence/m2-hierarchical-save-upgrades.json).
- Add a retained-game upgrade playbook with prerequisites, verification and recovery; reconcile related save, instance and collection reference pages.

## 0.0.78

- Add [complete runtime hierarchy instances](docs/RUNTIME_INSTANCES.md): frozen local graphs, fresh member IDs, local-reference remapping and atomic character, camera, rig/skin, lighting and audio membership.
- Preserve surviving simulation and animation state through births, cancellations and whole-instance removal. Repair incoming references before deletion; retain complete instance maps and allocator history in exact version-6 checkpoints.
- Add an independently negotiated 232-byte C# node-resolution extension while preserving older service profiles. Carry nested asset closure through dependency queries and export, and advertise the feature in installed runtime metadata.
- Qualify Windows/Linux CoreCLR and Native AOT with two original imported characters, independent pose/geometry checks, rollback and save continuation. Check generated player cameras and relocated compiled playback on both GPUs, preserving earlier compiled artifacts. [Evidence](docs/evidence/m2-runtime-instances.json).
- Publish the runnable recipe, compiled fixture instructions and recovery/compatibility reference. Hierarchical schema upgrades and desktop prefab authoring remain separate work.

## 0.0.77

- Add a [live shared native player](docs/LIVE_PLAYER.md): deferred launch, readiness inspection, guarded pause/resume/stop and fresh captures through the existing graphics context without advancing simulation.
- Share a frame-driven native lifecycle with blocking playback. Finish accepted client batches before each frame; guard runtime ownership and replay, retain retry outcomes, and reconcile save replacement without stale input or borrowed runtime pointers.
- Prepare player readback on demand, retain terminal reports and release graphics resources on completion. Qualify both laptop GPUs, compiled Native AOT UI/save callbacks, independent runtime/image continuation and preserved player/game contracts. [Evidence](docs/evidence/m2-live-player.json).
- Publish a runnable observation playbook and a [documentation coverage inventory](docs/DOCUMENTATION_COVERAGE.md) for the manual, API reference, task guides and compact agent skills.

## 0.0.76

- Add [portable native player settings](docs/PLAYER_SETTINGS.md) for FOV, pointer sensitivity/inversion, absolute UI scale, MSAA and outstanding graphics frames. Keep sparse preferences separate from authored worlds, input bindings and saves.
- Provide independent revisions, preview/reset, persistent retry receipts and guarded file replacement. Resolve preferences at player launch, report their effective values/sources, and preserve semantic replay input.
- Connect exported-game CLI profiles and overrides, retain explicit option precedence and read external preferences without bundle writes or lock sidecars. Reject stale profiles and duplicate override keys.
- Qualify three native builds, persisted contracts, eight Vulkan readbacks on both GPUs, source-free relocated game launch and the preserved compiled locomotion sample. Publish a tested settings playbook and add repeatable CTest coverage. [Evidence](docs/evidence/m2-player-settings.json).

## 0.0.75

- Add [Locomotion Yard](examples/locomotion-yard/README.md), a compiled character game using original separate FBX Idle/Run takes, explicit retargeting, native capsule navigation and controller-derived playback rates.
- Keep both loops advancing; preserve cumulative clocks through rate corrections and defer those corrections until active inertial transitions finish. Exercise physical corner slowdown and exact midfade checkpoint continuation.
- Qualify Windows/Linux CoreCLR and Native AOT, independent original-source joint/skin calculations, nine Windows Vulkan readbacks on both GPUs, and a relocated source-free 1,400-tick Windows player followed by separate bundled-runtime checks. Preserve selected authoring, MCP and retargeting contracts. [Evidence](docs/evidence/m2-locomotion-game.json).
- Publish the complete character-game task playbook and current licensed capture. Distinguish reusable engine mechanisms from game-specific decisions in the documentation standard; retain contact, stride, loop repair and broader character qualification as separate work.

## 0.0.74

- Add [guarded original-source inspection and explicit rotation retargeting](docs/ANIMATION_RETARGETING.md). Inspect animation-only donors before cooking; bind original reference/take/node selections to normalized-model fingerprints and reject changed sources.
- Transfer quaternion-chain orientations across differing proportions and reference stances while preserving target geometry, defaults and inverse binds. Declare fixed target positions/scales or selected source-local position deltas; preserve raw rotation curves, cubic tangents, sparse defaults and original durations.
- Qualify three native builds, independent analytic motion and original Kenney Idle/Run/Jump checks, 22 actual Windows Vulkan readbacks across both GPUs, and the preserved compiled character game. Retain exact composition, frame conversion and stable authoring/MCP contracts. [Evidence](docs/evidence/m2-animation-rotation-retarget.json).
- Publish the source-inspection/retargeting playbook, recovery rules and current licensed character capture. Document remaining contact, stride, root-controller, loop and compiled-locomotion work.

## 0.0.73

- Add explicit [reference-pose bone-frame conversion](docs/ANIMATION_FRAME_TRANSFER.md) for compatible named hierarchies with coincident joint origins and exactly uniform mapped scales. Retain target geometry and inverse binds; choose original reference takes independently of selected motion and declare component alignment when needed.
- Preserve interpolation, key times, raw quaternion norms and cubic tangents; add constant channels for transformed source defaults absent from selected clips. Keep default exact-skeleton composition and failed-import publication behavior unchanged.
- Qualify three native builds, independent joint/skin motion and recovery checks, 16 Windows Vulkan readbacks across both GPUs, and the existing compiled character game. Preserve authoring and MCP contracts. [Evidence](docs/evidence/m2-animation-frame-transfer.json).
- Link the conversion playbook, document its runnable checks and limits, clarify historical Character Yard version setup, and require human workflow instructions and a capability-based documentation coverage inventory.

## 0.0.72

- Return versioned, bounded node and local-frame reports when exact-skeleton clip composition rejects an incompatible donor. Include missing-node roles, translation/scale differences, rotation diagnostics and complete issue counts. Keep existing admission thresholds and compatible cooked output.
- Expose the report schema through focused `asset.import` discovery and verify full payload preservation through the Python client and MCP. Document deliberate recovery and add the workflow to the [task playbooks](docs/PLAYBOOKS.md).
- Qualify native Windows, Linux runtime and simulation-disabled authoring, including analytic numerical checks, bounded reports, unchanged failed-import state and same/fresh-owner recovery. Preserve existing FBX and authoring contracts and run the previous compiled Character Yard artifact against the new runtime. [Evidence](docs/evidence/m2-animation-composition-diagnostics.json).

## 0.0.71

- Add [Character Yard](examples/character-yard/README.md), a compiled imported-character game with native capsule routing, source animation, dispatch controls and durable checkpoints. Preserve the source rig and align its visual wrapper with the controller; the supplied arm-opening take is not locomotion.
- Author a compact wine-styled runtime HUD and check all seven controls at small and larger virtual viewports. Save/load labels follow native terminal results rather than request acknowledgements.
- Qualify independent source poses, per-tick physical detours, reload policy and exact same/fresh-owner checkpoint continuation across Windows/Linux CoreCLR and Native AOT. Run eight Vulkan observations across both laptop GPUs. [Evidence](docs/evidence/m2-character-game.json).
- Publish and export an actual Windows Native AOT game, then run its relocated player for 1,400 ticks after removing the owned source project. Verify the bundled runtime separately and retain character attribution, dependency notices and exact bundle inventory.
- Link the complete character workflow through the task playbooks and documentation, with build options, source licensing, expected results and recovery instructions.

## 0.0.70

- Decompose supported static glTF node matrices into editable local TRS while preserving hierarchy, skin joints and inverse binds. Reject unsupported transforms and matrix/TRS animation conflicts under explicit numerical bounds. [Transform policy](docs/ASSETS.md#matrix-authored-transforms).
- Add a tested imported-character playbook using an unchanged licensed human figure, independent source-pose calculations and source-independent cooked content. [Guide](docs/IMPORTED_CHARACTER.md).
- Qualify Linux, Windows and simulation-disabled authoring, including numerical-boundary native tests, existing asset/animation/FBX regressions and 24 Vulkan captures on both laptop GPUs. Keep character gameplay, locomotion and retargeting scope explicit. [Evidence](docs/evidence/m2-gltf-matrix-intake.json).

## 0.0.69

- Share a bounded native executor across animation sampling and procedural material baking, with dependency-aware frame/background lanes, reserved frame capacity, cooperative cancellation and a serial reference policy. [Native jobs](docs/JOBS.md).
- Sample independent rigs into private candidates, preserve serial pose/save results and commit only after complete success. Keep material publication on its owner and fence callback cleanup before terminal completion.
- Record actual worker CPU intervals, queue delays and submission attribution in the profiler; export physical thread lanes and reject late observations from retired recordings.
- Qualify native Windows/Linux consumers, existing CoreCLR and preserved Native AOT gameplay, sanitizer checks and exact NVIDIA Vulkan pose references. Retain workload timing summaries without claiming whole-game performance. [Evidence](docs/evidence/m2-native-jobs.json).
- Add a task playbook index with prerequisites and recovery checks, compile/run the manual's C++ example, and rerun guarded editing with a freshly installed Python client.

## 0.0.68

- Import bounded ASCII/binary FBX models and transform takes through pinned ufbx, with metric/axis normalization, static or linear-skinned geometry, opaque materials and contained PNG/JPEG textures. [Import profile](docs/FBX_IMPORT.md).
- Compose selected, renamed clips from separate sources using exact normalized hierarchy/rest-frame matching. Reject mismatches and unsupported deformation instead of silently retargeting or dropping influences.
- Preserve existing glTF cooked identities; protect model staging files from truncation and return bounded UTF-8 filesystem diagnostics with same-owner recovery.
- Qualify original fixtures and asset/animation/authoring regressions on Windows/Linux, plus source-independent Windows Vulkan skinning and 30-tick runtime reference captures. [Evidence](docs/evidence/m2-fbx-import.json).
- Define the manual, API reference, tested playbook and agent-skill documentation standard; add a runnable guarded-authoring playbook with conflict reconciliation, retained receipts and fresh-owner persistence.

## 0.0.67

- Separate world API schema construction from the stateful world service so source edits compile the relevant unit independently. Preserve existing compiler optimization, debug information and warnings.
- Make unsigned URL-byte validation explicit and retain Unicode source paths in asset records.
- Match 1,067 scoped discovery responses byte for byte on Windows/Linux, plus complete fresh CLI/shared-host comparisons and native contract regressions. [Evidence](docs/evidence/m2-world-schema-extraction.json).

## 0.0.66

- Refresh the desktop screenshot from the matched Windows editor. Full editor and independent dual-view regressions pass, including fresh-process layout restoration and scripted Game input. [Desktop evidence](docs/evidence/m2-desktop-current.json).
- Add immutable source, license and credit records for cooked assets, with guarded selections, preview, retries and undo/redo. Assets without records remain usable. [Workflow](docs/ASSET_PROVENANCE.md).
- Freeze selected records with runtime and saved content; retain original credits after authoring changes and preserve selections through component save upgrades.
- Export checked records and deterministic credits with exact inventory verification. Retain deliberate unused mappings without bundling unused cooked data, and reject runtimes lacking provenance support.
- Qualify native Windows/Linux authoring, bounds, read-only admission, corruption rejection, save continuation, export relocation and legacy bundle controls. [Evidence](docs/evidence/m2-asset-provenance.json).

## 0.0.65

- Record a relocated Windows Native AOT navigation game using the 0.0.64 runtime and artifact: the original compiled NPC plans around cover and arrives during a 1,400-tick Vulkan replay. Separate fresh worlds exercise the bundled runtime's route, rollback and save continuation. [Evidence](docs/evidence/m2-navigation-shipping.json).
- Add opt-in thin core archives for local Linux development and preserve unchanged generated font headers during configure. Compiler settings and dependency archives remain unchanged; normal archives remain the default. [Build option](docs/BUILD.md#local-linux-thin-archives).
- Document licensed asset discovery, user imports and attribution records. Automatic acquisition and provenance export remain planned. [Content workflow](docs/CONTENT_PRINCIPLES.md).

## 0.0.64

- Bind checked static navigation through guarded world transactions, with preview, retry, undo/redo and typed asset references. Runtime, export and saved-source closure retain the bound package. [Navigation workflow](docs/NAVIGATION.md).
- Query routes from live characters through compiled C# `INavigationGame`; steer independently through normal character input. Immutable meshes, explicit path statuses and bounded Tick queries preserve caller buffers on failure.
- Persist NPC routes and cursors in existing components. The compiled follower verifies physical detours, arrival, whole-batch rollback, compatible development reload and same/fresh-owner save continuation.
- Append the independently negotiated 224-byte navigation service while retaining earlier services-7 profiles and save formats. Runtime inventories advertise the actual navigation feature and its dependency license.
- Commit to no generative-AI art or music in Poima content, with AI-assisted code permitted. Licensed online asset acquisition and procedural wear/deformation are recorded directions. [Content principles](docs/CONTENT_PRINCIPLES.md).
- Windows/Linux CoreCLR and actual Native AOT follower checks pass separately, alongside native, authoring and package-contract regressions. [Recorded evidence](docs/evidence/m2-runtime-navigation.json).

## 0.0.63

- Bake static walkable space from authored box and indexed mesh collision geometry with optional Recast/Detour. Inspect clearances and query revision-guarded routes through native commands. [Navigation API](docs/NAVIGATION.md).
- Report complete, partial, unreachable and budget-limited paths with endpoint projections; reject stale topology and malformed packages. Immutable meshes retain independent query scratch.
- Bound native allocations and recover from every tested Detour allocation-failure stage. Filesystem failures return bounded UTF-8 diagnostics without terminating the owner.
- Isolate upstream backend dependencies from core compile commands and enforce static linkage even when ambient shared-library builds are enabled. Native Windows/Linux and disabled authoring checks pass. [Evidence](docs/evidence/m2-navigation.json).

## 0.0.62

- Generate tileable brick and plaster PBR materials from versioned native recipes: correlated color, tangent normals and roughness, cooked mips and reusable image assets. No downloaded art or generation service is required. [Workflow](docs/PROCEDURAL_MATERIALS.md).
- Inspect, cancel and forget bounded bake jobs without editing the world. Apply results through guarded transactions; shipping bundles contain their image dependencies while recipes remain authoring data.
- Validate stored descriptors and package hashes, reject corrupt outputs and preserve UTF-8 failure diagnostics. Same-profile regeneration checks generated output identity.
- Native Windows/Linux and authoring-only checks pass. Both laptop GPUs pass normal-map controls and repeated material captures with zero reported NVRHI errors. [Evidence](docs/evidence/m2-procedural-materials.json).

## 0.0.61

- Drive camera-free characters from compiled C# with `ICharacterInputGame` and `SetCharacterInput`. NPC movement uses native capsule physics, while interactive players retain their camera bindings. [Character controls](docs/CHARACTER_INPUT.md).
- Stage movement, look and jump per fixed tick, reject caller/NPC control conflicts and restore the whole batch after failed updates. Saves retain actual character and gameplay state rather than queued commands.
- Add the independent `character_input_v1` service extension while preserving older gameplay profiles. Windows and Linux CoreCLR, Native AOT, guarded ABI and preserved-artifact checks pass. [Evidence](docs/evidence/m2-character-input.json).
- Add [Patrol Room](examples/managed/PatrolGame), a compiled guard encounter with vision occlusion, investigation, pursuit, extraction and checkpoint continuation. Native-input checks pass on both OSes, with rendered checkpoints on both laptop GPUs. [Evidence](docs/evidence/m2-patrol-game.json).

## 0.0.60

- Find the authored entity, template and material-slot fields using a cooked asset, or list one owner's references. Typed model/image/audio edges exclude unrelated values and retain distinct repeated uses. [API](docs/ASSET_REFERENCES.md).
- Query references without loading packages, including when files are missing or corrupt. Revision-pinned pages reject stale continuations. Windows, Linux and authoring-only checks pass. [Evidence](docs/evidence/m2-asset-references.json).

## 0.0.59

- Isolate compiled version/platform metadata in one small source file. Version-only changes avoid recompiling the world service, project exporter, MCP adapter and profiler; optimization and debug settings are unchanged.
- Add a native integration check that compares CLI metadata, MCP initialization and exported profiler traces against the actual executable target. Windows, Linux and authoring-only checks pass. [Build details](docs/BUILD.md#incremental-version-builds), [evidence](docs/evidence/m2-build-metadata.json).
- Retain the selected authoring API, services-7 tables and save formats. Preserved Native AOT layer fixtures pass on Windows and Linux.

## 0.0.58 — Unreleased

This checkpoint exposes masked animation layers to compiled C# gameplay. The installed desktop package remains unchanged.

- Opt into `IMaskedAnimationGame`, read committed layer state and stage clip transitions with independent weight ramps through `GetAnimationLayer` / `SetAnimationLayer`. Frozen masks, override/additive modes and references retain the native layer contract. [C# API](docs/MANAGED_GAMEPLAY.md#control-masked-layers-from-c).
- A named 208-byte services-7 extension preserves the original 176-byte and inertial 192-byte tables. Dedicated versioned records, strict decoder guards and shared command budgets retain whole-batch rollback and exact saves. [Artifact contract](docs/NATIVE_GAMEPLAY.md#artifact-contents).
- Each OS passes 59 ABI guard groups, 513-RPC CoreCLR and published 501-RPC Native AOT layer cohorts. Preserved older gameplay artifacts and actual old/new unlayered save comparisons pass. [Evidence](docs/evidence/m2-managed-animation-layers.json).
- Selected native/authoring checks and stable API gates pass. Protocol 1, discovery revision 53 and authoring-core v1 remain unchanged. Desktop layer widgets, IK, root motion and game-scale animation qualification remain unfinished; native libraries still require process restart for replacement.

## 0.0.57 — Unreleased

This checkpoint adds persistent native masked animation layers. The installed desktop package remains unchanged.

- Author up to four ordered override/additive slots with sparse node masks and frozen reference poses. Each layer owns playback, crossfade/inertial history and independent weight ramps. Native commands and joined runtime observations expose the layers. [Contract](docs/RUNTIME_ANIMATION.md#masked-animation-layers).
- Rollback and nested animation-state v3 retain complete layered state and reject changed mask/reference identity. Unlayered v1/v2 bytes remain compatible; actual old/new executable checks pass on Windows and Linux.
- Each OS passes eight layer protocol checks, preserved CoreCLR/Native AOT animation cohorts and fresh save continuation. Both GPUs pass 36 exact reference pairs across 72 captures. The evidence distinguishes initial Linux discovery failures from successful affected reruns. [Evidence](docs/evidence/m2-animation-layers.json).
- Discovery advances to revision 53 while protocol 1, selected authoring-core v1 and the 176/192-byte services-7 tables remain unchanged. Compiled C# layer access, desktop layer widgets and game-scale animation performance remain unfinished.

## 0.0.56 — Unreleased

This checkpoint exposes native inertial animation to compiled C# games through explicit service negotiation. The installed desktop package remains unchanged.

- Declare `IInertialAnimationGame`, select `AnimationTransitionMode.Inertial` with the new overload and inspect nullable active mode/progress with `GetAnimationExtended`. Existing numeric-time calls and the original seven-parameter setter retain crossfade behavior. [C# example](docs/MANAGED_GAMEPLAY.md#control-animation-from-c).
- A named 192-byte services-7 extension preserves the 176-byte baseline callbacks. Host and artifact requirements are validated separately from saved state; unsupported declarations and descriptor omissions reject before game construction. [Artifact contract](docs/NATIVE_GAMEPLAY.md#artifact-contents).
- Both Windows and Linux pass 54 ABI guard groups, 486-RPC CoreCLR cohorts and actual published 435-RPC Native AOT cohorts, including rollback and fresh saves. An unchanged older Windows Native AOT inventory game passes 151 RPCs on the new runtime. [Evidence](docs/evidence/m2-managed-inertial.json).
- Eighteen selected Linux native groups, four authoring-only groups and stable authoring contract gates pass. Protocol 1, discovery revision 52 and authoring-core v1 remain unchanged. Desktop mode controls, layers, IK, retargeting and game-scale animation qualification remain unfinished; native libraries still require process restart for replacement.

## 0.0.55 — Unreleased

This checkpoint adds native inertial animation transitions with persistent motion history. The installed desktop package remains unchanged.

- Opt-in `transition_mode:"inertial"` preserves the exact starting pose and estimated recent output motion, then settles onto the destination over fixed ticks. Translation, quaternion and logarithmic positive-scale corrections use analytic incoming derivatives; interruptions retain one correction rather than a recursive blend tree. [Contract](docs/RUNTIME_ANIMATION.md#inertial-transitions).
- Immutable distinct-tick history and corrections participate in batch rollback and nested animation-state v2 saves. Fresh-process and whole-runtime restoration checks include immediate re-interruption. Crossfade-only version-1 save bytes are retained; an actual 0.0.54 saved interrupted fade restores and continues unchanged.
- Windows/Linux native and protocol checks pass, as do authoring-only checks and the existing compiled C# crossfade/reload/rollback suite. Both laptop GPUs pass 48 exact reference pairs across 96 inertial/legacy captures at 640×480 with 1×/4× MSAA and zero reported NVRHI errors. [Evidence](docs/evidence/m2-animation-inertial.json).
- Discovery advances to revision 52. Gameplay services ABI 7 and authoring-core v1 are unchanged. C# and desktop controls still select crossfades; the new mode currently uses C++ or the world protocol. Layers, IK, foot locking, phase matching and game-scale animation performance remain unfinished.

## 0.0.54 — Unreleased

This checkpoint adds coherent, bounded runtime observations for agent playtesting. The installed desktop package remains unchanged.

- `runtime.observe` joins selected live entity state and native custom-component values in one guarded owner dispatch. Field projection, shared schema metadata, sorted membership pages and revision pins reduce repeated reads without changing simulation. [Protocol](docs/RUNTIME_OBSERVATION.md).
- A reproducible Windows comparison runs the same compiled Workshop Relay game with both observation paths. It preserves thirteen game checks, thirteen route events and six complete checked snapshots, normalizing only runtime session IDs. Native RPCs fall from 2,549 to 1,292; returned JSON bodies are 16.9% smaller under the documented reserialization measure. [Evidence](docs/evidence/m2-runtime-observation.json).
- Nine native protocol tests pass on Windows and Linux. Shared/read-only scope checks, an actual authoring-only build and selected stable authoring-contract gates pass. Discovery advances to revision 51; gameplay ABI, save formats and authoring-core v1 remain unchanged. This provider-free comparison does not measure agent tokens, latency or game performance.

## 0.0.53 — Unreleased

This checkpoint adds a recorded agent-authored inventory and recipe game, with a provider-free public replay. The installed desktop package remains unchanged.

- Codex creates and playtests Workshop Relay through native MCP authoring: a generated three-slot int32 inventory, actual spawned pickups, recipes, deferred Drop, wine-styled HUD/menu and compiled controls. Independent tests observe resources directly, use genuine controller inputs and check full checkpoint restoration and fresh-process continuation. [Source and reproduction](docs/evidence/fixtures/agent-workshop/README.md).
- The retained source rebuilds on Windows with zero warnings or errors. Its replay passes 2,549 native requests on the final host; both owned native processes exit cleanly. The original exercise has separate verification and two inspected NVIDIA Vulkan checkpoints. [Evidence](docs/evidence/m2-agent-workshop.json).
- Public documentation records the generated game's fixed Drop-placement and premature save-label limitations. This is one bounded prototype exercise, with no claim of general agent success rate, physical input, production performance or new gameplay/API implementation.

## 0.0.52 — Unreleased

This checkpoint qualifies the existing generated component collections in Windows Native AOT games. The installed desktop package remains unchanged.

- The actual inventory fixture publishes with its generated component manifest and dependency notices. Compiled capacity-four entity/int32 buffers pass the unchanged 151-RPC mutation, rejection, rollback and fresh-process save contract.
- Exported native games run seven ticks after relocation outside the checkout on AMD and NVIDIA. Both retain all 56 bundle files, report disabled dynamic code and zero NVRHI errors, and contain no managed PE/bridge/CoreCLR payload. Separate save worlds also pass through each bundled runtime. [Evidence](docs/evidence/m2-component-collections-native-windows.json).
- A reusable bounded Windows bundle qualification runner records owned command exits and cleanup. This is fixture qualification, with no new gameplay implementation, ABI or catalog change; clean-machine deployment, other collection kinds, capacity migration and production performance remain unqualified.

## 0.0.51 — Unreleased

This checkpoint qualifies the existing bounded component collections on Windows CoreCLR with the actual compiled inventory fixture. The installed desktop package remains unchanged.

- Matching SDK, source generator and bridge build the fixture and extract its real component manifest. Compiled callbacks exercise ordered entity/int buffers, capacity rejection, published reads and actual template spawning.
- Reference/index/exception failures and a later-tick batch failure preserve complete saved payload bytes. Nonempty inventories restore and continue in two fresh processes, including atomic reference repair and second-generation saves. [Windows evidence](docs/evidence/m2-component-collections-windows.json).
- Documentation distinguishes bounded generated buffers from arbitrary managed arrays. Historical Linux qualification is retained separately; Native AOT collections, global-state collections, nested buffers and capacity migration remain unqualified. No gameplay ABI or API catalog change is introduced.

## 0.0.50 — Unreleased

This checkpoint gives agents typed control over native runtime HUD/menu layouts and styling. The installed desktop package remains unchanged.

- Optional frozen layout/style fields support responsive dp/percent dimensions, anchored placement, flex rows/columns, spacing, visual ordering, palettes, borders, font size and color states. Native validation rejects malformed metadata and unsupported rounded clipping. Older definitions retain absent fields and their default presentation. [UI contract](docs/GAME_UI.md#authored-layout-and-styling).
- `world.ui.layout` observes a revision-pinned virtual viewport with bounds, clipping and hit eligibility when the native UI backend is built. An explicit runtime-capture density matches layout measurements independently of monitor DPI, while omission retains the earlier behavior. Runtime inspection exposes frozen metadata; these UI interfaces remain development APIs outside authoring-core v1.
- Pass-through canvases let HUDs leave gameplay input free. Focus follows visual traversal; pointer and keyboard gestures cancel across action or occlusion changes while retaining release ownership. Confirmation cannot activate a fully occluded control.
- A centered menu/HUD fixture exercises transactions, history, retries, runtime freezing, save-content binding and rendered presentation. Theme assets, images, inventory/text widgets, UI animation, visual authoring and broad accessibility remain unfinished. Both laptop GPUs pass the styled capture suite; seven default-UI capture pairs on AMD are pixel-identical to 0.0.49. [Evidence](docs/evidence/m2-authored-ui.json).

## 0.0.49 — Unreleased

This checkpoint releases authoring-core v1 for a selected authored-world API. The broader engine remains a prototype, and the installed desktop package remains unchanged.

- Native session discovery advertises the contract identity, selected methods/Transform scope and mode-specific mutation availability across full and focused views. Catalog revisions remain independent; runtime, rendering, game saves, gameplay ABI and world formats are outside this guarantee. [Contract](docs/AUTHORING_API_COMPATIBILITY.md).
- Canonical v1 request/response artifacts and a hashed release manifest preserve the earlier candidate schemas and paths. The 0.0.49 Python wheel uses the canonical response resource while retaining the historical one.
- A separate conservative response gate checks that new producer outputs fit old consumer requirements. Recognized required fields, types, bounds and extras are directional; unsupported interacting context fails closed. This does not replace native behavioral qualification.
- Focused native conformance checks caller IDs/errors, old transaction/undo/redo receipts after later edits and restart, true hierarchical shear, keep-local reparenting, pinned query boundaries and storage rejection without consuming history or retry IDs. Windows/Linux request-scope, shared-host and client checks qualify this bounded release. [Evidence](docs/evidence/m2-authoring-core-v1.json).

## 0.0.48 — Unreleased

This checkpoint adds focused mutation discovery for external agents and automation. The installed desktop package remains unchanged.

- `world.describe` can select a transaction operation and optional component type while preserving revision/receipt guards, previews and operation bounds. Read-only/shared restrictions apply first; registered custom types and removal enums keep their original schemas. Full and existing focused views remain available. [Discovery contract](docs/WORLD_SERVICE.md#focused-discovery-development).
- MCP advertises the new selectors, and the 0.0.48 Python client adds `discover_mutation()` with contextual response checks. Native Windows/Linux tests cover advertised selections, observational errors, scope restrictions and unchanged mixed transactions. Installed-wheel checks qualify the updated package.
- Conservative projection retains complete unions when references or unfamiliar schema context prevent pruning. An indexed-reference regression rejects the old contract exporter. The unchanged candidate baseline passes a narrow closed-object union-extension proof; this does not release full-engine API stability.
- A Windows Transform selection produces 1,318 native response bytes versus 27,779 for the complete transaction method; its MCP response is 2,910 bytes versus 60,276. Counts include JSON-RPC envelopes and exclude line delimiters. Response bytes are not provider tokens or measured development speed. [Evidence and reproducible requests](docs/evidence/m2-scoped-mutation-discovery.json).

## 0.0.47 — Unreleased

This checkpoint replaces headless owner retry sleeps with transport readiness waits. The installed desktop package remains unchanged.

- Windows servers retain overlapped connect/read/write operations with private completion events; Linux servers wait on eligible socket readiness. Owner polling still bounds each client's I/O, dispatches requests serially and preserves deferred reply ordering. Pending Windows operations are canceled and reaped before buffer/slot reuse. [Transport contract](docs/SHARED_SESSIONS.md#transport-and-limits).
- Native regressions cover fragmented frames, synchronous completions, budget continuation, deferred pipelines, full client capacity, slow readers and endpoint reuse. Windows/Linux client and shared-host checks, candidate request gates and ten Windows desktop ABI checks pass. The unchanged compiled defense game reproduces all recorded outcomes across 3,208 calls with clean exits.
- Four matched Windows trial pairs reduce shared read-query median from 16.03 ms to 0.159 ms. Five-second idle CPU probes on Windows and Linux record smaller CPU-time deltas, with finite accounting granularity. These are authoring and idle-host measurements, not game FPS, GUI latency or power measurements. [Evidence and limits](docs/evidence/m2-host-readiness.json), [idle probe](tools/benchmark_shared_idle.py).

## 0.0.46 — Unreleased

This checkpoint replaces Windows client exchange retry sleeps with completion waits. The installed desktop package remains unchanged.

- Overlapped pipe reads and writes share one exchange deadline. Timeout or failure cancels and reaps pending operations before releasing buffers, closes the client and never replays work. Kernel cleanup can outlast the requested deadline; server polling and editor cadence are unchanged. [Transport contract](docs/SHARED_SESSIONS.md#transport-and-limits).
- Native Windows regressions cover stalled responses, write backpressure, disconnects, fragmented deadlines and endpoint recovery. Shared-host, SDK, MCP and native desktop ABI checks pass. The unchanged compiled defense game preserves every recorded checkpoint and win/loss outcome with clean process exits.
- Four matched Windows trial pairs reduce shared read-query median from 31.4 ms to 15.9 ms; an old-build repeat remains at 32.2 ms. This is same-machine authoring latency, not game FPS or GUI responsiveness. [Measurement and limits](docs/evidence/m2-client-completion-waits.json).

## 0.0.45 — Unreleased

This checkpoint removes an avoidable local reply wait and adds a reproducible authoring-latency probe. The installed desktop package remains unchanged.

- Native replies attempt immediate nonblocking delivery within a 256 KiB budget, retaining partial frames for later owner polls. No extra request polling, transport threads, busy spin, automatic retry or global timer change is introduced. [Transport contract](docs/SHARED_SESSIONS.md#transport-and-limits).
- Windows/Linux transport regressions cover immediate small/empty replies, slow readers, other-client progress and pipelined frame ordering. The new regression rejects the old queued-only backend. Shared-host, SDK, MCP and native desktop ABI checks pass; the compiled defense game also passes shared-host checkpoint continuation with clean process exits.
- Four matched Windows trial pairs per build reduce the shared read-query median from 47.6 ms to 31.8 ms on the development machine. Client sleeps and editor dispatch cadence remain unchanged; this is authoring latency evidence, not game FPS or GUI responsiveness. [Measurement and limits](docs/evidence/m2-immediate-replies.json), [probe](tools/benchmark_shared_session.py).

## 0.0.44 — Unreleased

This checkpoint adds a second independently checked agent-built game and a reproducible defense fixture. The installed desktop package remains unchanged.

- One fresh Claude Code session creates the scene, templates, logical UI and compiled C# for a three-wave defense game through guarded native MCP authoring. An independent trace audit reconstructs the retained source and authored world; no verifier game repairs or live gameplay patches are used. [Exercise evidence](docs/evidence/m2-agent-defense.json).
- A public Python replay checks real controller movement, cover rays, two-hit drones, finite ammunition, cooldown, reload, victory, unattended loss and exact checkpoint continuation in a separate native process. The unchanged generated source rebuilds and passes 3,222 native calls on Windows CoreCLR with clean exits. This is bounded headless evidence; rendered defense presentation and physical input remain unqualified. [Fixture and reproduction](docs/evidence/fixtures/agent-defense/README.md).

## 0.0.43 — Unreleased

This checkpoint adds an installable Python automation client and candidate core response checks. The installed desktop package remains unchanged.

- A standard-library client opens a native world or connects to a shared endpoint, with guarded mutation helpers, explicit retry receipts and revision-pinned query pagination. Generic calls retain access to discovered engine operations. [Client guide](docs/PYTHON_CLIENT.md).
- Bounded transport checks correlation, UTF-8/JSON envelopes and error responses. Timeouts distinguish unsent work from unknown outcomes; immutable recovery parameters survive interruption. There are no automatic retries or reconnects.
- A packaged candidate response manifest covers nine core authoring methods and discovery/read/transaction variants. Validation permits additive result fields and legacy retained receipts without inventing history metadata. The request gate remains separate; broader API stability is unfinished. [Candidate contract](docs/AUTHORING_API_COMPATIBILITY.md).
- Rebuilt Windows/Linux hosts pass native client authoring, shared-session conflict/detachment, durable retries and legacy receipt checks. Synthetic transport and independent malformed-response fixtures pass on both operating systems; installed-wheel checks qualify local distribution. [Evidence](docs/evidence/m2-python-client.json).

## 0.0.42 — Unreleased

This checkpoint adds repeatable visual evidence for an agent-authored game and a candidate authoring API compatibility gate. The installed desktop package remains unchanged.

- A fresh native Windows replay captures the recorded escape-room game's locked exit, completed escape and restored checkpoint through Vulkan. The retained agent source is unchanged. The three-checkpoint slideshow is rendered replay evidence, not a live agent session or a real-time gameplay recording. [Fixture](docs/evidence/fixtures/agent-escape/README.md).
- A conservative request-schema gate checks nine authored-world methods and `Transform`, with explicit scope projection and regression tests for overlapping exclusive branches. This prepares authoring-core v1; it does not yet promise stable responses, gameplay APIs or save formats. [Candidate contract](docs/AUTHORING_API_COMPATIBILITY.md).
- The rebuilt Windows world host qualifies development compile jobs and structured diagnostics with a real SDK success and intentional failure; authored-world, history and kind-specific UI checks pass. This extends the earlier standalone qualification without activating a new editor package.

## 0.0.41 — Unreleased

This source checkpoint adds compact compiler feedback for agent iteration. The installed desktop package remains unchanged; full world-host integration qualification remains pending.

- `development.diagnostics` returns bounded, deduplicated compiler codes, messages and source locations while retaining the job's actual state and exit status. Raw logs remain available through `development.inspect`; an empty diagnostic list never implies success. [Development jobs](docs/DEVELOPMENT_JOBS.md).
- Native discovery advances to schema revision 47 for the development operations and kind-specific UI schemas. Agents caching discovery can detect the change.

## 0.0.40 — Unreleased

This checkpoint collects development since 0.0.39. The installed desktop package remains at its earlier qualified checkpoint. This entry tracks the next source checkpoint, not an available release download.

### Gameplay and authoring

- Native-owned C# components, generated accessors, template-based root-prop spawning and removal, with guarded runtime edits and save continuation. CoreCLR and Native AOT lifecycle checks cover Linux and Windows; arbitrary component membership changes remain unfinished. [Gameplay lifecycle](docs/GAMEPLAY_LIFECYCLE.md).
- Bounded component collections, qualified with Linux native and CoreCLR gameplay/save checks. Windows and Native AOT collection qualification remains pending. [Collections](docs/COMPONENT_COLLECTIONS.md).
- Explicit scalar save upgrades using stable field identities and guarded restoration. Linux CoreCLR old-save/new-game continuation is qualified; Windows and Native AOT integration remain pending. [Save upgrades](docs/SAVE_UPGRADES.md).
- Native UI state, compiled C# callbacks, Vulkan presentation and player/editor input routing. Physical-device and full desktop interaction qualification remains separate. [Game UI](docs/GAME_UI.md).
- Collection Room, a small C# sample combining movement, collection, UI and durable checkpoints, with Windows CoreCLR/Native AOT continuation checks. [Sample](examples/collection-game).

### Rendering and inspection

- Shared HDR composition and clustered direct-light assignment with bounded overflow fallback. [HDR composition](docs/HDR_COMPOSITION.md).
- Bounded frame submission and a typed render-pass schedule that validates resource identity, initialization and ownership. [Render schedule](docs/RENDER_SCHEDULE.md).
- Depth, world-space shading normals, backward UV motion and raw pixel probes for rendered observations. [Scene products](docs/SCENE_PRODUCTS.md).
- Optional deferred opaque lighting with bounded correctness checks on both development-laptop GPUs. No general performance or GI claim. [Deferred rendering](docs/DEFERRED_RENDERING.md).
- Optional FSR 3.1.4 reconstruction for Windows Vulkan, disabled by default. Integration checks pass, but moving scenes retain near-edge color residue; temporal quality and performance remain unqualified. Frame generation and DLSS are unfinished. [Reconstruction](docs/RECONSTRUCTION.md).
- Experimental spatial GTAO, disabled by default. Linux API/scheduling and bounded capture, reconstruction lifecycle and UI checks pass on both laptop GPUs. General visual quality and performance remain unqualified. [Ambient occlusion](docs/AMBIENT_OCCLUSION.md).

### Development workflow

- UI discovery declares panel, label and button text/action constraints, matching existing native validation. A focused schema/atomic-rejection regression is added; rebuilt-host execution remains pending. [UI contract](docs/GAME_UI.md).

- Bounded asynchronous native compile workers with Linux/Windows real-process checks, session-local compile receipts and UTF-8-safe diagnostic tails. Linux adapter and real SDK compilation pass; full world-host/package integration remains pending. [Development jobs](docs/DEVELOPMENT_JOBS.md).

- One external Codex-authored escape-room exercise passes independent compiled gameplay and checkpoint replay. Its public source fixture separately passes Windows CoreCLR controller replay and checkpoint continuation in a fresh native process. Headless logical UI only; graphical and physical-input qualification remains separate. [Original exercise](docs/evidence/m2-agent-game.json), [reproducible replay](docs/evidence/fixtures/agent-escape).

- Scene-camera smoothing with a session-local toggle: eased wheel zoom and flight acceleration/deceleration, canceled on lost focus/capture or external camera changes. Linux/Windows motion contracts and the Windows desktop regression pass. [Camera controls](docs/DESKTOP_EDITOR.md#scene-navigation).

- Native stdio MCP interface with compact discovery and exact world-operation forwarding. Linux protocol tests pass; real authoring/persistence and shared headless endpoint tests pass on Linux and Windows. Bounded Codex and Claude Code CLI authoring exercises pass. A development Agent window separately passes one Windows Codex chat edit through the editor-owned endpoint; broad autonomous game creation and physical-input qualification remain pending. [Client setup](docs/AGENT_CLIENTS.md). [MCP interface](docs/MCP.md), [evidence](docs/evidence/m2-mcp-shared.json).

- Focused native discovery: list operation/component names or retrieve one schema or contract section. Linux protocol and native scope tests pass. Measured responses are 2,092 bytes for the catalog and 860 bytes for `entity.query`, versus 110,867 bytes for full discovery; these are response sizes, not model-token or latency measurements. Windows compilation passes; execution remains unqualified. [Discovery](docs/WORLD_SERVICE.md#focused-discovery-development), [evidence](docs/evidence/m2-focused-discovery.json).
- Removed an unnecessary dependency from scene data to renderer settings and diagnostics. The full Linux build and all optional baseline object rebuilds pass. Renderer-header dependencies fall from 77 to 17 across the same baseline objects (60 removed). The Windows build and graphics fixtures compile successfully; the same Windows comparison falls from 85 to 31 objects (54 removed). Wall-clock measurements and hardware qualification remain pending. [Dependency evidence](docs/evidence/m2-build-dependencies.json).
- Reworked the README around agent workflows, current capabilities, setup and documentation, with explicit creator/architect and implementation credits.

- Opt-in MCP PNG observations preserve capture receipts and distinguish image-conversion failures from unknown operation outcomes. Linux codec/protocol/regression tests and archived SDL capture conversion pass; fresh GPU-image delivery and simple color identification also pass with Codex and Claude Code. [Observation evidence](docs/evidence/m2-agent-observation.json). [Evidence](docs/evidence/m2-mcp-images.json).

## 0.0.39

The earlier editor-audio checkpoint added Game-camera audio output, guarded audio controls and bounded DSP work, alongside the existing profiler, gamepad assignment, C# components and durable save workflows. Detailed tests and limitations are preserved in the [implementation record](docs/IMPLEMENTATION_STATUS.md) and [editor audio evidence](docs/evidence/m2-editor-audio.json).
