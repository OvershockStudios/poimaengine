# Implementation status

Updated 2026-10-16 for **0.0.167**. Capability discovery reports what is available in each build. The desktop editor and CLI are first-class interfaces over shared native authoring, a Vulkan renderer with forward and optional deferred opaque paths, a configurable physics runtime and a continuous native player. Headless builds run without the editor or its managed runtime. The gameplay SDK and broader production qualification remain incomplete.

## Current limits

Version 0.0.167 adds opt-in [CoreCLR callback metrics](MANAGED_CALLBACK_METRICS.md):
fixed per-module Initialize/Tick/Control counters, current-thread allocated bytes
and runtime-wide GC deltas observed during callbacks. Default loads do not
measure; NativeAOT reports unsupported. Reads never force collection, and
attempts remain diagnostic when native state rolls back. Successful reload or
restore starts a fresh counter lifetime; counters and the enable flag are not
saved gameplay state. The gameplay C ABI, binary SDK and save codec are unchanged;
the public C++ configuration layout extends and does not establish binary
compatibility. These extensions are outside the frozen contracts.

The new bridge passes 171 compiled helper checks against the retained binary
SDK. Six Windows native suites and five Linux headless CTests pass. A real
Windows CoreCLR fixture passes default/off/opt-in, allocating and throwing
callbacks, returned-but-rejected writes, compatible reload, old-bridge rejection
and retained NativeAOT reads. Independent file review joins 357 original calls,
twenty complete checkpoints and eight exact whole-file state pairs. The first
collector's final newline census failure remains failed. Full-scene prerequisites
also retain an import failure, a pre-play census/cleanup failure and a distinct
raw-versus-canonical metadata assertion failure. No representative CoreCLR game,
recording-overhead, sustained memory-growth or Alpha gate closes from these
fixtures. The desktop profiler does not yet display these managed aggregates.
[Results and boundaries](evidence/m2-managed-callback-metrics.json).

Version 0.0.166 adds explicit background native play for unattended agents:
real elapsed-time simulation, a nonfocusable window, owner-controlled pause and
no physical input routed to gameplay. Presentation has its own nominal 60 Hz
opportunity gate; minimized simulation continues without claiming presentation
readiness. Optional audio and lens interpolation are admitted separately from
physical controls. Existing interactive/replay defaults remain available.
Thirteen Windows CPU executions pass. A native background-window check passes
161 calls, compiled Tick/Control, complete paused-save preservation, same-window
Load replacement, guarded BMP capture/retry and actual minimized progression.
Its 52 raw performance rows contain 40 successful presents and zero eligible
foreground cadence samples; this is not a performance budget qualification.
[Workflow and limits](LIVE_PLAYER.md#background-play-for-agents).

The full Performance Yard also passes two warm save/load cycles and fresh-process
continuation through the new clock: 2,394 authored entities, 24 animated rigs,
768 props, 32 dynamic bodies, 64 lights, fifteen cooked assets and retained
NativeAOT gameplay at 1080p with deferred lighting and medium GTAO. Its authored
overview camera uses 50° FOV; settings are not matched to the earlier focused
performance cohort. Five play intervals total 19.132 seconds, with nine complete
checkpoints and seven exact whole-file comparisons. Twenty-nine actual process
memory queries join sixty-nine phase marks across two process identities.
This validates recording integration, not recording overhead, steady memory,
VRAM peaks or the thirty-minute mixed-reliability gate. Thirteen Linux CPU
CTest entries pass with original deadlines, and separate file audits verify
the Windows and Linux records. Earlier source-preparation and reader-limit
failures remain recorded; no native workflow verdict was regraded.
[Results and scope](evidence/m2-background-player.json).

Version 0.0.165 lets agents inspect original skin-joint membership and animation
channel metadata before cooking, with paged fingerprint guards and unchanged
legacy responses. Explicit retarget maps can use that topology without first
importing the sources. Optional development-job process observations retain raw
leader exits and cleanup requests; default status, legacy exit projection and
retry receipts stay unchanged. Both extensions are outside the frozen contracts.
Eight Windows native executions, four new topology groups and the legacy
retarget checks pass. Three actual .NET compiler jobs verify success, structured
CS1525 failure and repair through the Python helpers. All 289 selected old/new
source response pairs and four complete cooked packages match exactly.
All twelve Linux CTests pass under their original limits, but the enclosing
run remains failed: retained intentional FBX size-rejection fixtures exceed
the collector's declared output bounds. A separate file audit verifies the
tests, identities and that failure without changing its verdict. The optional
external-character case is skipped on both systems. New rendering, physical
interaction, performance and broader Alpha gates remain separate.
[Source topology](ANIMATION_RETARGETING.md#inspect-the-original-sources),
[process facts](DEVELOPMENT_JOBS.md#process-and-cleanup-observations),
[results and scope](evidence/m2-source-topology-process-inspection.json).

Version 0.0.164 adds an optional installed CoreCLR authoring pack: matching
bridge/SDK, prebuilt component analyzer and binding generator, with the existing
NativeAOT publisher. The default thirteen-payload v1 pack remains available;
v2 admits seventeen payloads and verifies all three SDK copies. Actual Windows
installed compilation, structured CS1525 diagnostics, repair, compatible reload,
compiled component/Control rollback and complete warm/fresh-process save
continuation pass 697 calls and seventeen complete version-7 checkpoints while
the engine checkout pathname is unavailable. Its external supervisor observes
seventeen processes and closes empty without forcing cleanup; the same checkout
directory is restored. Independent file checks
join the original requests, native replies, saved bytes, compiler images and
whole-document comparisons. A separate full NativeAOT route using v2 passes
865 calls, eighteen checkpoints and relocated readonly consumption, with all
thirty-two processes observed by external supervisors that close empty without
forcing cleanup; this run excludes the test-created
source project during bundle consumption, not the engine checkout. Six Windows native suites and eleven Linux headless
CTest entries pass; portable pack checks pass on both systems, with one Windows
symlink case skipped. No runtime API/schema or native ABI changes are introduced.
Clean-machine installation, new timing/rendering and the broader Alpha gates
remain separate. [Installed workflow](AUTHORING_TOOLS.md),
[results and scope](evidence/m2-installed-coreclr-authoring.json).

Version 0.0.163 fixes deferred remote selection in the desktop editor. Authored and live-component drafts retain their original targets, fields and guards until resolution; Apply, Reload and completed edit-back reconcile the latest remote choice, including clear or deleted targets. Read failures retain drafts, expose Reload recovery and block Scene manipulation; automatic field-callback errors cannot unwind the GUI or recursively retry. All 34 production-linked model scenarios and 302 assertions pass on Linux and native Windows using the same managed image. Existing component/save regressions pass, and the complete Windows-targeted editor compiles. Four checks using the actual compiled EditorModel, ComponentEditorModel and NativeHost over the installed 0.0.162 native bridge pass 53 assertions, including complete authored-document/history preservation and original-target Apply receipts. No window, rendering, focus or input is used; displayed interaction and docking remain separate Alpha work. [Editor behavior](DESKTOP_EDITOR.md#current-authoring-controls), [results and scope](evidence/m2-editor-selection-reconciliation.json).

Version 0.0.162 adds a relocatable [NativeAOT authoring pack](AUTHORING_TOOLS.md): binary C# SDK, prebuilt component analyzer and native binding generator, Entry source and publisher. Native profiles pin its manifest and complete inventory, reject changed queued packs and revalidate after publication; omitted packs preserve the checkout workflow and historical fingerprints. Actual Windows publication, adoption, native callbacks, full saves and relocated export pass 894 calls and eighteen complete checkpoints while the engine checkout pathname is unavailable. All 32 processes close naturally; the same checkout directory is restored with the selected input fences unchanged. Portable checks pass on Windows/Linux; native policy coverage combines six original Windows suites, a successor fixture and six original Linux tests plus the corrected fixture under the unchanged 60-second limit. The initial Linux timeout, missing C# fixture callback and auditor banner assumption remain separate failures. The fixture-only rebuild leaves all production binaries exact. CoreCLR pack distribution, clean-machine installation, rendering and broader Alpha qualification remain separate. [Results and scope](evidence/m2-installed-authoring-tools.json).

Version 0.0.161 imports separate opaque grayscale FBX roughness and metalness maps into the existing linear material texture, preserving independent scalar factors. Seven Windows native executions and eighteen protocol tests pass; a fresh Linux headless build passes fourteen CTest entries, including all nine legacy FBX protocol methods and three new scalar-map groups. Exact packed pixels/mips, image reuse, analytic animation, atomic rejection, same-source repair and source-independent cooked reopening are checked. Comparing native 0.0.158 with 0.0.161 preserves all 26 accepted cases across 12 byte-identical packages and all 58 complete decoded errors. The optional original-character retarget case is skipped on Linux and exercised on Windows; two legacy filesystem methods remain unexecuted on Windows. General exporter compatibility, new rendering and broader Alpha gates remain separate. [Supported profile](FBX_IMPORT.md#roughness-and-metallic-textures), [results and scope](evidence/m2-fbx-scalar-maps.json).

Version 0.0.160 adds opt-in [process-memory query timing and phase association](MEMORY_TIMELINE.md) to the reliability tooling. Exact integer-clock intervals, observed process identities, conservative boundary handling, retained partial failures and bounded reports have portable contract checks. An actual native Windows self-process probe passes seven OS counter queries: five are assigned and two deliberately cross phase boundaries. The integrated soak recording, its representative overhead and the cause of retained game-memory growth still need native game qualification. Normal sampler reports and soak checks retain their default path. No engine or GPU workload is run for this tooling checkpoint. [Checks and scope](evidence/m2-memory-timeline.json).

Version 0.0.159 qualifies twenty encoded PNG texture edits on the full Performance Yard at **0.854 s mean and 0.909 s p95**, meeting the selected one-second target on Windows/NVIDIA. Each interval includes actual PNG read/decode, mip cooking, cached-package validation, authoring, live adoption, matching presentation observation and a fresh verified capture. Expected authoring-byte preparation is separate; preparation through verification reaches **1.426 s p95**. The cohort uses native 0.0.158 and the retained 0.0.94 Native AOT game. All 21 captures match the four actual reference states exactly. Complete version-7 saves retain all 14 payload keys, both cameras and all 50 gameplay values except the two legitimate adopted-content identity fields; original scalar materials and omitted inherited maps remain exact. A separate missing-package attempt preserves the entire previous snapshot and succeeds on retry after exact restoration. Independent audits join every raw request, save, capture, mask and input identity, and separately verify original PNG pixels against cooked base mips. This is one paused 1080p workload; cold-start latency, other devices, advancing-game edits, foreground/physical interaction and broader Alpha gates remain open. [Measurement boundaries](LIVE_TEXTURES.md#representative-encoded-texture-iteration), [samples and receipts](evidence/m2-live-texture-iteration.json).

Version 0.0.158 reserves a bounded full-frame allocation for exclusive BMP serialization, retaining SDL's encoding, exclusive creation and file flush. Seven Windows native executions and three cooperative Vulkan window groups pass. A fresh native 0.0.158 Performance Yard cohort changes all 24 character materials twenty times at **0.574 s mean and 0.660 s p95**, meeting the selected one-second scalar-edit target. The full workload retains 2,394 authored entities, 768 props, 64 lights, 32 dynamic bodies, medium GTAO and the retained 0.0.94 Native AOT game. Twenty-one 1080p captures match the earlier native reference bytes exactly; complete version-7 save payloads retain both cameras and all 50 gameplay values, apart from the two legitimate material-baseline identity fields. Expected authoring bytes prepare outside each timed interval and have separate timing records. The earlier 2.304 s p95 cohort remains a missed budget; both the engine allocation and verifier changed, so this is not an isolated causal end-to-end comparison. Encoded texture timing, other devices, foreground interaction and the broader Alpha gates remain open. [Measurement boundaries](LIVE_MATERIALS.md#representative-scalar-iteration), [samples and receipts](evidence/m2-live-material-iteration.json).

Version 0.0.157 records a fresh paused Windows CoreCLR Relay Yard cohort using the qualified native 0.0.156 engine and retained 0.0.88 managed tooling. Twenty real source edits and 25 compiler jobs pass at **1.959 s mean and 2.206 s p95**, meeting the selected three-second warm edit-to-first-verified-Tick budget. Every reload preserves complete typed version-7 native state, both cameras and all 50 gameplay values; changed three/four-metre behavior is verified on the first updated tick. A separate six-edit pilot qualifies paging and real stale guards. Independent audits join compiler requests/images, all journal records, saves and captures; the measured Job's 78 processes close naturally. This independent cohort does not establish a causal comparison with historical 0.0.108, cold builds, new SDK/Native AOT support or the broader Alpha gates. [Workflow and measurement boundaries](RELAY_YARD_ITERATION.md), [samples and receipts](evidence/m2-relay-yard-iteration-156.json).

Version 0.0.156 lets incoming shared-CLI requests interrupt voluntary idle-player waits, while preserving owner maintenance, capture invalidation and paced presentation. Seven Windows native executions, three actual SDL/Vulkan cooperative-window groups and eight Linux headless CTest entries pass. One matched paused 640×480 UI fixture on the RTX 4070 Laptop GPU reduces median world/player/gameplay inspection latency from 15–16 ms to 0.20–0.68 ms across 128 requests per method. Complete saved payloads remain unchanged around observation and restore exactly on same-window Load. Read-phase frame counts differ; the separate quiet sample uses more host CPU, so no idle-power or representative rendering improvement is claimed. The first driver attempt remains failed and excluded. The three-second 20-edit target and broader Alpha gates remain open. [Scheduling and reproduction](LIVE_PLAYER.md#shared-idle-scheduling), [measurements and scope](evidence/m2-cooperative-player-poll.json).

Version 0.0.155 identifies contained external FBX texture-read failures through versioned import and source-inspection errors: relative dependency, reason, known size, effective byte limit and donor index. Seven Windows native executions, seven selected Windows FBX tests and all eight retarget tests pass. A fresh Linux headless build passes eleven CTest entries, including all nine FBX protocol tests; its optional original-character retarget case is skipped and is exercised separately on Windows. Each platform verifies 36 typed dependency errors, unchanged same-owner authoring/history and packages on rejection, same-request repair, and cooked-model reopening after source removal. Two filesystem methods remain explicitly unexecuted on Windows. The short-read/open-failure branch is source-reviewed only. Existing path, image and material admission remains unchanged; no new rendering, performance or Alpha qualification is implied. [Repair guide](FBX_IMPORT.md#repair-an-external-texture-dependency), [results and scope](evidence/m2-fbx-texture-dependencies.json).

Version 0.0.154 adds opt-in GPU-memory observation recording to the existing Performance Yard reliability harness. It consumes already-returned player reports, keeps bounded owner/device/cache and terminal observations, and reports telemetry errors/omissions separately from functional gameplay. The workload, focus, timing and save predicates are unchanged. Portable collector/integration checks and offline retained 0.0.152 report ingestion cover the new recording path; no new representative performance, foreground acquisition, lifetime/peak VRAM or Alpha qualification is implied. Recording overhead and a genuine representative run remain open. [Guide](PERFORMANCE_YARD_RELIABILITY.md#optional-gpu-memory-recording), [checks and scope](evidence/m2-gpu-memory-observations.json).

Version 0.0.153 qualifies one fresh Codex management workflow, Beacon Foundry, using the supplied SDK 0.0.150 and native 0.0.149 cohort. The original run completes two publications and two exports with natural process closure. Independent source and actual relocated readonly campaigns earn separate V1 saves, verify complete restoration and an explicit V2 global-field upgrade, then complete warm and cold newly spawned work. Together with three paused Vulkan captures and UI/pixel review, 6,476 independent RPCs pass their declared checks. Queue, progress, allocator, strong-reference and all-global callback forecasts retain their stated scope. All three selected Alpha game families and both agent clients now have bounded evidence; broader Alpha gates remain open. Earlier failed management workflows remain failed. Public-source rebuild, physical input, representative performance and clean-machine installation are separate; capture/UI frame IDs differ and only unchanged committed state is joined. [Original source](evidence/fixtures/agent-beacon-foundry/README.md), [results and limits](evidence/m2-agent-beacon-foundry.json).

Version 0.0.152 adds experimental GPU heap allocation estimates and dynamic budgets to player/capture diagnostics, CLI smoke reports and desktop viewport reports. Four paused native players on the laptop’s AMD and NVIDIA devices each produce five usable cached samples; four pairs of saved payloads preserve complete state byte-for-byte across observations, stopping and separate capture. Device identity, cadence, sample immutability, native receipts, raw checksums and frozen terminal reports are checked. Four World captures, two CLI smoke renders, seven Windows native suites and five Linux headless tests pass. The Linux rendering-disabled CLI reports unknown data. A failed graphics-ownership request in the first verifier attempt remains retained. Actual unsupported-device behavior, embedded editor sampling, residency/peak memory and representative performance remain separate. [Guide](GPU_MEMORY.md), [results and scope](evidence/m2-gpu-memory.json).

Version 0.0.151 gives newly exported Windows launchers a per-user save directory keyed by the verified project ID. Explicit roots take precedence; direct and service launches retain explicit storage setup. Actual generated launchers save, restore and reopen after relocation using the retained Native AOT SaveProbe. A separate readonly owner verifies complete checkpoint restoration before callbacks and the subsequent 24-global forecast. Six Windows native suites, the focused 50-command/20-call CLI regression and seven Linux headless tests pass. The Python wheel passes isolated imports on both operating systems. Invalid default paths and an actual NTFS junction are rejected. The original profile-cache assertion and source-drift qualification failures remain retained. Save-image compatibility, automatic game-level loading, physical input, performance and the broader Alpha gates remain separate. [Storage setup and reproduction](GAMEPLAY_SAVES.md#configure-storage), [evidence](evidence/m2-default-launcher-saves.json).

Version 0.0.150 exposes read-only `Get<T>`, `TryGet<T>`, `Query<T>` and `IsAlive` on C# `ControlContext`, using the existing native component services. Compiled controls can inspect committed queues and inventory before accepting an action, without advancing simulation. A fresh Windows Native AOT fixture checks all five scalar and bounded-array kinds, complete queries, local-copy isolation, same-tick edits, immediate full-queue refusal and failed-callback rollback. This is an SDK addition over the qualified native 0.0.149 backend, with no services ABI change. The original management workflow remains failed after a separately confirmed deferred-refusal bug in its game code; Alpha management breadth remains open. [API](CUSTOM_COMPONENTS.md#reads-in-compiled-controls), [fixture](../tests/control_component_reads_gameplay/README.md), [evidence and limits](evidence/m2-control-component-reads.json).

Version 0.0.149 releases [runtime game-session v1](RUNTIME_GAME_SESSION_V1.md): 19 selected runtime/state/spatial/UI/save methods in three Windows x86_64 contexts, with explicit backend/target availability and frozen request/result artifacts. The packaged Python validator is independent of the existing prospective 21-method surface. All 174 portable tests pass on each of Linux and Windows; six native Windows suites and three actual schema99 discovery scopes pass. The unchanged scalar and collection consumers pass 742 and 1,299 calls against the new source and legitimately exported executables, retaining their original compiled images and cold-save checks. A separate native146 fixture passes 873 calls covering defaults/paging, compiled eligibility, literal consent, acknowledged recovery and durable cross-owner retry. Earlier retained games supply additional result/behavior evidence. Failed preflights and stale test assumptions are retained. Bootstrap, arbitrary upgrades, SDK/ABI, other native targets, physical input and broader Alpha/performance qualification remain separate. [Evidence and exact scope](evidence/m2-runtime-game-session-v1.json).

Version 0.0.148 records a fresh Claude-authored puzzle/adventure workflow using native 0.0.146 and SDK/reference checkpoint 0.0.147. Pump House reaches publication, adoption and Windows export in one original session: one publication, two exports, 26 script attempts including six failures, and 33 naturally closed native invocations. Independent source and actual relocated bundled services pass 3,880 native calls, including 3,825 selected candidate-validated successes. Eight comparisons preserve the complete version-7 native snapshot/document and selected entity/UI/registry/schema/value state before restored callbacks. Four subsequent checks predict all 25 gameplay globals from actual native owner metadata and camera rays. Real controller input verifies pickups, rejected prerequisites, moving collision bodies, bridge traversal and completion; cold Welcome Load and New Game preserve the old checkpoint. The original case is unavailable during exported execution and restored byte-for-byte afterward. Three independent 1280×720 Vulkan captures join logical UI at the same boundary; text is readable with mid-word wrapping and close framing. The interrupted first provider trial and failed pre-native verifier admission remain recorded. The default launcher needs an explicit external save root, portable source-record rebuild and interactive input remain unqualified, and the wider Alpha/runtime-contract/performance gates stay open. [Source record](evidence/fixtures/agent-pumphouse/README.md), [evidence](evidence/m2-agent-pumphouse.json).

Version 0.0.147 adds explicit recovery-consent checks to the opt-in result validator and a retained-game recovery/upgrade consumer. Thirty-four portable response tests pass on Linux and Windows. Using unchanged native 0.0.146 executables and genuine retained 0.0.85/0.0.86 Native AOT images, four naturally closed owners pass 694 calls, including 675 selected candidate-validated successes and seven expected errors. A legitimately written but test-owned corrupted generation 3 selects the original verified generation 2 at tick 567. An explicit plan preserves all 49 globals and defaults one new field; full native payload comparison before target callbacks permits only the approved gameplay change. Six denied loads preserve complete selected observations, save routing/queues and files. Historical retries preserve the live state with the plan absent. Actual controller/camera-ray Use collects the next cell and retires exactly one entity; a new healthy save restores fully and exactly in a fresh actual bundled executable. After callbacks, three owner-observation globals are checked against each owner's native epoch/restore generation; every other selected/native payload field remains exact. Legacy snapshot 5 reconstructs current snapshot 7 only through the checked frozen camera records and singleton instance maps. The failed camera-selection export and five failed verifier attempts remain recorded. No native code is changed or recompiled here; broader upgrades, rendering, performance, physical input and stable runtime/Alpha qualification remain separate. [Workflow](../examples/relay-yard/UPGRADING.md#focused-recovery-and-exported-game-consumer), [evidence](evidence/m2-retained-save-recovery-upgrade.json).

Version 0.0.146 fixes a stale save-generation request that could remain resolving and block gameplay after durable receipts rolled over. A first-invocation rejection explicitly observed before publication now completes as failed; uncertain publication still resolves its retained identity. The original native 0.0.142 diagnostic reproduces the defect at generation 33 with receipts 2–33. New Windows native 0.0.146 source and exported executables pass 2,355 calls, including 2,330 selected candidate-validated successes and twelve expected query errors. Missing loads and stale writes preserve stored files and the callback's specified committed changes; repeated queries/retries and same-owner/cold restoration compare complete selected observations. The unchanged scalar and collection consumers also pass against the new executables. Seven Windows native suites and seven Linux CTest selections pass; Linux simulation is disabled. The background Windows World suite uses a separately published current-SDK Native AOT SaveProbe, while the source/export consumer retains the earlier ComponentGame image and its 0.0.142 descriptor. This does not qualify arbitrary unknown outcomes, corrupted-slot recovery, upgrades, full internal state, renderer/editor interaction or a broader stable API/ABI. [Reproduce](COMPONENT_GAME_WORKFLOW.md#save-failure-and-recovery), [evidence and retained attempts](evidence/m2-save-publication-rejection.json).

Version 0.0.145 adds an all-five-kind compiled collection consumer and a selected collection preparation profile. The fresh Native AOT module uses generated capacity-two int32/int64/float32/float64/entity buffers plus one scalar in a 256-byte native component. Source and actual bundled native 0.0.142 services pass 1,299 calls, with 1,260 selected candidate-validated successes, eighteen expected errors and ten complete selected source/export state pairs. Full, shortened, empty and refilled arrays preserve their wire types and order; historical edits return retained outcomes without reapplying state. Four compiled failures and fourteen invalid/stale edits preserve complete selected observations, save routing/queues and external save files. Two same-owner restores plus cold restoration and continuation pass exact selected comparisons. The original scalar consumer also passes all 742 calls after the preparation extension. Publication, preparation and both consumers close naturally; the failed pre-compiler path-selection attempt remains recorded. This qualifies capacity-two readonly collection consumption, not generated Clear/RemoveAt/full-buffer operation coverage, hidden wire padding, arbitrary capacities, migration, complete internal snapshots or a stable runtime/Alpha release. [Workflow](COMPONENT_GAME_WORKFLOW.md#bounded-collection-profile), [results](evidence/m2-collection-workflow.json).

Version 0.0.144 qualifies a fresh current-SDK Native AOT ComponentGame through source authoring and its actual exported readonly executable, using the retained native 0.0.142 runtime. The public preparation script creates the project, imports Health/Interaction schemas, authors their instances, adopts compiled gameplay and exports without changing its inputs. Three native owners make 742 calls; 715 selected successful results pass the opt-in candidate validator, and twelve expected native errors are retained. Three pre-ticket source/export pairs, two same-owner save/load pairs and one cold readonly restore pair preserve complete selected typed observations. Four duplicate-write/later-tick exception checks preserve those observations, save routing/queues and external save files. Random ticket globals remain present; the cold owner's expired-ticket transition is checked explicitly. Fresh publication, preparation and gameplay Jobs close naturally. This covers scalar readonly components and completed/expired tickets; readonly arrays, resolving/failed tickets, full internal snapshots, broader error/ABI stability and Alpha release gates remain separate. [Reproduce](COMPONENT_GAME_WORKFLOW.md), [evidence](evidence/m2-compiled-component-workflow.json).

Version 0.0.143 adds opt-in Python successful-result checks for the 21 selected runtime/component/UI/save methods. Thirty-one new consumer tests and the unchanged nineteen authoring-response and seventeen observation tests pass on Linux and Windows. Independent offline validation checks 65,293 selected successful replies: 65,132 from sixteen retained native 0.0.134/0.0.141 traces and 161 from four focused native 0.0.142 captures. The focused capture joins 185 original request/reply records across source standalone/shared and readonly game scopes, including nineteen expected errors, stopped and active save restoration, typed component pagination, exact selectors and historical retries. All seven observed Windows Job processes close naturally. Rejected-read/selector checks preserve authored bytes and the selected runtime/gameplay/registry/UI/save-configuration observations; this is not a complete physics/component-instance/storage equality claim. Canonical component schemas and preserved embedded gameplay declarations are distinct, including valid optional metadata and declaration order. Nonempty readonly components, queued/resolving/failed tickets, recovered/upgraded saves and broader error/behavior/ABI compatibility remain separate gates; no broader stable API or Alpha qualification is released. [Result contract](WORKFLOW_RESPONSE_CONTRACT.md), [evidence and scope](evidence/m2-workflow-response-contract.json).

Version 0.0.142 adds a prospective 21-method runtime/component/UI/save request projection and comparison tool, with complete schema nodes, exact non-schema context and fail-closed reference scope. Thirty-six portable tests pass on Linux and Windows. The native gameplay path now validates integer tick/revision tokens before receipt lookup: retained Native AOT and current-SDK CoreCLR checks reject float-equal historical guards while valid typed retries preserve their results. The final CoreCLR component contract passes 463 RPCs and ten groups, including 100 collectible reloads, rollback, durable saves and fresh-process continuation. Six Windows native suites pass; the final fixture reseal preserves all tested compiled artifacts byte-for-byte. A genuine matched runtime/export passes whole typed-world freeze checks, and all three Windows discovery scopes pass without starting a player. The initial missing-architecture test environment and mismatched-export discovery attempts remain recorded failures. No Linux-native 0.0.142, broader stable gameplay/response/ABI contract, renderer, performance or Alpha qualification is claimed. [Request contract](WORKFLOW_API_COMPATIBILITY.md), [results and limits](evidence/m2-workflow-request-compatibility.json).

Version 0.0.141 adds optional authored shadow-filter selection, explicit capture/player overrides and new-project fractional-filter defaults. Legacy omitted fields retain point filtering. Rendering resolves the authored choice at each scene extraction, while runtime environments remain frozen. Six Windows native suites, ten lighting tests and thirteen project/export tests pass. Linux passes four non-renderer suites plus authoring, lighting and project contracts, with four documented simulation/runtime skips. Twenty-five NVIDIA and ten AMD controlled character captures pass, with ten exact-pixel comparisons; actual inspection shows smoother edges and improved contact in these fixed views. The forward shading delta is 0.005584 ms in this small fixture; deferred measurements remain noisy and inconclusive. The retained compiled game passes thirteen source and thirteen exported-game groups with full checkpoint continuation; the module is unchanged. Seven separate NVIDIA native-presentation captures pass across three paused player generations and one same-HWND hosted viewport: inherited and explicit filters, frozen runtime images after authoring edits, and exact authored restoration by explicit transactions. Avalonia, physical input, broader shadow quality/cost, caching, GI and RTAO remain separate work. [Contract](SHADOWS.md#filter-selection), [evidence](evidence/m2-shadow-environment-preset.json).

Version 0.0.140 adds read-only Python `cleanup_error` diagnostics to `WorldClient` and `JsonRpcProcess`. An active body exception propagates unchanged while callers retain access to the latest close `TransportError`; successful closes do not erase that history. All 49 client contract tests pass on each of Linux and Windows with native 0.0.134, including thirteen cleanup tests; the Windows Job closes naturally with no forced cleanup or source drift. [API and recipe](PYTHON_CLIENT.md), [evidence and limits](evidence/m2-python-cleanup-diagnostics.json).

Version 0.0.139 records a second bounded fresh Switchback workflow: its original supervisor passes with 36 native invocations and 31 naturally closed script attempts, including nine retained failed attempts. Independent source and relocated readonly game checks pass thirteen groups with complete checkpoint continuation; separate native Vulkan observations pass with declared original paths unavailable. This uses native 0.0.134, Python client/reference source 0.0.138 and the retained managed SDK 0.0.127. Earlier failed workflow verdicts remain unchanged; broader Alpha, physical-input and representative-performance gates remain open. [Evidence and limits](evidence/m2-agent-switchback-fresh-repeat.json).

Version 0.0.138 adds explicit Python `shutdown_host()` with native empty parameters and an [owned-host cleanup recipe](PYTHON_CLIENT.md#explicitly-close-a-host-you-own). Nineteen client/lifecycle tests pass on each of Linux and Windows using native 0.0.134. Attached-client close still detaches, and shutdown acknowledgement does not imply observed process exit. [Evidence and scope](evidence/m2-python-host-shutdown.json).

Version 0.0.137 preserves [another independently authored Switchback game](evidence/fixtures/agent-switchback-repeat/README.md). Its source and relocated readonly service each pass thirteen groups, nine complete checkpoints and five typed-exact save comparisons; separate Ready/Idle/Run Vulkan observations pass with the original project paths unavailable. Both source trees restore byte-for-byte. The original agent workflow remains failed after forced script cleanup and missing historical owner proof. The repeated-workflow Alpha gate stays open. [Results, native version and limits](evidence/m2-agent-switchback-repeat.json).

Version 0.0.136 adds [retired build storage maintenance](BUILD_STORAGE.md): explicit root selection, read-only reports, reviewed plans and guarded removal of proven compiler objects/PCH. Qualification passes 24 temporary-filesystem tests on Linux and 21 on Windows; three Windows symlink tests are skipped for unavailable creation privileges and pass on Linux. Retirement with no live writers or consumers is required; the helper does not detect or contain processes. [Evidence and limits](evidence/m2-build-storage.json).

Version 0.0.135 qualifies rebuilding the published fresh Switchback C# and project without changes, using copied current SDK/publisher sources and the native 0.0.134 runtime. The new Native AOT module passes all thirteen independent groups and 21,990 RPCs through four naturally closed owners, nine complete version-9 checkpoints and five typed-exact state comparisons. Preserve the failed environment diagnostic and rejected non-clean SDK preflight separately. Qualification profiles now retain the host processor architecture; actual isolated `.NET --info` succeeds. This closes the portable-source rebuild gap, while repeated fresh workflows, export/rendering of this new module and the remaining Alpha gates stay open. [Evidence](evidence/m2-agent-switchback-fresh-rebuild.json).

Version 0.0.134 adds [cached player readiness and pause conditions](LIVE_PLAYER.md#readiness-and-pause-conditions), including nullable platform observations, terminal retention and coherent historical command receipts. It updates the stale first-poll report detail without changing playback or focus policy. Five Windows native suites and three Linux non-rendering suites pass; both platforms pass two authoring-core identity tests and report the compiled 0.0.134 version. The AMD compiled UI/save-load workflow passes 353 RPCs; NVIDIA replay passes 32 genuinely advancing observations while its cached input-focus gate and interactive activity remain false. Interactive observations remain paused; physical input, real focus/minimize transitions and observation cost at scale remain unqualified. [Evidence](evidence/m2-player-readiness.json).

Version 0.0.133 improves [Python terminal-error diagnostics](PYTHON_CLIENT.md#failure-and-deliberate-recovery). `RpcError` retains the correlated method, RPC ID and original bounded request context while preserving native errors and existing exception formatting. It performs no automatic retry, schema lookup or parameter rewriting. All 32 client tests pass against native 0.0.132 on Windows and Linux, including real peer/native error context and deliberate recovery; the SDK wheel passes exact-content and isolated native checks. [Evidence](evidence/m2-python-rpc-diagnostics.json).

Version 0.0.132 adds [displayed game UI inspection](PLAYER_UI_OBSERVATION.md): immutable native bounds, clipping, computed font size and sampled hittability tagged with the actual presented frame. Exact player/session/generation guards remain required; optional Tick/UI guards allow live reads without a prior-read race. Native Windows UI/discovery/MCP checks and Linux non-rendering discovery/MCP/version checks pass. The actual AMD player passes 351 RPCs through compiled controls, UI edits and save/load; NVIDIA replay passes 32 genuinely advancing UI observations with compiled Tick-driven text. Focused interactive observation, physical input, atomic image/layout joins and observation cost at scale remain separate gaps. [Evidence](evidence/m2-player-ui-observation.json).

Version 0.0.131 adds a [fresh agent-authored Switchback implementation](evidence/fixtures/agent-switchback-fresh/README.md). Its original supervisor passes with eighteen repaired script attempts in one provider session. Independent native gameplay passes thirteen groups and 21,990 RPCs through four naturally closed owners, including nine full version-9 checkpoints and five typed-exact state pairs. The exported service passes the same groups and Ready/Idle/Run Vulkan observations while the original project is unavailable. Earlier workflow, presentation-assertion and renderer-plan failures remain recorded; the candidate gameplay source is unchanged. This qualifies one bounded fresh workflow using the retained 0.0.128 engine, not a native 0.0.131 build. The portable source rebuild is qualified separately in 0.0.135; the repeated fresh-workflow Alpha gate remains open. [Results](evidence/m2-agent-switchback-fresh.json).

Version 0.0.130 publishes the original [Switchback Range](evidence/fixtures/agent-switchback/README.md)
C# game, licensed cooked content and an independent native verifier. The portable
fixture rebuilds its SDK and Native AOT game, adopts the artifact and exports a
verified Windows bundle using the retained 0.0.128 engine with corrected runtime
notices. Thirteen gameplay groups pass across four naturally closed owners,
including nine full version-9 checkpoints and five typed-exact state pairs.
Both original fresh-agent workflow verdicts remain failed; the corrected receipt
audit and independent game qualification are separate results. The repeated
fresh-workflow Alpha gate remains open. [Results](evidence/m2-agent-switchback.json).

The actual relocated `game serve` route passes the same thirteen groups through
four fresh host/client pairs with an unchanged bundle after owned build-source
paths are removed. Ready/Idle/Run Vulkan captures show real GPU skinning and
4× world/foreground MSAA. The full checkout remains installed; clean-machine,
physical-input, audio-listening and performance gates remain open.

Version 0.0.129 completes the identified SDL runtime-notice packaging gap.
Installed rendering runtimes include the original yuv2rgb BSD notice, the
selected HIDAPI BSD terms and an explicit selection note. Configuration checks
the pinned upstream notice bytes. Actual CMake installation and native Windows
export/inspection verify the added payloads; the Windows export check uses the
retained 0.0.128 runtime. [Recorded scope](evidence/m2-sdl-runtime-notices.json).

Version 0.0.128 adds [guarded project gameplay adoption](PROJECT_GAMEPLAY_ADOPTION.md).
The CLI and profile-scoped agent service verify and copy a published Native AOT
artifact, then update the manifest using exact project/artifact hashes and a
durable request receipt. Historical retries preserve later project edits and
can outlive the original artifact. Prepared recovery requires the recorded
manifest, world and input boundaries; torn request-owned staging is retained
before reconstruction. Adoption neither executes gameplay nor changes the
world or existing saves. MCP keeps it behind separate build consent. Schema
revision 95 exposes the project operations without changing the gameplay ABI.

Four selected Windows native suites and thirteen project regressions pass.
Five adoption groups cover 41 RPCs and seven native owners, including six
independently authored prepared/torn-file boundaries. Three supplemental
recovery cases pass eight RPCs. The retained Traversal Yard Native AOT artifact
runs on the new engine, completes its objective and verifies nine version-9
snapshots with five complete state pairs across two native owners and 519 RPCs.
One existing SDK error projection is recorded explicitly: absent optional
error data becomes null, with unchanged native code/message. The 62-file,
98,938,204-byte relocated export passes inspection with unchanged contents.

Windows adoption supports long staging paths. Directory publication now retries
selected Windows errors while preserving ownership, complete content checks and
no replacement. Controlled starter-project tests verify transient sharing
recovery, a held-handle ten-attempt failure and rejection of mutated staging.
Their observed error is Windows sharing violation 32; other admitted errors
were not independently induced. Completed native/adoption/game/recovery results
are revalidated from a closed earlier owner; its three export retry timing
misses remain excluded. A separate successful owner executes the publication
cases. Both qualification Jobs close naturally with no active or terminated descendants.
[Results](evidence/m2-project-gameplay-adoption.json).

These are bounded Windows handoff and continuation checks. Synthetic recovery
states do not qualify power-loss recovery, and relocated inspection does not
add rendered playback, physical-input or clean-machine coverage. Fresh agent
workflows, broader compatibility and Alpha acceptance remain open.

Version 0.0.127 adds [Traversal Yard](../examples/traversal-yard), a complete
small first-person C# course combining native steps, crouch clearance, a moving
lift, three beacon objectives, camera zoom, an imported animated courier and
durable checkpoints. CoreCLR and Native AOT each pass five groups through public
semantic inputs, with 702 and 708 RPCs respectively, two native owners and nine
selected version-9 snapshots. Four complete snapshot pairs match on each
backend; Native AOT also preserves exact state after denied module replacement.
CoreCLR compatible reload preserves state apart from the gameplay revision and
its digest. Continuation compares independently cold-restored owners.

Three AMD Vulkan captures use the authored sky, directional shadows, 4× MSAA
and fractional nine-tap PCF at 960×540. These checks qualify the sample's
scripted course and rendered checkpoints, rather than physical-input comfort,
AA/AAA visual quality or game-scale performance. The existing locomotion and
query profiles remain unchanged; there is no new gameplay ABI.

The installed runtime declaration now reports the current 336-byte epoch-7
services prefix and its available gameplay features. A configuration-only
correction leaves compiled native and managed artifacts unchanged. Fifteen
descriptor regressions reject stale declarations and check enabled-feature
membership against the native contract. Shipping symbol removal preserves
loadable section bytes and loader semantics, including retained crash IDs.

The relocated Windows Native AOT bundle passes six commands and 564 RPCs
across authoring and cooked-world checks. With original owned source paths
absent, it presents 1,401 frames over 1,400 replay ticks on NVIDIA, then completes
the course through semantic inputs in a separate cooked-world run. Nine saves
and five complete snapshot pairs verify UI Save/Load and cold continuation.
Four 960×540, 4× MSAA captures have no reported NVRHI errors. The 62-file,
98,568,923-byte closure remains unchanged and meets existing export limits.
All owned processes exit naturally. These are scripted single-machine checks;
physical devices, clean-machine installation and Alpha acceptance remain open.
[Results](evidence/m2-traversal-yard.json).

Version 0.0.126 adds optional [native character locomotion](CHARACTER_LOCOMOTION.md)
for steps, walkable slopes, floor following and supported kinematic platforms.
The frozen profile selects a Jolt CharacterVirtual motor with a separately owned
query capsule; omission retains the physical-character backend. Reported
post-tick velocity measures actual foot displacement, while integration intent
remains separate. Obstruction-safe stance and planar root-motion ownership
continue through the existing controls, without a new C# service extension.
Schema revision 94 exposes the configured profile and cached ground vectors.
Strict version-9 snapshots preserve motor pose and both velocities, install
saved stance shapes first and reconstruct support after all body poses.

Twenty-six native CPU suites pass, including nine locomotion groups. Three
four-group protocols pass 191 RPCs across six clean native processes. The
locomotion protocol checks authored steps, slope/height rejection, a separate
third-tread ray, platform transport, retries, frozen edits, complete rejection
rollback and exact 20-tick continuation between cold-restored owners. Compiled
CoreCLR and Native AOT consumers pass nine groups and 246 RPCs across four clean
processes, with complete state rollback, actual proxy queries, staged stance,
durable save intent and cold continuation. CoreCLR also qualifies compatible
reload. Ninety-two SDK ABI checks and sixteen publishing tests pass.
[Evidence](evidence/m2-character-locomotion.json).

These are synthetic geometry and compiled-control checks. They do not qualify
physical-input feel, animated foot placement, arbitrary crowded/dynamic support,
swimming, new rendering behavior or production performance. A rounded capsule
can remain supported by a steep tread-edge contact during descent; this does
not make that contact walkable. Portable continuation compares independently
cold-restored owners and does not preserve Jolt's internal warm contact caches.
This checkpoint does not establish Alpha readiness.

Version 0.0.125 adds [optional character stance](CHARACTER_STANCE.md).
Configured controllers shrink at their foot origin, use their authored crouched
speed and eye drop, and test native collision shapes before standing. A blocked
stand retains crouched posture and retries without another command. The profile
stays frozen through live authoring; camera-free actors use the same clearance
policy. Schema revision 93 adds guarded inspection and atomic stance commands.
The independent `character_stance_v1` extension appends a 336-byte epoch-7
services prefix; earlier declared profiles retain their bounded views.

Twenty-five native CPU suites pass, including seven stance groups and four
protocol groups with 62 native RPCs and two clean processes. Compiled SDK tests
pass 92 ABI checks and 16 publishing tests. Actual CoreCLR and Native AOT
consumers pass 523 RPCs across six clean processes, with exact quotas, staged
reads, pending births, complete rollback and cold save continuation. CoreCLR
also qualifies compatible reload. Forty AMD/NVIDIA forward/deferred Vulkan
captures match twenty independent authored-camera references pixel for pixel;
the eye drop visibly changes the synthetic course. Version-8 snapshots restore
the saved capsule before body state and preserve complete posture intent.
[Evidence](evidence/m2-character-stance.json).

These checks cover scripted crouch/stand, fixed-tick physics and standalone
320x240 rendering. Stairs, slope handling, moving platforms, swimming, physical
input and animated body crouching remain separate qualification work. Durable
continuation compares independently cold-restored owners; it does not serialize
Jolt's warm contact caches. This checkpoint does not establish Alpha readiness.

Version 0.0.124 adds opt-in `lens_interpolation` for interactive native players.
World and first-person FOV interpolate in logarithmic perspective scale between
the final two consecutive committed ticks; gameplay easing and saved lenses
remain authoritative. Camera pose, input and simulation are not interpolated.
This presentation policy adds up to one fixed tick of FOV lag. Ownership,
clipping, preference and discontinuity boundaries reset the pair. Default-off
output retains its existing metadata shape, and replay rejects interpolation.
Schema revision 92 also exposes `player.capture_current`: the native owner
atomically selects and returns a captured tick and full committed camera lenses,
then renders the exact immutable snapshot. The existing exact-tick capture API
retains its required tick guard. Both operations preserve identity, control,
session and applicable material/structure/UI guards and their original retries.
Twenty-four Windows native CPU suites pass, including six interpolation groups,
capture lifetime/ownership and strict discovery/admission checks. Four live
Native AOT cohorts exercise AMD/NVIDIA forward/deferred rendering at 320x240;
48 player/reference images form 24 exact reference pairs, including eight
unpaused fractional zoom/release captures. Each cohort checks seven rejected
current-capture guards without output or runtime changes. The unchanged,
qualified 0.0.123 gameplay artifact is consumed without a new SDK compilation;
the epoch-7 services prefix remains unchanged. Earlier failed observation
recipes are retained and excluded from these results. Physical ADS input,
camera-pose interpolation, continuous reconstruction history and game-scale
performance remain separate qualification.
[Contract](PLAYER_LENS_PRESENTATION.md),
[results](evidence/m2-player-lens-presentation.json).

Version 0.0.123 adds `CameraFovTransition.Evaluate` for authoritative fixed-tick
ADS easing. The stateless C# helper interpolates logarithmic perspective
magnification with smoothstep; endpoints and zero-duration snaps remain exact.
Caller-owned primitive phase fields survive saves and compatible reload.
Interrupted transitions start from the current committed lens, preserving
position continuity, with no velocity-continuity claim. The existing
`camera_lens_v1` marker, epoch-7 services prefix and schema revision 91 are
unchanged. Guarded lens getters are now available while the interactive player
runs; direct runtime mutation still requires pausing.
Twenty-two Windows native suites, 88 compiled SDK/ABI checks and 15 publisher
tests pass. CoreCLR and Native AOT consumers pass 176 and 165 native calls,
including independent curve samples, complete later-tick rollback, inert
mid-transition inspection and exact cold-restored continuation. CoreCLR also
preserves mid-transition phase through compatible reload. Actual interactive
windows on AMD and NVIDIA exercise running getters and blocked direct mutations;
32 forward/deferred captures match 16 independently authored reference pairs
exactly. The running transitions use scripted compiled gameplay; image captures
are paused checkpoints. Physical ADS input, rendering between ticks,
velocity-continuous reversals, continuous FSR history and game-scale performance
remain separate qualification.
[Contract](CAMERA_LENSES.md#fixed-tick-ads-easing),
[results](evidence/m2-camera-zoom.json).

Version 0.0.122 adds fixed-tick world and first-person lens controls for
ADS and zoom through schema revision 91 and the independent C#
`camera_lens_v1` extension. Complete FOV/clipping channels persist through
rollback, camera births, compatible CoreCLR reload and fresh saves. Camera
presence on each entity and first-person lens presence remain frozen; configured
instance births and retirements can change live camera membership. The 320-byte epoch-7
prefix preserves earlier profiles and their permissions; reads are bounded
at 256 per callback and writes at 64 unique combined targets per tick.
Player FOV preferences remain the neutral baseline, composed with gameplay
zoom by perspective magnification; first-person projection stays independent.
Camera-bearing snapshots use version 7, while camera-free worlds retain their
previous formats. Earlier Camera snapshots restore authored lenses.
Twenty-two Windows native suites, four API groups with 62 lossless calls,
83 compiled ABI checks and 15 publisher tests pass. Actual CoreCLR and Native
AOT consumers pass 216 and 205 native calls respectively, including complete
batch rollback and fresh-process reconstructed-state continuation. Sixteen
320x240 captures on AMD/NVIDIA forward/deferred paths match the independent
authored target lenses exactly, including eight live/reference pairs and four
world/foreground independence pairs. Renderer receipts identify the effective
world and foreground lenses. All accepted Windows Jobs close naturally.
Retained test-fixture corrections preserve strict version-7 validation and
genuine older-format coverage; failed attempts are excluded from qualification.
These checks do not qualify continuous-window ADS, presentation interpolation,
temporal reconstruction, a weapon framework or game-scale performance.
[Contract](CAMERA_LENSES.md), [results](evidence/m2-camera-lenses.json).

Version 0.0.121 adds independent foreground MSAA at one, two, four or eight
samples. An explicit shader averages HDR color and coverage before foreground
composition, sharing the ordinary tone map while keeping world products
separate. Schema revision 90 exposes the option through capture/player routes;
native player/editor CLI launches accept `--viewmodel-samples`. Default one
sample aliases resolved color and omits the new pass. Exact image-format/sample
support and existing resident/staging budgets reject unsupported requests.
Nineteen Windows native suites, four API groups with 48 lossless calls and a
render-graph matrix of 349 valid variants/632 rejected mutations pass.
Ten accepted hardware cohorts record 54 actual 320x240 synthetic captures on
AMD and NVIDIA. Four-sample foreground works with forward, deferred and FSR
Quality world rendering: each case has 465 partial edge pixels across all three
quarter-coverage classes, within one display RGB level of the independent
linear-HDR reference. Raw world radiance stays analytic; complete world products
match across foreground/empty/reference cases. Twenty-four default full BMPs
and their world probes match retained 0.0.120 results exactly. Malformed counts
reject without changing runtime state, and empty foregrounds preserve pixels.
All accepted Windows Jobs close naturally with unchanged admitted inputs.
A retained initial NVIDIA FSR fixture run rejected an overly strict assumption
that reconstructed radiance must equal authored raw radiance: its output differs
by one half-float step. The fixture successor records that value separately;
raw-radiance, edge tolerances and exact world-product equality remain enforced.
Hardware image qualification covers foreground counts one and four; two/eight
have graph-contract coverage and require device checks when requested. These bounded captures do not
establish game-scale performance, temporal AA or physical input latency.
[Contract](VIEWMODELS.md), [results](evidence/m2-viewmodel-msaa.json).

Version 0.0.120 adds a separate frozen Camera viewmodel lens and explicit
CameraDraw bindings. Named Game cameras render bound opaque arms/weapons with
independent FOV and self-depth; camera-hidden bodies retain world shadows.
The output-resolution foreground uses its own cluster grid and composites in
linear HDR after world resolve/reconstruction, before the shared tone map.
World depth, normals, motion and reconstruction inputs remain world-only.
Exact scene receipts include selected lens, drawable IDs and incarnations.
Nineteen Windows native suites, nine camera-draw groups and four API groups
with 48 lossless calls pass, including graph rejection, invisible binding caps,
instance remapping and complete fresh save restoration. Twenty GPU cohorts
record 130 actual 320x240 synthetic captures on AMD/NVIDIA forward/deferred paths,
checking independent projection, weighted animation, other-camera omission,
world-product isolation, unchanged default output and foreground light-grid
parity/overflow. Explicit lighting recipes include world MSAA4 and FSR Quality.
Two paused NVIDIA player windows check scalar/texture adoption, retained retries
and world-FOV preferences against authored references and six complete durable
checkpoints each. All accepted Windows Jobs close naturally with unchanged
admitted inputs. Foreground stays opaque and single-sample, without temporal
AA or reconstruction; it receives available world-atlas shadows but casts
neither world shadows nor foreground self-shadows. Lens/membership changes,
physical weapon clearance, gameplay ADS and game-scale performance remain
separate work. Logical foreground resident-plus-staging payload is capped at
384 MiB, with existing aggregate renderer budgets also enforced. This is not
measured Vulkan heap use or allocation-failure qualification.
[Contract](VIEWMODELS.md), [results](evidence/m2-first-person-viewmodels.json).

Version 0.0.119 adds opt-in planar root motion from frozen original model
nodes, with Extract diagnostics or collision-resolved Character locomotion.
Loop-unwrapped intervals and transition velocity history advance once per
physics tick; composed poses remove planar motion before IK and skinning.
Mounted rigs account for pivot, yaw and uniform scale. Character movement
ownership, staged enable controls, instance remapping and complete batch
rollback share the native owner. Root worlds use nested animation-state v5;
IK-only and older worlds retain their previous formats. Independent
`animation_root_motion_v1` appends the 304-byte epoch-7 services prefix.
Sixteen Windows native suites, ten root-motion groups, three API groups with
89 calls, 80 compiled ABI checks and 14 publisher tests pass. Actual CoreCLR
and Native AOT consumers record 156 and 147 lossless calls, with 23 and 21
complete durable checkpoints, full rejection rollback, save intents and
three grounded ticks matching independently reconstructed saves. CoreCLR
also preserves the complete checkpoint across compatible reload. Four GPU
pairs on AMD/NVIDIA and forward/deferred paths match independent geometry
exactly in full BMP pixels and HDR/depth/normal probes. A retained earlier
run found a roughly 3.3e-8-metre Character Y difference between uninterrupted
warm-contact physics and a fresh restore: typed saves omit Jolt solver/contact
caches. Immediate restore is exact; bit-exact future warm-contact simulation
is outside this contract. Accepted Windows Jobs close naturally with unchanged
admitted inputs. Vertical climbing, stride warping, automatic contact/foot
planning, desktop root controls and game-scale performance remain separate
work. These checks do not qualify an unpaused player-service route or close
Alpha. [Contract](ROOT_MOTION.md), [results](evidence/m2-root-motion.json).

Version 0.0.118 adds explicit native two-bone IK after base animation and
masked layers, with world/rig targets, pole points, weights, reach clamping and
cached committed observations. Frozen original-index chains remap through
hierarchical instances; commands, skinning and attached hit regions share the
corrected pose and complete rollback. IK worlds save nested animation-state
version 4, while worlds without IK preserve earlier serialization. Independent
`animation_ik_v1` appends a 288-byte epoch-7 services prefix without changing
older prefixes. Fifteen Windows native suites, nine IK groups, three API groups
with 88 recorded calls, 79 compiled ABI checks and 13 publisher tests pass.
Actual CoreCLR and Native AOT consumers record 118 and 109 lossless calls,
checking read/write limits, staged controls, complete failure rollback, save
intents and exact fresh-process continuation; CoreCLR also checks compatible
reload. Eight synthetic GPU captures across AMD and NVIDIA adapters and both
forward/deferred paths match independently calculated static geometry exactly
in full BMP pixels and HDR/depth/normal probes. All accepted Windows Jobs close
naturally with unchanged admitted inputs. Automatic grounding, full-body IK,
joint limits, root motion, desktop IK controls and game-scale performance remain
separate work. These checks do not qualify an unpaused player-service route or
close Alpha. [Contract](TWO_BONE_IK.md), [results](evidence/m2-two-bone-ik.json).

Version 0.0.117 implements immutable query-only HitRegion spheres, boxes and
capsules on existing entities, including imported rig nodes and hierarchical
instances. Composite hit tests use current native poses and geometry, exact
affine shape intersections and deterministic tie rules. Enabled region owners
suppress only their own physics proxy in this query; existing raycasts and
collision response retain their behavior. The independent `hit_regions_v1`
gameplay extension uses a 272-byte services prefix in epoch 7 and permits up to
1,024 admitted read-only calls per Tick. Twelve Windows native suites, seven
hit-region groups, four API groups and 78 compiled ABI checks pass. Actual
CoreCLR and Native AOT consumers check committed/staged bone poses, quota and
complete failure rollback, save intents and exact fresh-process continuation;
CoreCLR also preserves the complete checkpoint across compatible reload.
The API fixture records 56 interpreted calls; compiled consumers record 197
lossless calls across four clean native owners. All accepted Windows Jobs close
naturally with unchanged admitted inputs. [Results](evidence/m2-hit-regions.json).
This does not provide automatic anatomical regions, damage rules, IK, network
lag compensation or a production weapon framework. [Contract and example](HIT_REGIONS.md).

Version 0.0.116 adds an optional explicit node map to
`reference-rotation-v1` for differently named imported character and animation
nodes. The 1–1,024 pairs use guarded original-model indices, cover target skin
and selected donor-channel ancestry, and preserve bijective direct-parent/root
correspondence. Omission retains named matching. Explicit output uses target
topology while preserving target geometry, defaults and inverse binds; selected
position deltas still use donor IDs. Five Windows native CPU suites, eleven
retarget groups and all eight protocol tests pass, including the original
licensed character case with no skips. The protocol records 1,017 calls and
ten clean owners; schema-85 discovery exposes the exact bounds. The accepted
Windows Job closes naturally with unchanged source and asset inputs.
This does not infer anatomy, collapse
helper bones, solve contacts or extract controller root motion. Broader
character intake and Alpha qualification remain open.
[Contract](ANIMATION_RETARGETING.md#map-differently-named-nodes),
[results](evidence/m2-animation-node-map.json).

Version 0.0.115 adds opt-in named digital actions for independent fire, aim,
reload and game-specific controls. Explicit v3 profiles hold up to 32 named
bindings; omission preserves v1/v2 bytes and defaults. Marked C# games read
Held, Pressed, Released and Present during Tick. Held state repeats through a
batch; edges arrive on its first tick, including both edges of a quick tap.
The independent `input_actions_v1` extension appends a 264-byte services prefix
in epoch 7 while preserving legacy input and older negotiated prefixes.
Ten Windows native CPU groups, 77 compiled ABI checks, 11 publisher tests and
both input-profile/gamepad protocol groups pass; the profile suite skips one
Windows symlink case requiring privileges. Actual CoreCLR and published Native
AOT gameplay pass 149 and 140 recorded RPCs respectively: raycast hits, ammo,
aiming state, reload, independent entity input, rejected records and callbacks,
complete later-tick rollback, exact save restoration and fresh continuation.
CoreCLR additionally passes compatible reload. Two hardware Vulkan replays on
one AMD GPU match the full headless checkpoint across forward/deferred paths.
All accepted owners close naturally; earlier harness and compiler-lifetime
failures remain excluded. Desktop forwarding is source-reviewed, without a new
physical-input/editor qualification. Contexts, chords, analog named values,
rebinding UI, weapon assets and input-latency qualification remain separate
work. This slice does not close the Alpha gate. [Contract](INPUT_ACTIONS.md),
[results](evidence/m2-input-actions.json).

Version 0.0.114 adds guarded live texture adoption on existing authored PBR
primitives, static meshes and skinned meshes. Five maps, sampler settings and
normal/occlusion strengths share the scalar material epoch; omitted maps inherit
the original model, explicit null disables them, and removal restores inheritance.
Only selected texture intent and precisely associated dependency/provenance
changes are admitted. Nine Windows native checks and schema-83 discovery pass.
On one AMD GPU, 72 standalone forward/deferred captures verify 28 adoptions,
paired authored/live pixels, HDR probes and independent channel controls. Two
continuous players add 34 captures and 24 replacement cycles, preserving their
runtime owners through paused and running edits. A retained CoreCLR game adopts
a newly imported texture at tick 140 with its full saved gameplay payload intact
except the authored/content baseline, completes at tick 777, and exact-restores
in a fresh process before five callbacks. Its 4,181 settled native rows remain
retained. Disabled-backend Linux syntax checks pass. The corrected initial
fixture-lock failure remains separate from successful qualification. Import,
closure validation and upload are synchronous; these checks do not qualify
iteration latency, peak VRAM, physical scanout, general live scene changes or
Alpha completion. [Contract](LIVE_TEXTURES.md),
[results](evidence/m2-live-textures.json).

Version 0.0.113 adds explicit guarded scalar material adoption on existing
authored drawables with `PbrMaterial`. Base color, emission, metallic and roughness
change together with the frozen save baseline; the runtime session, simulation
state, gameplay image and asset owners remain intact. Schema revision 82 exposes
separate accepted material epochs and exact observed/submitted/presented snapshot
receipts. Eight Windows native checks and actual discovery pass. On one AMD GPU,
42 forward/deferred captures cover primitive, static and weighted bind-pose meshes;
ten further captures verify paused and running adoption in the same player window.
All authored/live comparisons match exactly. Four native window graphics groups
and Linux syntax checks of both disabled backends also pass. A retained CoreCLR
game adopts at tick 140 without changing its complete saved payload except the
frozen authored/content identity, completes at tick 777, and restores exactly in
a fresh process before five further callbacks. Its 4,180 settled native RPC rows
are retained losslessly. Corrected harness failures remain separate from passed
qualification. These checks establish functional behavior, not material latency,
texture/general scene adoption, physical scanout or Alpha completion.
[Material contract](LIVE_MATERIALS.md), [results](evidence/m2-live-materials.json).

Version 0.0.112 adds explicit version-3 save plans for entirely new custom
component types. Each type is bound to its exact target schema and listed
original authored entities that remain live in the checkpoint; new instances
receive declared defaults. Existing types, globals, native state and allocator
history stay preserved. Eight Windows native checks pass, including legacy-v1
restore and actual schema-81 discovery. A retained CoreCLR Relay Yard checkpoint
at tick 140 restores with its original image, upgrades to a separately compiled
target, verifies defaults before Tick, completes the remaining game at tick 777,
and exact-restores in a fresh process before five further callbacks. The 4,292
recorded native request/result/error rows also cover rejected edges, stale guards
and receipt retries. Source saves, compiled images and assets remain unchanged.
This qualifies the recorded Windows CoreCLR addition workflow; Native AOT
additions and automatic gameplay/editor upgrade selection remain unqualified.
[Results](evidence/m2-defaulted-component-additions.json),
[plan contract](SAVE_UPGRADES.md#add-a-defaulted-component-type),
[retained-game recipe](../tests/fixtures/relay_component_addition/README.md).

Version 0.0.111 adds independent `cast_shadows` and `receive_shadows` controls
for primitive, static and GPU-skinned drawables. Optional Boolean fields default
to true without rewriting old components; runtime definitions freeze the values.
Four native suites pass, including invalid-edit atomicity, authored undo/redo,
frozen templates and save restoration. Schema revision 80 exposes the controls.
Across forward/deferred paths and both shadow filters on one GPU, 108 candidate
captures pass; 32 images match the prior default captures exactly, and 12 explicit
true controls match omission. Visible/off-camera animated casters, disabled
casting with culling off, and independent receiving are covered. These fixtures
use AO disabled and do not qualify frame times, GI, caching or physical penumbras.
[Qualification](evidence/m2-renderer-shadow-controls.json),
[contract and usage](SHADOWS.md#per-drawable-controls),
[eligible draw counters](RENDER_DIAGNOSTICS.md#response).

Version 0.0.110 fixes a normal-bias point-shadow seam: cube-face selection and
projection now use the same receiver position. Both filters pass mirrored
deferred probes, with all 54 crossed-face measurements per filter staying
shadowed. The 24 baseline control bitmaps remain byte-identical. The expanded
public forward regression passes 49 captures, including cascades, all six point
faces, spotlights and moving/hidden/indexed casters. The Windows build and eight
selected native/SDK/Python groups pass. This fixes visibility correctness;
physical soft shadows, cross-face filtering, caching, GI and RTAO remain separate.
[Qualification](evidence/m2-point-shadow-face.json), [shadow behavior](SHADOWS.md).

Version 0.0.109 adds a bounded Python [evidence journal](EVIDENCE_JOURNAL.md)
with exact reconstruction; persistence success stays separate from experiment
success. An independent native 0.0.108 cohort with retained 0.0.88 managed tooling
passes 20 full-state C# edits and 25 build jobs using compact polling, batched
reads, an owned compiler, prepared restore and journal checkpoints. Mean is
**2.854906 seconds**; p95 is **3.003971 seconds**, missing the unchanged
three-second target. A fresh six-edit guard pilot is separate. Earlier full and
compact rewrite observations use different fixture bytes; the rejected journal
attempt is excluded. No matched comparison, causal improvement or Alpha closure
is qualified. [Results](evidence/m2-iteration-journal.json),
[all twenty samples](evidence/iteration-journal-native.json.gz).

The fresh 0.0.109 Windows build passes seven native/SDK cases, five discovery
requests and 25 pure journal regressions. Linux passes the journal, build-job and
runtime-observation suites (54 tests). These separate checks qualify persistence
and selected development contracts, without adding iteration timing samples.
[Qualification](evidence/m2-evidence-journal.json).

Version 0.0.108 adds Python `development_status` and `wait_for_development`.
The latter uses compact reads by default, passes each RPC the remaining finite
budget and returns native terminal outcomes. Local expiry retains detached last
status without cancelling work; native errors and interruptions propagate.
Twelve offline contract tests pass in checkout and installed-wheel form on both
Linux and Windows. Six real Windows SDK builds check success, syntax failure and
repair through both package origins, including complete diagnostics, exact
full/compact terminal projections, receipt replay and unchanged authoring state.
These client checks use the retained native 0.0.107 executable and schema 79;
they do not qualify a new native build, gameplay or iteration latency.
[Client recipe](AGENT_BUILD_JOBS.md),
[Windows qualification](evidence/m2-python-development-jobs.json).

Version 0.0.107 adds optional `development.inspect.include_output` in schema
revision 79. Omitted/true preserve the full response; false keeps authoritative
state, elapsed, errors, verified results, byte/truncation counters and compiler
ownership/use/retirement metadata while omitting raw client and compiler tails.
Native summary polling constructs that projection without copying the omitted
strings. Seven Windows cases pass, including real owned-SDK success, syntax
failure and repair with exact full-to-compact comparison. Five native discovery
requests verify the schema, strict flag typing, unknown-job rejection and
unchanged job catalog. No compact-polling latency improvement follows from these
correctness checks. [Qualification](evidence/m2-compact-development-poll.json).

A separate full-state 0.0.106 comparison passes twenty actual compatible edits,
25 build jobs and complete supported checkpoint/first-Tick checks per cohort.
Original reads with isolated compilation reach p95 6.972 seconds; owned
compilation reaches 7.732 seconds despite a better mean and native worker time.
With the same owned compiler and qualified batched rig reads, p95 reaches
3.759 seconds. All remain above the three-second target. Fresh parity matches
61 native entity envelopes across 91 snapshots and independently joins twelve
real stale-boundary/removed-member rejections; the batched arm retains 74 complete
two-page observations. All owned warm builds have correlated Received/Completed/Csc
proof and clean outer process trees. Fixed sequential order and distinct output
caches limit causal interpretation. These paused CoreCLR results do not qualify
Native AOT reload, physical input or the broader Alpha gate.
[Comparison](evidence/m2-iteration-comparison.json),
[all numeric samples](evidence/m2-iteration-comparison.numeric.json.gz).

Version 0.0.106 increases structured diagnostic parser capacity to 128 KiB per
stream and 64 KiB per line. Ordinary verbose MSBuild invocation/response lines
can now fit without an incomplete report. Genuine clipping, oversized input,
malformed diagnostic-looking lines and result overflow remain explicit.
Parser regressions pass on Windows and Linux; three Windows native groups and
fresh owned-compiler success, real syntax failure and repair pass. Each SDK
check encounters a 23,802-byte command line and returns complete diagnostics;
failed compilation preserves the previous DLL. The outer process group closes
empty without forced cleanup. [Evidence](evidence/m2-diagnostics-budget.json).

An earlier 0.0.105 full-game isolated run passes twenty warm edits and 25 compile
jobs, with p95 6.913 seconds, missing the three-second target. Its owned-compiler
arm stops before gameplay: compilation succeeds, but the old parser's 16 KiB
line limit makes diagnostics incomplete. The failed receipt remains retained.
The parser checks above resolve that capacity mismatch. The later 0.0.106
comparison is recorded above; the earlier failed receipt remains separate.

Version 0.0.105 adds opt-in `development.compile.compiler_server` for Windows
x64 .NET SDK 10.0.401. The native worker owns a private compiler between ordinary
MSBuild jobs, with bounded lifetime, diagnostics and retirement. Isolated builds
remain the default. Status separates process reuse from correlated original
process/Csc proof; incomplete evidence stays unproven.
[Contract](DEVELOPMENT_JOBS.md#optional-owned-windows-compiler).

Ten real SDK jobs pass, including changed-source reuse, syntax failure,
unresponsive-generator cancellation/timeout, fresh recovery and missing-tool
rejection. Six successful builds emit distinct DLLs; failed attempts retain the
prior artifacts. Five Windows native contract groups and separate queued/late
cancellation and host-destruction checks pass. Every outer process group ends
empty without forced cleanup; cancelling an unresponsive compiler deliberately
forces its inner owned group. Healthy destruction proves original-process death
and unchanged artifacts without a natural-exit receipt. A further idle-expiry
check observes original-process death at 60.07 seconds and proves fresh compiler
use and a distinct DLL afterward; it does not attribute the idle exit or prove
an exact lifetime. Earlier failed cleanup and idle-admission attempts remain
recorded. These checks qualify compiler correctness and
lifecycle, not gameplay reload, rendering or the three-second iteration budget.
[Evidence](evidence/m2-owned-compiler.json).

Version 0.0.104 adds an explicit `development.compile.restore` boolean, defaulting
to `true`. Setting it to `false` skips implicit dependency restore while keeping
the native worker, diagnostics and retry guards. The caller owns restore
invalidation; Poima does not automatically validate a prepared dependency cache.
[Build policy](DEVELOPMENT_JOBS.md#dependency-restore-during-c-iteration).

Native service contracts pass on Windows and Linux. The World discovery check
passes 1,183 responses across six owner scopes. Five real Windows .NET builds
verify cold failure without fallback, ordinary restore, changed source without
restore, syntax failure retaining the prior DLL, and a successful repair.
Windows process trees exit cleanly. This check covers compilation and artifact
identity, without qualifying gameplay reload or a faster iteration budget.
[Evidence](evidence/m2-compile-restore.json).

Version 0.0.103 exposes `--save-write-mode synchronous|background` on standalone
game launches and headless game serving. CLI discovery includes the mode and
required external root. Player polling performs owner maintenance at paused,
active and terminal boundaries; it does not advance gameplay or wait for storage.
Blocking playback reports its final pending save observation without turning
window close into a storage fence. Synchronous writing remains the default.
[Launch and completion contract](GAMEPLAY_SAVES.md#configure-storage).

Four Windows native checks pass: disabled-backend owner polling, renderer-linked
deferred stop/terminal polling before device initialization, schema discovery,
and compiled CoreCLR background saves with a fresh World owner in the same
process. The disabled backend also passes three Linux correctness groups.
A newly exported primitive game bundle passes 27 native CLI commands and 20
service calls for discovery, admission, defaults and headless mode forwarding.
Every Windows check exits with no remaining descendants or forced cleanup.
Rendered launch forwarding, durable completion in a paused native window,
fresh-process restoration and save hitch budgets remain separate checks.
[Evidence](evidence/m2-standalone-save-mode.json).

Version 0.0.102 adds opt-in background gameplay save writes through
`save.configure.write_mode`. The owner captures one immutable checkpoint after
the accepted batch; background jobs inspect storage, select the generation,
write it and resolve uncertain receipts. Later ordinary ticks and non-saving
controls retain the original save boundary. New save/load requests remain busy,
and pending writes preserve replacement, reconfiguration and external-edit
guards. Results and worker traces publish on the owner, including idle owners.
Synchronous writing remains the default; zero-worker configurations reject
background mode. Capture still runs on the owner and its hitch budget remains
unmeasured. Eight Windows native checks cover writer faults/cancellation and
compiled CoreCLR dispatch, continued ticks, rollback, guards, retries and a
fresh-owner restore in the same process. The separate retained Native AOT UI
check uses a manually dispatched queue. No player window or save-performance
measurement is included. That original checkpoint did not include the standalone
mode flag or blocking-player idle hook; those are added by 0.0.103.
[Contract](GAMEPLAY_SAVES.md#commit-and-rollback) and
[evidence](evidence/m2-background-gameplay-saves.json).

Version 0.0.101 adds bounded Python SDK query pagination for registered runtime
components. The first read supplies omitted guards; every continuation pins all
five runtime revisions/control guards and verifies the same authored-source
boundary, membership, cursor and selected projections. Native values remain
authoritative, and incomplete traversals never become completed snapshots.
Core-result validation also rejects boolean/float read guards that Python could
otherwise equate to integers, retaining request context without retrying.

Forty-six pure test groups pass on Linux and Windows. The built wheel passes ten
wrapper groups on Windows. Checkout and wheel each pass four genuine native
scenario groups using the retained 0.0.99 Windows engine: 65 entities across
three pages, complete parity with individual state/component reads, empty query,
page-budget rejection and a real same-tick component conflict. Each run makes
146 native requests and closes normally. Native Linux runtime-query coverage,
field-value schema validation and whole-traversal time budgets remain separate.
This is SDK/protocol qualification, not renderer, game-performance or Alpha
qualification. [Guide](PYTHON_CLIENT.md#traverse-registered-runtime-components),
[evidence](evidence/m2-python-runtime-observation.json).

Version 0.0.100 fixes development-job capture cleanup under the worker mutex.
Forgetting a completed job can now destroy verifier captures that query or
cancel jobs on the same live worker. Submission keeps capture construction and
failed-admission destruction outside the lock. Two real children and a gated
verifier establish the last-owner boundary; the focused regression and original
worker suite pass on Linux and Windows. The old Linux implementation reaches
that gate and deadlocks, retained as a timed-out negative reproduction. The
first Windows standalone build failed before test entry because it lacked
runtime DLLs; the corrected static build passes with an empty owning process
tree. These are standalone worker checks, not new renderer or game-iteration
measurements. Allocation-failure injection and reentry during worker destruction
are not qualified. [Contract](DEVELOPMENT_JOBS.md),
[evidence](evidence/m2-development-capture-cleanup.json).

Version 0.0.99 adds opt-in `pcf3_fractional` shadow filtering, with the existing
`pcf3_point` default preserved. On the RTX 4070 Laptop, each mode passes 33
controlled deferred captures of a licensed skinned character. All 33 reference
images match the retained 0.0.98 engine exactly; all 12 shadow-disabled images
match between modes. Twenty-five committed pose variants match, and verified
floor samples establish actual direct-light occlusion. Visual inspection shows
smoother cast edges and self-shadow bands in this fixture. Two paused native
player windows verify mode forwarding, frozen poses, capture retries and normal
shutdown. All owners exit with empty process trees and no render validation errors.
[Filter contract, comparison images and reproduction](SHADOWS.md#controlled-character-comparison).

The 0.0.99 build passes 14 selected native/metadata contracts and six
live-preference groups. The initial lighting test incorrectly requested a
shared-player command from a standalone owner; the corrected test preserves
filter admission and atomic rejection checks, with shared-player behavior
qualified separately. [Build and checks](evidence/m2-checkpoint-0.0.99.json).
Fractional-filter GPU cost, moving-scene stability, forward-path visual coverage
and point/spot edge cases remain open. Physical penumbras, caching, per-object
cast/receive controls, GI and RTAO are not implemented by this change.

Checkpoint 0.0.98 records two separate real-game checks using the retained
0.0.97 Windows engine. Performance Yard's 0.0.94 Native AOT image passes 40
checkpoint replacements, 1,801.047 cumulative unpaused seconds and another
60 seconds of fresh-process continuation. All 161 complete checkpoint
observations match, including 40 immediate Save and 40 Load retries; current
preferences stay separate from saved gameplay. Forty-one bounded diagnostic
reads retain healthy clock/audio observations with no discarded simulation
time. Four native owners exit normally, with no render validation errors and
drained audio. This is cumulative play across planned pauses and restores,
not an uninterrupted interval or a code-reload check.
[Reliability evidence and raw observations](evidence/m2-performance-yard-reliability.json).

Twelve short frame windows on the RTX 4070 Laptop meet their timing budgets;
the highest present-return p95 is 13.3114 ms. Those windows do not cover every
frame of the longer session. Working set and private commit remain below the
4 GB screens, but private commit's late-minus-early median increases by
23,474,176 bytes. Sampled process memory is not VRAM or proof of no leaks, and
growth still requires review. The earlier 0.0.96 clock failure remains retained
without an established cause. An initial 0.0.97 rehearsal also failed before
play because foreground activation was denied; neither attempt is erased by
the later pass.

A separate paused Relay Yard CoreCLR cohort uses the matched 0.0.88 SDK/bridge.
Twenty genuine warm source edits pass compatible reload, preservation of all
50 gameplay fields and supported native saved domains, and a first semantic
Tick proving the changed 3 m/4 m interaction range. Twenty-five real compiler
jobs include 24 successes and one intentional syntax failure; failure/repair,
stale and incompatible reloads, immediate reload retry and collectible-module
cleanup pass. Same-context native captures remain exact across compatible
reload and a separately authored scene edit. The scene edit stays outside the
running game until explicit stop/start; an old save restores with its exact
original image, without implicit conversion.

Edit-to-first-verified-tick p95 is **7.598753 seconds**, missing the **3-second**
target. The interval includes source write, compilation and polling, retained
artifact/proof work, full state/checkpoint inspection, reload and verified Tick;
it is not bare compiler time. Functional success is separate from this timing
miss. Neither cohort qualifies Native AOT hot reload, general live scene apply,
physical controls/listening, arbitrary project upgrades or the whole Alpha gate.
The publication checkpoint is 0.0.98; the measured engine remains 0.0.97.
[Iteration evidence and raw observations](evidence/m2-relay-yard-iteration.json).

The version-only 0.0.98 Windows rebuild passes 13 selected native/metadata
contracts and six live-preference groups, with unchanged inputs and empty owned
process trees. This separate check does not transfer the longer 0.0.97 game
measurements to the rebuilt binary. [Build and check record](evidence/m2-checkpoint-0.0.98.json).

Version 0.0.97 adds bounded native slow-poll observations through
`player.diagnostics.inspect`. The owner retains up to 128 events with current
and preceding poll timings, clock acceptance/discard, retained swapchain dimensions,
resize work and separate audio device durations. Inspection reads owned CPU
values and remains available after the native window closes. The interactive
clock's eight-tick cap, gameplay services and save formats are unchanged;
discovery revision 73 adds the inspection method. [Contract](LIVE_PLAYER.md#slow-poll-diagnostics).

Thirteen selected Windows native CTests, twelve simulation-disabled headless
authoring CTests and six native preference groups pass against frozen inputs.
The Windows player-service cohort passes 148 requests. A separate two-cycle
Performance Yard diagnostic rehearsal passes 1,406 requests, including 160
diagnostic reads, nine complete checkpoint/restoration observations and a fresh
native-process continuation. Programmatic paused resizing settles at both
960×540 and 1920×1080 without advancing the tick. Resize observations are
retained; both lifetimes retain audio timings, with zero discarded simulation
time and no unrequested GPU profiling. These short checks do not close the longer reliability gate or
measure diagnostic overhead in a paired comparison.
[Qualification and compressed native observations](evidence/m2-player-diagnostics.json).

The first longer Performance Yard attempt used the retained 0.0.96 native
engine and 0.0.94 Native AOT game. It failed after 23 Save/Load cycles and
1,035.611 cumulative unpaused seconds, with all 92 checkpoint observations
matching. Cycle 24 caught 0.759537 seconds of discarded native clock time.
The first failure inspection also reports a viewport change from 1920×1080 to
1920×1051; neither the stall nor that change has an established cause. Seven
earlier short frame windows passed their timing budgets. Native owners exited
cleanly with no render validation errors and drained audio. That attempt did
not reach the 1,800-second requirement or fresh-process continuation.
[Failed attempt, reproduction and limits](PERFORMANCE_YARD_RELIABILITY.md).

Version 0.0.96 avoids rebuilding mesh objects and animation skin palettes when
projecting native player reports. A camera/lens/lighting-only query shares
camera validation with full snapshots; rendering, cached report boundaries,
JSON schemas, gameplay services and save formats stay unchanged. Native tests
cover moving camera-attached lights, independent lenses, owned values and fresh
checkpoint continuation.

The unchanged Performance Yard workload shows mean host gaps of 3.440 → 3.000 ms
on NVIDIA and 3.952 → 2.997 ms on AMD. NVIDIA present-return p95 is essentially
unchanged (12.746 → 12.773 ms); AMD improves from 21.877 to 18.281 ms but still
misses 16.7 ms. AMD candidate p99 is 19.172 ms. GPU durations also vary without
renderer changes, so the entire cadence difference is not isolated to camera
extraction. Runs are sequential one-minute pairs on one laptop, without thermal
control or randomized repeats. [Comparison and reproduction](CAMERA_OBSERVATION.md),
[raw measurements](evidence/m2-camera-observation.json).

Nine selected Windows native CTests, nine simulation-disabled headless authoring
CTests and two Linux simulation CTests pass, along with six native preference
unit groups, 14 Windows world and nine runtime contract checks. Actual Windows
player-service, live-settings and 40 repeated checkpoint-restoration cohorts
pass. The restore cohort uses a small physics/UI fixture without compiled
gameplay; it is separate from the Native AOT performance workload. A regression
supervisor first stopped before launching an engine due to an incorrect client
package path; that failed setup is retained alongside the corrected run.

Version 0.0.95 reduces GTAO work without lowering resolution, slice/step counts,
radius, filtering or quality settings. Front-facing slices use a direct form
of the existing integral; rear-facing and tiny projections retain its general
form. Neighbor samples omit unused normal reads under the renderer-owned
G-buffer producer contract. Center normal validation remains complete. No
engine API, discovery schema, gameplay services or save format changes.

Paired reference/candidate captures pass on both GPUs: 25 images per binary and
device, with all seven AO-disabled images exact, enabled images differing by at
most one channel level, and raw/filtered visibility probes by at most
0.00048828125. These are bounded equivalence checks, not general physical AO
accuracy. Fresh Performance Yard comparisons use the same 1080p medium-GTAO
workload, 30-second warm-up and 60-second measurement:

| Device | Mean AO, reference → candidate | Mean total GPU, reference → candidate | Present-return p95, reference → candidate |
| --- | --- | --- | --- |
| RTX 4070 Laptop | 1.079 → 0.902 ms | 2.747 → 2.692 ms | 12.478 → 12.695 ms |
| AMD integrated | 10.777 → 4.001 ms | 19.584 → 13.416 ms | 27.067 → 18.625 ms |

AMD candidate p99 is 19.486 ms: it meets the 25 ms p99 budget but still misses
the 16.7 ms p95 budget. NVIDIA candidate p99 is 13.721 ms; lower AO cost does
not establish a NVIDIA cadence improvement. Earlier integral-only and guarded
neighbor-decode attempts remain recorded; neither established a meaningful AMD
gain. Results are short measurements on one laptop, not a soak, scanout FPS,
VRAM residency or production-scene guarantee.

Seven Windows and nine headless authoring CTests pass, along with 14 Windows world
and nine runtime checks. The original per-device AO fixtures also pass: 12 basic,
21 API, 100 reconstruction and 50 UI captures. The first API attempt stopped
before image capture at a stale schema-52 assertion; only that discovery
expectation was updated to 72. No visual tolerance was relaxed.
[Optimization, reproduction and raw observations](evidence/m2-ao-optimization.json),
[AO guide](AMBIENT_OCCLUSION.md#bounded-optimization-checkpoint--0095).

Version 0.0.94 adds [Performance Yard](../examples/performance-yard/README.md),
an API-authored combined workload retaining compiled Relay gameplay, licensed
character/audio, navigation and UI, with 24 weighted animated rigs, 768 simple
props, 32 dynamic boxes and 64 lights/eight shadow views. An opt-in compiled
controller follows native navigation on the actual interactive clock; no replay
or per-tick RPC supplies movement. Its new module/content identity is separate
from retained Relay save cohorts. No engine API, schema or services change.

Native Windows Native AOT publication and both focused GPU runs pass functional
checks with 30 seconds of warm-up and 60 seconds of measurement at 1920×1080,
deferred lighting, medium GTAO and no reconstruction/frame generation.
Both sustain approximately 60 simulation ticks/s, travel 163 m and complete three
circuits. RTX 4070 Laptop present-return cadence is p95 12.444 ms/p99 13.348 ms;
AMD integrated graphics is p95 27.409 ms/p99 29.196 ms and misses both timing
budgets. Sampled peak working sets are 404,819,968 and 469,823,488 bytes,
respectively; these are process observations, not VRAM. All GPU tickets retire,
native owners exit normally and NVRHI errors remain zero. Seven selected
headless and six Windows tests pass against frozen native inputs.

These are short, modest-geometry measurements on one laptop. Spectators play
Idle, can overlap the camera at circuit corners, and do not provide crowd AI;
dynamic boxes can settle. Skinning includes shadow-only work. Initial power and
owning-monitor observations are AC and a 144 Hz OS mode, not continuous power,
scanout or VRR qualification. The short cohort does not qualify a 30-minute
soak; the later cumulative-play check is recorded above. Production FPS
complexity, physical input/audibility, GPU residency and clean-machine delivery
remain open.
[Recorded results and compressed raw rows](evidence/m2-performance-yard.json).

Version 0.0.93 adds an independent `performance.*` capture for native player
polls: bounded preallocated rows, guarded identities/retries, immutable pages,
CPU wall stages, actual present-return cadence and delayed GPU execution/query
outcomes. Stop closes admission, drains submissions and freezes; unresolved
execution and invalid samples remain explicit. Existing `profiler.*` stop and
retirement semantics are unchanged. Discovery schema is 72; authoring-core v1,
gameplay services epoch 7 and native artifact format 2 are unchanged.

Actual Windows integrity probes pass on NVIDIA RTX 4070 Laptop and AMD integrated
GPUs, each with one/two frame slots, screenshot/resize interventions, overflow,
paused tick stability and independent row/counter reconstruction. Each collects
40 samples from the original engine process handle, keeping working set and
private commit distinct. Native owners close normally with no NVRHI errors.
The probes use a small paused fixture, so their timings do not establish
interactive game FPS, representative performance, a 30-minute soak, GPU residency
or click-to-photon latency. Seven headless and six Windows selected tests pass,
plus six authoring compatibility checks. [Capture guide](FRAME_PERFORMANCE.md),
[qualification and raw observations](evidence/m2-frame-performance.json).

Version 0.0.92 publishes DEPOT RUN's retained agent-authored source, licensed
cooked content and independent gameplay/save/bundle verifier. The measured
engine is 0.0.91. A Linux Codex provider uses the native Windows MCP host and
separate build consent to author 99 entities, compile C# and submit genuine
Native AOT publication and verified 60-file Windows export.

The first fresh attempt is incomplete: a capture and subsequent publication
return unknown transport outcomes while the owner stops answering. The owner
is forcibly closed; missing owner output directories and the outage's cause
remain unexplained. A maintainer restarts the same engine and restores owner
directories. A separate agent recovery session completes publication/export
without changing game source or scene. This is a recovered workflow, not a
one-shot autonomy claim. Native Windows provider file operations fail an earlier
preflight; Linux provider file access passes without widening policy.

Independent managed/native gates each pass 14,623 requests, covering ordered
native-controller interaction, negative cases, all 14 scalar fields, exact
same/fresh-process saves, terminal audio state and compiled Begin resuming an
initially paused player. Relocated source-free bundle checks pass 14,607 requests
with development-tool variables removed and system-only PATH. The public
fixture/verifier repeat both native scopes successfully. An earlier verifier
launch used an unpumped stdio owner; its failure remains separate from the
corrected shared-owner pass. No engine runtime, authoring schema or gameplay
ABI change is introduced by this checkpoint.

Physical input, audible device output, clean-machine launch, visible imported
skinning at the initial camera, game-scale performance and general agent success
rates remain unqualified. Menu is checked while externally paused because live
external UI mutation is prohibited; Begin alone is proven to resume. Repeated
fresh workflows and representative performance still block broader alpha
qualification. [Fixture and reproduction](evidence/fixtures/agent-depot/README.md),
[recorded attempts](evidence/m2-agent-depot.json).

Version 0.0.91 exposes native publication and game export through named,
owner-configured development profiles. Standalone, shared and MCP world hosts
load the same policy before accepting jobs. Clients cannot supply tool paths,
environment changes or shell commands to the new operations. Exact retries
include the operation and profile fingerprint; accepted output locations remain
reserved across failure, cancellation and forgotten status.

Worker verification checks complete native artifact or bundle inventories after
successful child cleanup. A zero exit alone cannot report success; verification
errors fail the job, and cancelled jobs expose no successful result. Inspection
returns compact verified hashes, file/byte counts and target information.
Verification uses bounded filesystem reads but is not interruptible by the child
deadline or cancellation. Successful receipts identify completion bytes, not a
lease against later output edits.

MCP requires the separate `poima_build` tool for all compile, publish and export
submissions; `poima_call` rejects them before backend execution. Provider build
consent is separate from world editing. Direct native RPC remains a trusted
interface, and other world operations can execute selected trusted gameplay.
This is not a process sandbox or universal code-execution boundary.

Matching-host publication checks pass 222 MCP requests on Windows and 544 on
Linux, with actual Native AOT artifacts, compiled Tick/Control callbacks,
independent inventory checks, compiler-error feedback, exact retries and observed
queued cancellation. Windows also passes native game export and inspection;
Linux qualification is publication-only with a headless installed runtime.
Standalone/shared profile startup passes on both platforms. Seven Windows native
contracts, three native-Windows Python protocol suites and eleven regressions
in each Linux runtime/authoring build pass. Three privileged Windows symlink
fixture cases are skipped; actual Windows junction configuration/output guards
pass separately. The relocated source-free primitive game-service regression
passes 48 RPC requests, six MCP requests and one notification without compiled
gameplay; it does not extend compiled-game relocation or rendering evidence.

The initial native fixture build failure and incorrectly launched Windows
protocol regressions remain recorded separately from corrected passes. Frozen
native inputs match all three terminal builds. Authoring-core v1 remains stable;
schema revision is 71, services epoch 7 and native artifact format 2 are unchanged.
Tools and live projects are trusted, tool pins do not cover whole SDK/resource
closures, and publication still needs source-checkout authoring resources.
A fresh external-agent build-through-export gate and representative performance
qualification remain ahead. [Contract and reproduction](DEVELOPMENT_PROFILES.md),
[evidence](evidence/m2-development-profiles.json).

Version 0.0.90 adds complete per-job environment replacement to the native
development worker. Omission preserves inheritance; an explicitly empty list
produces an empty child environment. Unicode, names, platform-specific duplicate
rules and byte/count limits validate before queue acceptance. Parent environment
and existing cancellation, deadline and process-group cleanup behavior remain
unchanged. Native gameplay publication disables persistent compiler/MSBuild
servers and node reuse for supervised builds.

Windows and Linux real-child contracts pass inherited, replaced and empty
environments, exact bounds, malformed input and descendant cleanup. Matching-host
toolchain checks probe the exact replacement, omit deliberately invalid owner
startup hooks, publish actual Native AOT games through both SDK reference forms,
execute compiled Tick/Control callbacks and verify complete immutable artifacts.
The missing-copy fixture must fail normally with exit 1; timeout, cancellation
or launch failure cannot pass it. Windows qualification required explicit machine
configuration/platform variables and job-owned profile/cache directories; initial
failed attempts are retained in the evidence.

The authoring compile RPC retains its original shape and inherited environment;
real SDK success/failure tests reject injected environment parameters before
creating work and retain unchanged world revisions. Authoring-core v1, schema
revision 70, services epoch 7 and native artifact format 2 are unchanged. This is
trusted worker/toolchain qualification, not an autonomous publish/export API,
source snapshot, process sandbox or clean-machine installation claim. Native
owner checks monitor selected sentinels; the Python driver separately compares
its full environment. Emergency Linux driver cleanup remains best-effort and
unqualified; normal worker cancellation is tested independently.
[Contract and reproduction](DEVELOPMENT_JOBS.md#per-job-environments),
[evidence](evidence/m2-development-environment.json).

Version 0.0.89 fixes Native AOT publication for games consuming the distributed
`Poima.Gameplay.dll` through a binary SDK reference. Generated entry/binding
code explicitly references the SDK copied by the selected game build; projects
that disable copying receive an actionable rejection before native generation.
The game project remains a project reference, preserving its source/dependencies.
This changes build tooling, not authoring-core v1, services epoch 7 or artifacts.

Matching-host Windows and Linux regressions publish the same unchanged compiled
UI fixture through DLL and project references, execute real native Tick/Control
callbacks, verify exact SDK/artifact hashes and reject a noncopying SDK reference.
These checks cover matched SDK/tooling cohorts, not arbitrary SDK version mixes,
clean-machine toolchain installation or complete autonomous build workflows.
See the [publishing contract](NATIVE_GAMEPLAY.md#publish),
[regression runner](../tests/native_gameplay_sdk_reference.py) and
[recorded evidence](evidence/m2-native-sdk-reference.json).

Version 0.0.88 integrates licensed audio into Relay Yard. Five original Kenney
CC0 clips are preserved with licenses, hashes and a reproducible offline
conversion recipe; native import supplies mono 48 kHz cooked audio. Eight
permanent emitters carry pickup, denied interaction, courier arrival, completion
and alternating footsteps. The pickup voice survives collectible removal.
Per-character scalar cadence uses committed displacement, a short flat-floor
support ray and bounded phase; it is not animation-driven foot contact or a
general terrain/material gait model.

Windows and Linux genuine Native AOT playthroughs each verify 5,962 native
operations, including negative interaction, all cue events, exact voice/cadence
checkpoint restoration, inert retries and completion. A newly authored silent
profile also completes with the full three-schema module manifest. The 50 global
state fields, their persistent IDs and services epoch 7/256-byte prefix are
unchanged. The audio scene is a separate content cohort; retained two-schema
0.0.86 worlds and artifacts are not silently converted.

Real controller inputs independently reproduce two footsteps and a pickup
through offline capture and continuous WASAPI playback on both laptop GPUs.
Each Windows run uses five native owners and 1,066 operations. A saved active
pickup resumes at its exact logical cursor in fresh owners; 20 neutral ticks
finish it without another allocation. Device submissions of 72,800 and 16,000
stereo frames match fresh offline mixer peaks/voice starts, drain cleanly and
report zero timeline resets and over-range samples. This qualifies submission
and mixer continuity, not listening, hardware loopback or an uninterrupted DSP
waveform across restore. Linux headless coverage uses three owners/743 operations.

Source-free Windows exports carry all five cooked audio assets, provenance,
credits and redistribution notices. Both GPU runs remove the owned source copy,
relocate the immutable bundle, restore exact voice/cadence/native game state in a
fresh service and finish the objective. Selected protocol, audio, save and build
metadata regressions pass across the three native build configurations.
[Evidence](evidence/m2-relay-yard-audio.json) and
[licensed-audio playbook](../examples/relay-yard/README.md#optional-licensed-audio).
Physical input, listening, representative performance, fresh-project agent
workflows and clean-machine delivery remain separate Alpha gates.

Version 0.0.87 fixes saves after removing played runtime emitters. Emitter
retirement legitimately leaves holes, removes the newest handle or empties
logical voice history while preserving its allocator. Restore now accepts those
states while retaining positive ordered ID bounds, trusted clip bindings,
chronology, capacity limits and atomic load validation. Sound-state format 1,
authoring-core v1, schema revision 70 and services epoch 7 are unchanged.

Windows and Linux reproduce the previous build's isolated save rejection, then
pass real hierarchical spawn/play/despawn, relocated fresh-owner restoration,
failed sound-batch rollback and allocation after complete retirement. Each
corrected run checks three scenarios through nine clean native owners and 93
operations. Native tests additionally cover retirement after history pruning
and malformed ID/allocator rejection. Both Windows GPUs submit and drain actual
WASAPI output with matching offline mixer statistics; this is diagnostic content
and device submission, not listening or physical loopback qualification.
The retained real-game upgrade also passes on the corrected host without
changing its compiled artifacts or content. [Evidence](evidence/m2-sound-retirement.json)
and [lifetime/save task](AUDIO_EVENTS.md#removing-emitters-and-saving).

That checkpoint closes a sound/save lifecycle defect; its diagnostic audio
checks do not establish whole Alpha, general acoustic realism, representative
performance or audibility. Licensed game integration follows in 0.0.88.

Version 0.0.86 gives Relay Yard persistent global field IDs and a meaningful
checkpoint version. All 49 old fields keep their kinds; the new field defaults
to 2, the actual sequential layout grows from 328 to 336 bytes, and the artifact
adds `gameplay_persistence_v1` without changing its 256-byte services prefix.
The [gameplay/content/save profile](ALPHA_GAMEPLAY_PROFILE.md) separates released
stable authoring, negotiated gameplay features, bounded content intake and exact
or explicitly upgraded checkpoints. It does not freeze every SDK API or format.

The [retained-game playbook](../examples/relay-yard/UPGRADING.md) consumes actual
0.0.85 Native AOT artifacts and version-5 root-prop checkpoints, not regenerated
source saves. Windows and Linux checks exact-restore the old game, reject
changed-image and invalid-plan loads without mutation, then preserve every old
global value and all native state through one explicit hash-bound upgrade.
Immediate observations verify the new default, target schema/image and every
reported edge identity before target callbacks. Real remaining pickups, native
courier/animation travel and terminal Use complete the objective. A deliberately
written new target slot exact-reopens in a fresh process and runs real callbacks.
Native restoration bypasses `Initialize` by the reviewed dispatch path; state
preservation is not a dynamic constructor/static-initializer trace.

Both laptop GPUs compare source/target captures at the exact saved tick and
authored overview camera. Native pixels and draw counts match, with eight draws,
1,688 triangles, one skinned instance and 1,029 weighted vertices submitted.
The checkpoint menu overlaps part of that overview; this is native output
preservation, not an independent source-pose or unobscured-character oracle.
Each Windows capture cohort passes 4,218 operations; Linux headless passes 4,198.
The updated source-free exported-game continuation also passes on both GPUs,
with 4,580 operations and seven native captures per run.
[Evidence](evidence/m2-relay-yard-upgrade.json) retains the initial verifier's
incorrect version-6 assumption as a failed attempt; actual root-prop snapshots
require version 5. The save format/admission policy was not loosened. Protocol 1,
authoring-core v1, schema revision 70 and services epoch 7 are unchanged.

This qualifies a real global-only Native AOT update, not arbitrary component or
geometry updates, upgrade chains, automatic compiled-menu migration, CoreCLR
Relay Yard upgrade, physical input, audibility, representative performance,
clean-machine delivery or whole Alpha. Ordinary menu loads remain exact and
host-selected upgrades do not silently overwrite source checkpoints.

Version 0.0.85 adds [Relay Yard](../examples/relay-yard/README.md), a complete
small compiled first-person integration game. Three real camera-ray pickups
unlock an imported Idle/Run courier's native route around cover; an in-range
terminal Use after arrival completes the objective. Compiled menus provide
checkpoints, visible save errors and current native preferences. The sample
reuses the exact original licensed FBX intake and reviewed locomotion component
layouts. It adds no engine API or schema surface.

Actual Native AOT publication is qualified on Windows and Linux. Both semantic
playthroughs pass four groups and 5,492 native operations each: welcome gating,
missing-save and negative-use outcomes, premature terminal rejection, repeated
compiled saves, guarded retries, exact partial restoration, native courier
collision/animation and completion. The partial save contains one collected
cell and a stationary courier; this checkpoint does not establish active-motion
save coverage. [Evidence](evidence/m2-relay-yard.json) preserves earlier failed
attempts and their recovery rather than relabelling them as passes.

Windows exported-game qualification runs on both laptop GPUs. Each removes the
owned source copy, relocates the immutable bundle outside the checkout and
launches from an unrelated directory with a system-only path. One native
process collects a cell and saves; a distinct native process uses Welcome Load,
restores the exact partial snapshot under its independent current player
configuration, and completes the remaining game. Seven native frame captures
per GPU observe welcome, menus, restore, Run and final Idle. Public player
identities/configuration are observed; hidden preference epochs are not claimed.
These checks do not establish physical input, audibility, representative
performance, clean-machine deployment or full Alpha readiness. Protocol 1,
authoring-core v1, schema revision 70 and services epoch 7 remain unchanged.

Version 0.0.84 removes the native player's artificial exit after 32 interactive
checkpoint replacements. Restores retain the same window and current preference
owner while clearing old input, pausing the restored game and revalidating its
controller/camera. The replacement counter saturates at `UINT32_MAX`; recording
a session change remains independent of that diagnostic count. Recorded replay
still stops when its Runtime is replaced. World/session/module resource budgets
remain separate; this is not an unlimited-lifetime guarantee.

The [checkpoint-cycle verifier](LIVE_PLAYER.md#edits-replay-and-replacement)
uses actual controller movement and look before every load, exact restored
controller/camera state, fresh sessions, guarded retries and current live
preferences. Its captures compare the same native context around restore 32.
Forty restores pass on each of the NVIDIA and AMD laptop GPUs, with 21 selected
native regressions across Windows, Linux and the authoring-only build. The
retained exported C# menu checks also pass on both GPUs.
Recorded player/audio replay checks pass on both GPUs with a freshly published
Native AOT save fixture; replay still stops on Runtime replacement.
[Recorded evidence](evidence/m2-player-checkpoint-cycles.json) retains the
reproduced 0.0.83 exit and separates corrected runs from that negative result.
The fixture does not establish physical input, audible output, memory-growth
behavior, representative performance or whole-game/Alpha qualification. Protocol
1, authoring-core v1, schema revision 70 and services epoch 7 are unchanged.

Version 0.0.83 adds the [exported game service](GAME_SERVICE.md): `game serve`
verifies the complete bundle, opens its read-only world, starts the declared
Runtime and loads its optional native gameplay at tick zero. It acquires the
local endpoint before initialization and reuses the existing owner-thread host
loop. Clients discover the current runtime, step it, activate guarded compiled
menus, adjust preferences, service external checkpoints and capture a live
player through the same native API. No editor, new action-driver language,
automatic player window or initial simulation tick is introduced.

The no-window Windows contract exercises source deletion, relocation, native
controller movement, client detach/reconnect, exact receipts, external save/load,
MCP initialization/tools, rejected authoring and malformed bundle/endpoint
recovery. The compiled service runs the retained genuine 0.0.81 Native AOT
settings consumer on both laptop GPUs. Each performs 10 menu actions and three
Vulkan captures, verifies one actual Runtime replacement and retains the same
preference owner/ticket while current application/presentation reach revision 6.
The two client connections observe the same authoritative game. These are
semantic UI operations; physical pointer hit testing remains a separate check.

Three native configurations pass 21 selected regression executions. The initial
Windows launcher failure is retained separately from the corrected remaining
checks; suites that had already passed were not repeated.

Protocol 1, authoring-core v1, schema revision 70 and gameplay services epoch 7
are unchanged. `game inspect` adds the optional bundle-root-relative input-profile
path; resolve it before explicit `player.start` parameters. Current bundle export
still requires simulation and rendering. Linux headless builds compile this
service and retain their authoring/simulation regressions; native Linux game
bundles/rendering are not newly qualified. Tiny fixtures do not establish full
Alpha, clean-machine installation, audible output, broad failure recovery or
representative performance. See [recorded evidence](evidence/m2-game-service.json).

Version 0.0.82 qualifies the [exported settings/checkpoint workflow](EXPORTED_PLAYER_SETTINGS.md)
using the retained genuine 0.0.81 Windows Native AOT consumer. The source project
is removed before launching the relocated immutable bundle from an unrelated
working directory with a system-only PATH. Native window input opens the modal,
stages preferences, saves and restores the actual player Runtime. The restored
compiled callback queries its original acceptance ticket while the same current
player retains later FOV/UI/gain configuration and successful presentation.

The verifier observes initial/modal/restored visible UI pixels and checks exact
compiled-state, durable-save, ownership, revision, native backend and final Vulkan
menu pixels. Both the RTX 4070 Laptop and AMD integrated GPU pass five check
groups, with 10 menu actions per run; three native configurations pass 15
selected regression executions. It retains seven failed debug-symbol export
and input/verifier attempts;
stripped installation keeps the shipping executable within the exporter's
256 MiB per-file budget. The development binary retains its symbols. Protocol 1,
authoring-core v1, schema revision 70 and gameplay services epoch 7 are unchanged.

This is one small exported settings fixture, not complete-game or whole Alpha
qualification. Physical devices, audible output, representative performance,
clean-machine installation and fresh-process menu/save continuation remain
separate work. Input spacing is explicit and does not itself establish redraw;
changed OS header pixels supplement the exact final semantic assertions. See
[recorded evidence](evidence/m2-exported-player-settings.json).

Version 0.0.81 adds [compiled player settings and a native menu fixture](COMPILED_PLAYER_SETTINGS.md).
C# Tick/Control callbacks read committed configuration and cached observations,
stage a guarded typed patch and query its process-local acceptance ticket. They
use the same native preference owner as the external live API. At most one patch
is prepared for an entire native step or Control boundary; failed callbacks,
entity validation and later batch failure consume no preference revision or
ticket. Successful publication precedes queued save/load replacement.

The independent `player_preferences_v1` extension uses a named 256-byte service
view with callbacks at 232/240/248, preserving epoch 7 and every earlier prefix.
A larger table grants no undeclared intermediate feature. Host negotiation is
separate from attached-owner availability. Replay permits observations and
rejects mutations; stop detaches availability, while a fresh player gets a new
owner. CoreCLR compatible reload retains that owner; Native AOT keeps its
existing process-lifetime replacement restriction. Schema revision is 70;
protocol 1 and authoring-core v1 are unchanged.

The rendered menu opens a native modal, requests pause, tunes FOV/mouse/UI/gain,
resets sparse intent, refreshes status and closes with resume. Current graphics
remain frozen while presets report next-launch intent. Preferences do not enter
authored documents or gameplay saves; saving diagnostic owner/ticket words does
not restore the authority or ledger. Profile persistence remains explicit.

Windows CoreCLR and genuine Native AOT pass attached checks on both laptop GPUs,
including independent FOV/UI image calculations, accepted-versus-staged reads,
exact retry, external stale guards, rollback of earlier completed physics,
same-window save replacement and fresh-owner/process continuation. Linux
headless callbacks pass with both backends. The unchanged original 0.0.78
Windows consumers also pass under Native AOT/CoreCLR. Managed guard-page ABI
probes pass on Windows/Linux, and the guide's C# example compiles without warnings.

These checks do not qualify physical input, audible sound, injected Jolt or
device failures, same-event-batch ordering, production performance or a new
source-free exported settings menu. The native menu capture was inspected;
logical Control activation is separate from physical mouse/controller testing.
See [recorded evidence](evidence/m2-compiled-player-settings.json).

Version 0.0.80 adds a shared native [live player-preference owner](PLAYER_SETTINGS.md).
`player.settings.inspect` and guarded `player.settings.transact` support preview,
set/reset, revision conflicts and process-local retry receipts. FOV, pointer
sensitivity/inversion, absolute UI scale and master output gain apply to the
current interactive player. Inspection distinguishes accepted configuration,
native application and successful presentation. Queued semantic look and held
input survive pointer tuning; scaled UI invalidates old gestures/hit regions
until redraw. Graphics samples/frame slots remain explicit next-player intent.

Stored profiles now admit nine keys, including output gain. Profile persistence
is independent of live edits: changing a file does not alter a running window,
and gameplay save/load does not replace its preferences. Historical launch-report
outcomes remain compatible; the live channel supplies separate revision and
next-player observations. Protocol 1, authoring-core v1 and gameplay services
epoch 7 remain unchanged; discovery schema revision is 69. No gameplay SDK or
compiled settings-menu extension is added in this checkpoint.

Three native configurations pass 67 selected regression checks; both new CTest
registrations also pass on all three. Live fixtures on both laptop GPUs pass
268 recorded RPCs with six clean owners and 18 readbacks. Independent perspective
and UI-bound calculations check actual FOV/scale changes. Preview/rejection,
reset, profile relaunch, same-window restore and semantic replay are checked;
a deferred player's invalid combined graphics/UI patch preserves state and pixels.
Actual SDL streams verify numeric gain, including mute and timeline replacement.
Both live fixtures observe an accepted change while genuinely unpaused; this is
not physical input or audible-output qualification.

Preserved shared-player tests pass 277 RPCs with eight clean owners. Stored/launch
fixtures pass 224 RPCs with 138 clean owners, independent projection/reference
images and zero-frame failure outcomes. Separately compiled original hierarchical
games pass 206 RPCs with four clean owners under Windows Native AOT and CoreCLR.
Each births two imported characters, submits 2,058 weighted vertices, continues
native movement/animation and restores exact version-6 state and same-tick output
with current preferences intact. Original worlds, cooked content and artifacts
remain unchanged. Draw submission and same-owner output equality do not provide
an independent raw-FBX/visibility pixel oracle.

The documented native Python task also executes with the checked-out client,
producing independently checked before/after images without advancing simulation
or authored revision. All these are bounded fixtures, not a full game, performance
benchmark or GUI qualification. Post-acceptance device-failure recovery is
source-reviewed, without forced device-failure injection. Retained failures cover
test include/setup/guard errors, initial SDL surface settling and the corrected
legacy launch-outcome regression. [Evidence](evidence/m2-live-player-settings.json).

Version 0.0.79 extends [explicit save upgrades](SAVE_UPGRADES.md) to complete
hierarchy recipes, version-6 runtime snapshots and bounded collection fields.
Stable field IDs preserve saved values across renames and layout changes;
retired fields and added scalar/empty-array defaults require a complete approval
plan. Plan version 2 can explicitly change retained array capacity, preserving
length/order and rejecting overflow rather than truncating. Plan version 1 and
the scalar-only helper retain their strict existing contracts.

The mapper visits authored, template root/child and live-instance fields.
Ordinary source validation checks every saved field and entity reference before
retirement; approved transformation and normal target restoration finish before
publication. Native physics, animation, sound, UI, complete local-to-live maps,
structure revisions and allocator history remain unchanged. Exact loads remain
the default; upgrade plans are host-selected, identity-bound one-edge approvals.
Schema revision is 68; authoring-core v1, protocol 1 and gameplay service epoch 7
are unchanged. No gameplay SDK or service layout change is required.

Three native configurations pass 71 selected regression checks. Refined
snapshot tests also check growth, fitting shrink and runtime-only overflow with
valid source/target native reconstruction on Windows/Linux simulation builds;
the authoring-only build checks mapping without simulation. Four separately
compiled retained-save cohorts pass 1,040 RPCs with 28 clean owners under
Windows/Linux CoreCLR and Native AOT. They verify legacy-global ID binding,
renamed/reordered component fields, changed defaults, entity-array growth,
compiled callbacks, same/fresh-owner continuation, whole-root retirement/rebirth,
retry recovery and rejection isolation. A real compiled capacity-one target
rejects authored-recipe overflow; the native fixture separately covers saved
runtime overflow. The original source slots, artifacts and cooked assets remain
unchanged. An earlier scalar compiled consumer still passes 49 RPCs.

Optional readbacks on both laptop GPUs match source/target output at the same
tick, but their retained origin camera draws zero character geometry. They
qualify capture/context/output preservation, not character visual preservation.
Exact saved transforms, instance maps, animation and unchanged cooked geometry
provide separate state checks. Retained verifier failures document raw-native
JSON checksum handling and a corrected publisher-wrapper feature expectation;
no engine checksum or compatibility check was weakened. These are bounded
correctness fixtures, not automatic migrations, game-scale performance,
physical-input, GUI or an exported upgraded-game qualification.
[Evidence](evidence/m2-hierarchical-save-upgrades.json) and
[compiled fixture instructions](../tests/fixtures/instance_save_evolution/README.md).

Version 0.0.78 adds [hierarchical runtime instances](RUNTIME_INSTANCES.md).
Frozen recipes contain complete local entity graphs; each spawn allocates fresh
IDs and remaps local native references and registered custom entity/array fields.
Characters, cameras, imported rigs/skins, lights and audio join atomic membership
publication. Whole-root removal requires surviving reference repairs. Failed
batches restore membership, physics, animation, sound, typed gameplay and ID
allocation; successful canceled births consume their reservations. Version 6
checkpoints retain complete instance maps and allocator history. Exact restoration
and the explicit schema upgrades described above preserve that graph state.

The independent `hierarchical_instances_v1` extension uses a 232-byte service
prefix at epoch 7. C# `TemplateNodeId` and `ResolveNode` distinguish local recipe
identities from live handles. Tick can resolve reserved members and stage supported
commands; ordinary reads still see committed membership. Control resolves only
committed members. Older 176–224-byte profiles remain supported. Schema revision
is 67; authoring-core v1 and protocol 1 remain unchanged.

Three native builds pass 56 selected regression checks, including complete
membership, animation rebind, rollback, references, saves, nested asset closure
and simulation-disabled authoring. Both managed ABI cohorts pass 75 checks each.
Four real compiled original-FBX instance cohorts pass 8,944 RPCs with eight clean
owners under Windows/Linux CoreCLR and Native AOT. Independent quaternion-chain,
joint-transform and weighted-geometry calculations check two physically moving
actors with different playback rates; save continuation, cancellation, failure,
reference repair, deletion and reload policy are also checked. Original source
sampling uses the native FBX importer; this is not an independent FBX decoder.

Generated player controller/camera admission passes on both laptop GPUs. A
relocated Windows Native AOT game runs after removing its owned source project,
spawns two complete characters from its compiled first Tick and submits 2,058
weighted vertices. Its readbacks match exactly with a separate
source-free runtime using the same compiled artifact. This comparison is separate
from the original-source pose oracle. Retained 176-byte Windows UI/save and
224-byte Linux locomotion artifacts still pass on the new host. Initial fixture,
build-path and missing UI-guard failures are retained with their corrections.
These are bounded correctness and scripted graphics checks, not crowd,
physical-input, GUI, clean-machine or game-scale performance qualification.
[Evidence](evidence/m2-runtime-instances.json). The [fixture instructions](../tests/managed_instance_gameplay/README.md)
provide reproducible compiled and exported checks; desktop prefab/variant authoring
and arbitrary child or authored-entity deletion remain separate work.

Version 0.0.77 adds a [frame-driven shared native player](LIVE_PLAYER.md).
`player.start` acknowledges a deferred graphics lifetime; `player.inspect`
reports readiness separately. Two independent clients can inspect, pause,
perform guarded edits or steps on an interactive runtime, and request fresh
captures through the same graphics context. Capture does not advance simulation.
Generation, identity and control guards protect owner changes; 32 bounded retry
receipts retain original command outcomes. Replay supports observation,
pause/resume and cancellation without accepting intervening runtime edits.
Save load resolves the replacement runtime through the stable owner adapter,
clears stale input/UI/audio state and pauses interactive presentation.
Stopping presentation retains the runtime and terminal report. Mutation APIs
require a shared headless host; standalone and desktop discovery omit them.

The owner finishes its accepted request batch before one player frame, without
recursive request dispatch inside simulation. The existing blocking player uses
the same native frame driver. GPU initialization, readback, audio, imports and
storage can still stall serialized requests; this is not a nonblocking or hard
latency contract. On-demand captures reserve swapchain transfer capability and
allocate readback staging lazily; ordinary player frames skip that copy.
Schema revision is 66; authoring-core v1, protocol 1 and gameplay service epoch 7
remain unchanged.

Three native builds pass 47 selected authoring, input, shared-session, discovery,
MCP, lifecycle and metadata regression checks. Two shared-player and two retained
Windows Native AOT UI cohorts pass 558 RPCs with 14 clean owners. Twenty-two
Windows Vulkan readbacks across AMD and NVIDIA cover the new lifecycle fixtures
and runnable observation recipe, including changed pixels after actual stepping
or compiled UI edits and exact saved/restored images. The separate native fixture
checks deferred cancellation, replay tick consumption, replacement and device
failure recovery. Existing blocking replay, targeted window-message, audio,
settings and runtime-replacement checks pass, as does the preserved 0.0.75 Linux
Native AOT locomotion game with 5,064 RPCs. Initial capture preparation and mock
camera failures are retained with their corrections. These are small scripted
fixtures, not physical-input, response-time or game-scale performance claims.
[Evidence](evidence/m2-live-player.json).

The [documentation coverage inventory](DOCUMENTATION_COVERAGE.md) maps supported
mechanisms to manual/reference entries, task recipes and checks. It explicitly
records missing human workflows, generated native/C# reference, searchable
versioned documentation and evaluated agent skills. Current documentation is
not a complete developer manual.

Version 0.0.76 adds [portable player settings](PLAYER_SETTINGS.md): eight
sparse, typed overrides for vertical FOV, pointer sensitivity/inversion, absolute
UI scale, MSAA and outstanding graphics submissions. Profiles have independent
revisions, preview/reset, persisted retry receipts and guarded file replacement.
They store intent outside authored worlds, input bindings and saved gameplay.
Player launch resolves inherited/profile/session/explicit-option precedence,
applies the existing native owners and reports effective values, sources and
presentation outcomes. Inherited FOV follows the final native camera; explicit
FOV does not mutate camera data. Semantic replay validates pointer preferences
without reinterpreting recorded controls. Packaged games load external profiles
without writer sidecars; live settings and a C# settings/menu API remain separate
work. Schema revision is 65; authoring-core v1, protocol 1 and gameplay service
epoch 7 remain unchanged.

Three native builds pass 33 selected input, world-session, discovery, authoring,
MCP and metadata regression checks. Four retained settings cohorts pass 28
protocol groups, 394 RPCs and 246 clean owners; the repeatable CTest protocol
gate passes in all three builds. Eight Windows Vulkan readbacks across AMD and
NVIDIA verify independent camera projection, exact authored-reference pixels,
absolute UI pixel bounds and actual sample/submission limits. A relocated
source-free Windows game consumes external preferences, honors explicit CLI
choices and rejects stale revisions and duplicate override keys without changing
its bundle or profile sidecars. The existing 0.0.75 Linux Native AOT locomotion
game passes 5,064 RPCs against the new runtime. These fixtures do not qualify
physical input, live menus, clean-machine installation or game-scale performance.
[Evidence](evidence/m2-player-settings.json).

Version 0.0.75 adds [Locomotion Yard](../examples/locomotion-yard/README.md),
a compiled character game using an untouched original CC0 FBX body and separate
Idle/Run takes. Explicit rotation retargeting preserves original target geometry,
rest data and inverse binds, with declared HipsCtrl/Hips position deltas.
The native capsule owns travel and navigation; a child visual wrapper aligns
forward axes and standing height without rescaling the source body. Both clips
advance and loop. Run speed follows horizontal displacement from the previous
committed tick, divided by an authored 3 m/s playback reference. Physical corner
slowdown exercises real rate changes. Rate-only corrections preserve the observed
clock and wait for active inertial fades to complete, because the existing
playback command replaces the full animation state.

Windows/Linux CoreCLR and Native AOT pass the original-source joint/weighted
geometry oracle, cumulative playback/wrap checks, native physical detour,
compiled controls and exact midfade same/fresh-owner checkpoint continuation.
CoreCLR compatible reload preserves live state; Native AOT replacement retains
its documented rejection policy. Windows captures observe idle, run and delivered
idle on both laptop GPUs. A relocated Windows Native AOT player starts at tick
zero after its owned authoring project is removed; a separate bundled-runtime
contract then verifies native courier poses, clocks and saves. The first player
report exposes courier observations through compiled state and renderer counts,
not direct courier joint/clock output. [Evidence](evidence/m2-locomotion-game.json).

The 3.76 m imported standing height is retained, with a 0.42 m capsule radius.
The 3 m/s rate reference is sample configuration, not automatic stride fitting.
The original Run closes its pose but is not velocity-continuous at the seam;
foot locking, IK, contact correction, loop repair, root-controller extraction,
motion matching and a broad character-exporter profile remain unfinished.
The original source decoder is the native importer; the rotation/FK/skin oracle
is separate math, and weighted vertices are calculated from actual runtime
joints rather than read directly from GPU buffers. These small fixtures do not
qualify physical input, editor usability, clean-machine installation or game-scale
performance. Authoring-core v1, protocol 1, schema revision 64 and gameplay
service epoch 7 remain unchanged. The new task playbook documents the verified
profile without changing the historical Character Yard example.

Version 0.0.74 adds [explicit rotation retargeting](ANIMATION_RETARGETING.md)
for matching named ancestry with differing proportions, reference stances and
positive nonuniform scales. Original reference quaternion chains define the
orientation transfer; target-reference local positions/scales stay fixed unless
selected source-local position deltas are explicitly requested. Target geometry,
rest defaults and inverse binds remain unchanged. Read-only `asset.source.inspect`
exposes original nodes, takes, reference poses and normalized-model fingerprints,
including geometryless donors. Required fingerprint guards reject changed
original models before filtering or retargeting. Focused discovery reports
schema revision 64; authoring-core v1, protocol 1 and native gameplay services
epoch 7 remain unchanged.

Each of three native builds passes eight independent retargeting groups and
six API tests, including original Kenney Idle, Run and Jump files. The combined
2,379 RPCs and 21 clean owners check quaternion chains, target FK and original
weighted geometry, strict policies, unpublished inspection, failure atomicity
and source-independent cooked persistence. Windows Vulkan checks pass 22
readbacks across AMD and NVIDIA using the original 1,029-vertex weighted body,
separate Run/Idle takes and 15 ordinary runtime ticks. The Run pose and runtime
match the independent normalized-source reference exactly on AMD; NVIDIA differs
at one pixel above channel error two, with maximum channel error 17.
One AMD Idle GPU/CPU pixel has channel error 13. These are bounded readback
comparisons, not universal rendering tolerances. Existing exact composition,
frame conversion, discovery, authoring and MCP checks pass, as does the preserved
0.0.71 Linux Native AOT character game. Automatic bone-name mapping, contact/IK,
stride and loop repair and root-motion extraction remain unfinished. The
compiled FBX locomotion game is qualified separately in 0.0.75. The source oracle uses original native-normalized
observations with independent retarget/FK/skin math, not an independent FBX
parser. [Evidence](evidence/m2-animation-rotation-retarget.json).

Version 0.0.73 adds opt-in [reference-pose bone-frame conversion](ANIMATION_FRAME_TRANSFER.md)
for separate clips with matching named ancestry, coincident reference joint
origins and exactly uniform mapped scales. Choose original rest or sampled
reference poses and an optional rigid component alignment. The converter
re-expresses translation/rotation curves and cubic tangents, synthesizes missing
constant properties where needed, and preserves target geometry, hierarchy,
rest transforms and inverse binds. Default exact-skeleton composition stays
unchanged. Focused discovery reports schema revision 63; authoring-core v1,
protocol 1 and native gameplay services epoch 7 remain unchanged.

Each of the three builds passes eight independent native conversion groups
and five API tests. The combined 570 RPCs and 18 clean owners compare known
joint matrices and original skin weights, check original reference selections
before take filtering, reject unsupported conversions without publication and
reopen cooked motion without source files. Windows Vulkan checks pass 16
readbacks across AMD and NVIDIA: GPU/CPU preview, independently calculated
geometry/normals and 30-tick runtime playback agree exactly in this analytic
triangle fixture. Existing exact-composition diagnostics, ASCII/binary FBX,
focused discovery, stable authoring and MCP contracts pass. The existing 0.0.71
Linux Native AOT Character Yard artifact also passes against the new runtime.
Different proportions, contact/stance retargeting and a representative locomotion
library remain unfinished; these checks do not establish game-scale performance
or physical input. [Evidence](evidence/m2-animation-frame-transfer.json).

Version 0.0.72 gives exact-skeleton animation-composition failures structured
`error.data`: the first incompatible donor, source-local node identities,
missing-node roles, component mismatch flags and local TRS measurements. The
report keeps complete counts and at most 64 issue records. Focused discovery
publishes its strict schema at revision 62, and the Python client preserves
the native payload. The [recovery guide](FBX_IMPORT.md#inspect-a-composition-failure)
explains the existing thresholds, normalized local-rest comparison and safe
next steps. It does not infer bind poses or retarget clips.

Linux runtime, native Windows runtime and simulation-disabled authoring each
pass eight native diagnostic check groups and six protocol tests. The combined
306 RPCs and 21 clean owners cover original analytic TRS inputs, missing nodes,
bounded records, same-owner recovery and fresh-owner persisted state. Failed
imports preserve authored state, history and the populated asset store.
Successful cooked composition remains unchanged. Existing ASCII/binary FBX,
scoped discovery and stable authoring-contract checks also pass. Preserved
0.0.71 Linux Native AOT Character Yard gameplay passes against the new runtime.
Eight real stdio MCP tests on each OS verify complete text/structured error
payloads, atomic rejection and same-owner recovery alongside existing contracts.
This checkpoint does not add locomotion clips or qualify retargeting, new GUI
controls or graphics performance. [Evidence](evidence/m2-animation-composition-diagnostics.json).

Version 0.0.71 adds [Character Yard](../examples/character-yard/README.md), a
compiled C# game using the original imported figure. Its camera-free capsule
plans around solid cover with native navigation; the attached rig retains all
22 source nodes and 19 joints. An explicit wrapper aligns the model's forward
axis with the controller. Movement advances the single source take, while
Gesture replays it once in place. The source motion opens the arms; it is not
a locomotion clip. Pause/resume, dispatch and save/load use real compiled
callbacks and native results. The sample authors a compact wine-styled footer.

Five matching-version cohorts pass on Linux CoreCLR/Native AOT and Windows
CoreCLR/Native AOT, with both laptop GPUs used for the native Windows captures:
7,772 RPCs, 40 check groups, ten clean owners and eight Vulkan readbacks.
Each cohort observes 21 stable source poses, with maximum joint-matrix error
1.065e-8 against an independent GLB calculation. Per-tick physics checks verify
detour, speed and arrival. Immediate checkpoints, grouped/unequally partitioned
ticks and fresh-owner continuation retain complete compared native, typed,
gameplay and UI state. Continued comparisons exclude only two named host
save-epoch identity fields in addition to session IDs. Compatible unchanged-DLL
reload passes; Native AOT replacement rejects without altering state.

Native UI layout checks establish all seven buttons' bounds and hittability at
960×640 and 512×288 in presenter-enabled Windows builds. Linux headless builds
exercise logical controls without claiming presentation. These are virtual
layout and scripted command checks, not physical mouse/controller qualification.

A separately published Windows Native AOT artifact runs from a relocated
61-file exported bundle after its owned source project is removed. The player
starts with empty gameplay values and completes delivery over 1,400 ticks on
the NVIDIA GPU, with no reported NVRHI errors or runtime replacements. Its
source-free run precedes a separate 1,552-RPC pose/save contract using the bundled
runtime and newly authored content. The six bundle-verifier commands and three
owned native processes exit cleanly; bundle bytes and source attribution remain
intact. The host has .NET installed, so environment isolation and relocation do
not establish clean-machine distribution. This small flat yard does not qualify
locomotion libraries, retargeting, general save migrations or game-scale
performance. [Source, hashes and scope](evidence/m2-character-game.json).

Version 0.0.70 accepts bounded matrix-authored glTF nodes by decomposing them
into the existing positive-scale TRS profile. It preserves hierarchy and inverse
bind frames, reports converted nodes and rejects unsupported transforms or
matrix/TRS animation conflicts. The [transform policy](ASSETS.md#matrix-authored-transforms)
includes exact affine constraints and explicit numerical tolerances.

The [imported-character playbook](IMPORTED_CHARACTER.md) qualifies an unchanged,
licensed 19-joint human figure with independent source calculations, authored
rest frames, four native frozen-playback seeks and fresh-owner cooked closure.
Linux runtime, Windows runtime and simulation-disabled Linux pass 694 RPCs with
eight clean owner exits. Both laptop GPUs pass 24 Vulkan captures; the largest
GPU/reference error is one edge pixel. Native tests include unequal scales,
180-degree rotations, nested static/skinned frames, source-independent decode
and numerical-boundary admission. Existing asset, animation, FBX and stable
authoring-contract regressions pass separately. [Evidence](evidence/m2-gltf-matrix-intake.json).

This is bounded character intake and observation. Compiled character gameplay,
save/export continuation, locomotion clips, retargeting and broader exporter
compatibility are not established by this checkpoint.

Version 0.0.69 integrates a shared [native job executor](JOBS.md) into animation
sampling and procedural material baking. Each creating owner lazily obtains a
bounded pool, with two workers by default and an explicit 0–8 worker policy.
Frame/background admission is separate, and one worker is reserved for frame
work when at least two exist. Registry writes, Jolt physics, C# callbacks,
material file publication and renderer recording remain on their owners.

Animation workers sample independent rigs into private candidate histories and
poses. Owner assembly preserves stable ordering; failure or cancellation cannot
commit a partial sample. Material callbacks own frozen inputs and CPU outputs;
owner cancellation can veto an already-completed result before publication.
Terminal groups fence capture cleanup before releasing admission capacity.
Named worker intervals preserve source/session/tick/parent and actual OS thread
IDs in profiler inspection and trace export. Late records cannot enter a new
recording.

Five integrated native consumer suites pass on Windows and Linux, including
15 scheduler groups and exact serial/0/1/2/4/8-worker animation comparisons.
Linux ASan/UBSan and TSan pass scheduler and worker-profiler checks. Both OSes
pass the existing compiled layer fixtures under CoreCLR and preserved Native
AOT: 2,028 RPCs, 32 check groups and 16 clean owner exits in total. The NVIDIA
GPU passes 36 original-fixture Vulkan captures against 18 exact independent pose
references, with zero reported NVRHI errors. The simulation-disabled build also
runs the installed-client guarded-editing playbook and the manual's C++ DAG
example. These are bounded correctness checks, not production game-scale or
physical-input qualification. [Jobs evidence](evidence/m2-native-jobs.json).

The [task playbook index](PLAYBOOKS.md) names current prerequisites, completion
checks and recovery guides. Agent skills remain a later compact entry point to
qualified workflows; comprehensive manual/reference coverage is a release
requirement rather than a claim of present completeness.

Version 0.0.68 adds bounded [FBX model import](FBX_IMPORT.md), including static
and linear-skinned geometry, metric/axis normalization and transform takes.
Separate animation sources can select and rename clips through exact normalized
hierarchy/rest-frame matching. This is not retargeting; mismatched ancestors or
bone bases reject. The profile rejects unsupported deformation/materials and
invalid attribute indices, reports narrowly permitted collapsed-UV tangent
repairs, and preserves the default glTF cooked identities.

Windows and Linux each pass 36 original fixtures in both ASCII and binary
encodings, 27 asset/animation/authoring-core regression tests, and 1,067 scoped
schema responses. FBX protocol checks pass seven Linux tests over 194 RPCs and
six Windows tests over 183 RPCs; the POSIX invalid-byte filename test is skipped
on Windows. All 17 recorded FBX owners exit cleanly. Staging recovery preserves
pre-existing files/links, and filesystem errors remain bounded UTF-8 diagnostics.
Six Windows Vulkan captures on the AMD integrated GPU match CPU and independent
geometry references, including 30 runtime ticks after source removal and a fresh
owner. General exporter compatibility, retargeting, compiled FBX-character
gameplay and game-scale import/render performance remain unqualified.
[Recorded evidence](evidence/m2-fbx-import.json).

Version 0.0.67 separates base API schemas from the world owner. Both runtime-enabled
Linux and renderer/UI-enabled Windows builds preserve all 1,067 scoped discovery
responses byte for byte, including read-only and shared-editor filtering. Fresh
standalone/shared CLI sessions preserve complete replies, state, history and
world bytes, then shut down and reopen cleanly. Nine selected Linux CTest cases
and seven Windows native/contract commands pass. Compilation graph probes
isolate each source to its own unit without changing compiler flags. Recorded
command timings are observations, not a controlled speedup benchmark.
[Build details](BUILD.md#world-service-and-schema-compilation),
[evidence](evidence/m2-world-schema-extraction.json).

The 0.0.66 Windows desktop passes 232 scripted editor actions and four restart
checks, plus 97 independent dual-view actions and three restart checks. Both
Scene and Game render on the RTX 4070 Laptop GPU with zero reported NVRHI
errors. The README now shows an inspected OS capture of this editor. These are
semantic-input checks; physical-device testing and wine branding remain pending.
[Desktop evidence](evidence/m2-desktop-current.json).

Version 0.0.66 adds immutable [asset source/license records](ASSET_PROVENANCE.md)
and guarded world selections. Frozen runtime/save sources retain their selected
records; component upgrades preserve those selections. Exports carry checked
records and deterministic credits, reject notice tampering and require a
runtime advertising provenance support. Explicit unused mappings retain their
records without adding unused cooked assets. Unmapped legacy bundles retain
their earlier shape.

Actual Windows and Linux checks each pass seven authoring/save groups over
238 RPCs with 12 clean owners, six export groups over 53 RPCs with six clean
owners, and five native programs. Linux additionally passes ten selected
regressions; Windows passes the stable authoring-core and identity suites.
Records are caller declarations, not verified permission or importer-captured
source traces. The export fixtures deliberately use non-executable runtimes;
this does not qualify player deployment, online acquisition or graphics
performance. [Recorded evidence](evidence/m2-asset-provenance.json).

Version 0.0.65 records qualification of the original compiled navigation fixture
in an exported, relocated Windows Native AOT game using the 0.0.64 runtime.
The player loads gameplay at tick zero and
completes 1,400 Vulkan replay ticks: one complete six-corner plan, NPC arrival,
zero NVRHI errors and no runtime replacement. The owned source is removed and
the bundle's exact runtime, artifact and navigation inventories remain unchanged.
The original fixture has no visible meshes; this qualifies the hardware player
lifecycle and compiled planning/arrival, rather than a visible-NPC demonstration.

Separate fresh worlds use the same bundled runtime and artifact for eight checks
over 987 RPCs, including native pose detours, query quotas, rollback, saves,
fresh-owner continuation and replan. All native commands and owners exit cleanly.
The recorded runtime and Native AOT artifact are both from 0.0.64. This does not
qualify clean-machine deployment, physical input, crowds,
performance, GUI authoring or in-player save UX. [Shipping evidence](evidence/m2-navigation-shipping.json),
[reproduction](../tests/managed_navigation_gameplay/README.md#exported-windows-player).

A default-off [Linux development option](BUILD.md#local-linux-thin-archives)
changes only the core static archive's representation. It retains compiler
optimization/debug settings and dependency archives. The speed measurements
cover archive creation/indexing on one filesystem, not compilation, general
iteration latency or game performance. Automatic online acquisition and importer-captured source traces remain
planned; [owned-file imports](ASSETS.md) are available for the documented formats.

Version 0.0.64 binds static navigation into compiled NPC gameplay. A guarded
`navigation.set` transaction associates the world with a checked `.pnav` asset.
Binding, runtime freeze, export and saved-source restore validate the relevant
static geometry and package bytes. Unbound worlds retain their earlier content
shape; binding changes participate in preview, retry, history and undo/redo.

C# `INavigationGame` enables live-character route queries during Tick. The
independent named extension appends a callback to the 224-byte services-7 view,
preserving the existing 176/192/208/216-byte profiles. Queries use an immutable
mesh and bounded scratch, with eight native attempts per Tick and explicit
incomplete-route statuses. Games own their route/cursor in existing components
and steer through ordinary character input; navigation does not teleport actors.

CoreCLR and actual published Native AOT each pass ten compiled follower checks
over 1,009 RPCs with five clean owner exits on Linux, and nine checks over 998
RPCs with four clean exits on Windows. The fixture physically detours
around cover, reaches its goal, enforces query limits, rolls back a late failed
batch, and resumes a persisted route in a fresh owner. CoreCLR checks compatible
reload; Native AOT checks replacement rejection. Six project tests verify content
and license closure using metadata-only runtime/artifact fixtures, separately
from real compiled gameplay execution on both operating systems. [Navigation contract](NAVIGATION.md),
[compiled fixture](../tests/managed_navigation_gameplay/README.md),
[evidence](evidence/m2-runtime-navigation.json).

Navigation remains a static ground-planning foundation. Dynamic obstacles,
crowds, traversal, streaming and editor navigation tools are unfinished; these
fixtures do not establish game-scale performance or clean-machine deployment.

Version 0.0.63 adds optional static navigation through pinned Recast/Detour.
Revision-guarded native baking, inspection and path queries use authored static
box and indexed mesh colliders, including physical holes. Profiles control
capsule clearance, slope, climb and voxel resolution. Routes distinguish complete,
partial, unreachable, output-budget and node-budget results, with explicit
endpoint projections. Content-addressed packages contain checked neutral arrays;
loaded meshes are immutable and source fingerprints reject stale topology.

Windows and Linux each pass native navigation checks and six protocol groups
over 70 RPCs, with eight clean native-owner exits per platform. A disabled build
passes availability checks while intentionally skipping five enabled-only groups.
Selected regressions cover eleven Linux groups, ten authoring-only groups and
ten Windows execution groups; the existing CoreCLR patrol sample also passes
its four gameplay checks. Native regressions reproduce and fix partial mesh
initialization cleanup, malformed-path diagnostics and the reserved neighbor-bit
polygon boundary. Discovery revision 57 retains protocol 1, authoring-core v1,
existing gameplay service profiles and save formats.

The 0.0.63 checkpoint covers authored ground-navigation infrastructure; runtime
binding and compiled queries were added in 0.0.64. Patrol Room still uses its
authored route and direct steering. Moving obstacles, avoidance,
crowds, traversal links and streaming remain separate work. Endpoint projection
uses a bounded linear scan; bake/query memory admission limits are not a game-scale
CPU or latency guarantee. [Navigation API](NAVIGATION.md),
[evidence](evidence/m2-navigation.json).

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

[Explicit save upgrades](SAVE_UPGRADES.md) connect stable-ID mapping, authored-content validation and snapshot transformation to guarded external `save.load`. The earlier scalar checkpoint qualified Linux CoreCLR continuation, component changes, rejection isolation and recovery; its [integration record](evidence/m2-save-upgrade-integration.json) retains that scope. Hierarchical and collection evolution has separate 0.0.79 qualification above. Automatic gameplay/editor upgrade selection remains unfinished; ordinary restores remain exact. The earlier cross-platform [primitive evidence](evidence/m2-save-upgrade-primitives.json) covers mapping/plan guards only.

The latest development [C# lifecycle checkpoint](GAMEPLAY_LIFECYCLE.md) passes five compiled integration groups under both CoreCLR and Native AOT on Linux and Windows. Agent-service tests cover guarded births, durable restoration and continued gameplay; the CoreCLR path also verifies compatible reload with spawned entities. This is root-prop support, not general entity/component lifecycle. [Qualification evidence](evidence/m2-managed-lifecycle.json).

[Custom components](CUSTOM_COMPONENTS.md) provide native-owned per-entity data with stable IDs and generated C# accessors. Spawned root props can carry template-defined components during Play; development [bounded collections](COMPONENT_COLLECTIONS.md) now pass Linux native and real CoreCLR gameplay/save tests. Windows CoreCLR and Native AOT capacity-four entity/int32 buffers are now qualified separately. Arbitrary component addition/removal on existing entities and general schema/save migrations remain unfinished. Services ABI 7 now has a bounded compatibility contract; upgrading from earlier epochs still requires matching rebuilt artifacts.

The development [standalone template catalog](RUNTIME.md#standalone-template-catalog-development) adds transactional root-prop recipes without requiring live prototype objects. Runtime catalog inspection uses a frozen authored revision, and template-only models/textures participate in content dependencies. A guarded world-service transaction can create and remove spawned root props at paused boundaries, with session-scoped retry receipts. Version 3 snapshots preserve those spawned objects and generated-ID history. Native hosts can schedule guarded structural edits within an atomic multi-tick batch. The [C# lifecycle API](GAMEPLAY_LIFECYCLE.md) reserves IDs, initializes registered components, starts kinematic movement and removes spawned props within Tick. It uses the services ABI 7 baseline (176 bytes); earlier service epochs require coordinated rebuilding. RPC tick scheduling, arbitrary authored-entity removal, desktop template authoring and a dedicated generated-prop hierarchy/Inspector remain unfinished. [Service and desktop lifecycle evidence](evidence/m2-runtime-lifecycle-service.json). Tick-guarded RPC mutations also require the observed structure revision after membership changes; editor component, gameplay and animation drafts retain it. Linux and Windows pass the focused guards and save regressions, and Windows passes five native desktop checks. This checkpoint does not qualify the full GUI or activate a new desktop package. [Structure-guard evidence](evidence/m2-runtime-structure-guards.json).

[Keyboard/mouse profiles](INPUT_PROFILES.md), [gamepad profiles](GAMEPADS.md), configurable bindings, device discovery and hosted editor assignment are implemented. Physical gamepad qualification remains outstanding. General settings remain unfinished. The experimental [save-slot service](RUNTIME.md#durable-save-slots) adds durable generations, explicit recovery and guarded runtime replacement to the native snapshot foundation. The [editor Save/Load window](EDITOR_SAVES.md) now uses that service. [Typed gameplay requests](GAMEPLAY_SAVES.md) use a post-batch native owner. General migrations, autosave policy and asynchronous snapshot capture remain unfinished. Opt-in background gameplay writes have their own [qualification](GAMEPLAY_SAVES.md#background-write-implementation). Authored document recovery and internal runtime rollback are separate contracts.

Cinematic authoring, comprehensive post-processing and a general package manager are not implemented. Bounded [FBX import and exact-skeleton clip composition](FBX_IMPORT.md), reference-frame conversion and [explicit rotation retargeting](ANIMATION_RETARGETING.md) are supported; general Mixamo/exporter compatibility and automatic anatomical/contact retargeting remain unqualified. Supported asset formats and limits are documented in [Assets](ASSETS.md) and [Animation assets](ANIMATION_ASSETS.md).

The [native profiler](PROFILER.md) now provides bounded shared captures, an editor CPU timeline, subsystem summaries, native job-worker intervals, separate GPU duration samples and trace export. [Optional CoreCLR callback aggregates](MANAGED_CALLBACK_METRICS.md) provide current-thread allocation and runtime-wide GC observations separately from native captures. General worker-stack sampling, arbitrary managed allocation/call-stack tracking, GPU residency, calibrated GPU-clock correlation and a sustained benchmark workflow remain unfinished. Experimental [GPU memory diagnostics](GPU_MEMORY.md) separately expose allocation estimates and budgets; they are not residency measurements. Existing [render diagnostics](RENDER_DIAGNOSTICS.md) also retain capture/player aggregates and draw counters. Scene transform handles are implemented; orientation/axis-view controls, camera frustums and light/component icons remain planned. The current Console is a log tab beside Project.

Planned editor workflow improvements include configurable hierarchy/folder styling, component isolation/search/copy-paste, focused object/asset tabs, persistent favorites, fullscreen panels, configurable smooth navigation, hover highlighting and overlap selection, a live preferences inspector, clickable breadcrumbs/back-forward history/context locking, and integrated Git/GitHub workflows. Existing striping, automatic icons, basic geometry picking and dock panels cover only the initial subset.

Runtime collision supports boxes, capsule controllers and explicit [static triangle meshes](MESH_COLLISION.md), preserving openings present in source geometry. Moving/deforming mesh colliders, distinct movement versus weapon-query shapes, finite-radius projectile sweeps and texture-cutout collision masks remain unfinished. Contacts use triangle front faces; rays hit both sides. Numeric/resource bounds and synthetic fixture results do not establish exact arithmetic or game-scale collision performance.

Animation supports bounded two-pose crossfades, opt-in native inertial transitions and ordered masked override/additive layers, including opt-in compiled C# controls. Legacy fade interruption freezes the current pose. Inertial mode adds decaying corrections that preserve estimated distinct-tick output motion; it does not guarantee smooth clip discontinuities or foot contacts. [Explicit two-bone IK](TWO_BONE_IK.md) and [planar root motion](ROOT_MOTION.md) share the committed pose, physics, checkpoint and rollback boundary. State machines and transition-history scrubbing, automatic grounding/full-body IK, vertical root locomotion and automatic anatomical/contact retargeting remain unfinished. Internal batch rollback is not retained simulation history or a time-travel debugger.

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

- Native keyboard/mouse and gamepad profiles with alternate bindings, sensitivity/inversion, radial stick deadzones/response, trigger hysteresis, atomic profile edits, persistent retry receipts and isolated event evaluation. SDL gamepad discovery, single-player device assignment and attachment/focus/disconnect handling feed the same evaluator as headless traces; semantic replay remains independent of bindings. Action contexts, chords, analog named values, in-game rebinding UI, haptics and general settings menus remain unfinished. [Input profile contract](INPUT_PROFILES.md), [gamepad contract](GAMEPADS.md).

- glTF and bounded FBX skin/curve packages, paginated key/joint/pose inspection and CPU reference deformation with isolated Vulkan pose capture. A [compute skinning pass](GPU_SKINNING.md) feeds the shared material and shadow renderer. Editable rig/node bindings connect compiled curve sampling to fixed-tick runtime playback, independent instance clocks, atomic commands, bounded crossfades, pose/physics rollback and immutable live palettes. Native masked layers add independent clocks, transitions and persistent weights, with opt-in compiled C# layer controls. Explicit import-time retargeting, native two-bone IK and planar root motion have their own bounded contracts; automatic anatomical/contact retargeting and general full-body locomotion remain unfinished. [Runtime animation contract](RUNTIME_ANIMATION.md), [animation asset contract](ANIMATION_ASSETS.md), [FBX profile](FBX_IMPORT.md).

- Native mono WAV import and content-addressed audio packages, editable acoustic materials/emitters, and optional Steam Audio direct-path/HRTF capture from authored or live poses. Linux/Windows tests measure actual PCM, dynamic-door obstruction, delayed arrival, directional cues and mixing; the C# use action changes the observed acoustic path. Native logical voices, rollback-safe C# play/stop, persistent direct/HRTF streams, temporal replay recording and optional SDL3 player output and a separate bounded [editor DSP worker](EDITOR_AUDIO.md) are now implemented; reflections, production scheduling and the full environmental system remain unfinished. [Event/output contract](AUDIO_EVENTS.md). [Native audio contract and evidence](AUDIO.md).

- Optional C# gameplay integrated with the native runtime: one `Game<TState>`, typed native-owned fields, compact agent inspection/editing, same-tick use/raycast/kinematic interaction, compatible-field reload and joint gameplay/physics batch rollback. Linux and Windows pass 100 reloads plus real failure and retained-context checks; both laptop GPUs pass the scripted door replay. Full component/event APIs and production deployment qualification remain unfinished. [C# API and evidence](MANAGED_GAMEPLAY.md).

- Native collider raycasts with stable entity hits and ignore filters, timed kinematic targets that persist across input batches, live progress inspection and replay. Door collision, child poses, dynamic-body pushing and real failure rollback are checked; The optional C# module now uses this surface for scripted use-button behavior. [Physics interactions and evidence](PHYSICS_INTERACTIONS.md).

- Conservative object frustum culling for the camera and each shadow view, draw counters and optional CPU/64-bit GPU timestamps on capture/play. Exact pixel comparisons retain offscreen casters and moving runtime shadows on both laptop GPUs. Timings describe these small fixtures, not qualified game performance. [Visibility and render diagnostics](RENDER_DIAGNOSTICS.md).

- Optional cascaded directional, six-face point and spot shadow maps, bounded resolution/storage, receiver-plane filtering and moving runtime casters. Rendering selects the default nine-tap reference or opt-in fractional comparison filter; the latter has bounded directional-character checks on one GPU. GPU occlusion and continuous shadowed replay are checked against independent observations. Per-drawable casting and PBR receiving are independently selectable, with default-true omission. Caching, physical penumbras, ray-traced/virtual shadows and performance qualification remain outstanding. [Shadow contract and evidence](SHADOWS.md).

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
