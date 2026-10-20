# Road to Alpha 1 — 1.0.0-alpha.1

Alpha 1 is the first usable release for building small 3D single-player games
with agents, with a desktop editor available for inspection and manual work.
It is a milestone toward the full engine, not completion of its graphics,
simulation or platform ambitions. Version numbers count development checkpoints;
they do not measure completion.

Pre-alpha development checkpoints use `0.x.x`. The first alpha release will be
tagged `1.0.0-alpha.1`, followed by further alpha candidates, then
`1.0.0-beta.N` and `1.0.0-rc.N` before `1.0.0`. The suffix identifies maturity;
the alpha number counts candidates rather than completed roadmap phases.
The [full release roadmap](ROADMAP.md) defines the wider destination and beta
entry criteria. Alpha 1 is an intermediate usable-game milestone.

## Release gates

| Gate | Current evidence | What must be closed for Alpha 1 |
| --- | --- | --- |
| Agent authoring | Recorded Codex/Claude games include [DEPOT RUN](evidence/fixtures/agent-depot/README.md) and [Switchback Range](evidence/fixtures/agent-switchback/README.md). Switchback's public source rebuilds, publishes, adopts and exports; thirteen independent native groups verify gameplay and full checkpoint continuation. Focused discovery, transactions, guards, retries, observations and replay have bounded evidence. [Workflows](AGENT_CLIENTS.md). | Repeated Switchback workflows and the selected combat, puzzle/adventure and management matrix now have qualifying fresh-project, independent gameplay/save and relocated-export evidence under their documented input contexts. Preserve those workflows through the Alpha candidate regression, with machine-readable results and actionable errors. Earlier failed workflows remain failed; the wider contract, content, interaction, performance and delivery gates stay separate. |
| Contracts and persistence | Selected [authoring-core v1](AUTHORING_API_COMPATIBILITY.md) is stable; C# components, reload, saves and bounded upgrades have recorded qualification. [Runtime game-session v1](RUNTIME_GAME_SESSION_V1.md) releases a separate 19-method running-game boundary with frozen request/result artifacts and retained-consumer checks; the older prospective 21-method gate remains separate. The [gameplay/content/save profile](ALPHA_GAMEPLAY_PROFILE.md) defines the current boundary and [Relay Yard upgrade](../examples/relay-yard/UPGRADING.md) exercises a retained real game. The [paused CoreCLR iteration cohort](evidence/m2-relay-yard-iteration.json) verifies 20 compatible edits with exact 50-field/native-state preservation, stale/type rejection and changed behavior; its 7.599 s p95 misses the 3 s target. The later [0.0.106 comparison](evidence/m2-iteration-comparison.json) preserves the full checked state with qualified batched reads and reaches 3.759 s p95. An independent [native 0.0.108 journal cohort](evidence/m2-iteration-journal.json) passes 20 full-state edits with 3.004 s p95. A fresh [native 0.0.156 cohort](evidence/m2-relay-yard-iteration-156.json) passes 20 edits at 2.206 s p95 with complete current native-state preservation, meeting the selected warm-iteration budget. These are independent cohorts, not a matched causal comparison. Exact old-image saves remain separate from upgrades. [Defaulted new-component upgrades](evidence/m2-defaulted-component-additions.json) retain a real CoreCLR game's complete old state, complete its objective and reopen the target save in a fresh process. | Preserve the released authoring/session contracts and their compatibility gates. Qualify the remaining selected Alpha workflow and upgrade boundaries. Extend retained-game coverage where supported component/content changes are needed; clearly identify experimental APIs. Alpha does not freeze every engine API. |
| Content and characters | Bounded FBX/glTF intake, images, WAV, cooked assets, provenance and native animation have evidence. [Character Yard](../examples/character-yard/README.md) covers weighted GLB; [Locomotion Yard](../examples/locomotion-yard/README.md) covers original FBX body/takes, controller-driven idle/run, checkpoints and relocated Windows export. [Opt-in material witnesses](FBX_IMPORT.md#locate-an-unsupported-material) identify selected rejected source nodes, local slots and animation donors; [qualification](evidence/m2-fbx-material-profile.json) retains the separate failed Linux discovery gate. The [focused-discovery follow-up](evidence/m2-conditional-discovery.json) passes its corrected sixteen-test Linux cohort while retaining the earlier failed records. | Publish the supported character intake/rig/material profile and actionable unsupported-content diagnostics. Extend beyond the single pinned FBX pack; integrate this path into the fresh-project agent workflow. The [multi-take character intake](CHARACTER_LIBRARY.md) adds an unchanged original GLB library, complete curve/geometry checks and relocated cooked reopening. A separate [compiled consumer and standalone service export](evidence/m2-character-library-locomotion.json) checks locomotion and complete saved-state continuation using that intact library. [Explicit IK and imported-scale profiles](IK_METRIC_PROFILES.md) have controlled-pose evidence on the unchanged original library. [Automatic ground contacts](GROUND_CONTACTS.md) add bounded actual surface and save/rollback evidence for original Idle/Running_A poses. [Live placement controls](evidence/m2-grounding-controls.json) add real jump/landing, compiled C# policy and selected original-v7 save continuation checks. Foot locking, broader exporter families and fresh agent intake remain open. |
| Playable systems | Physics, character controls, static navigation, keyboard/mouse/gamepad profiles, [portable and live native preferences](PLAYER_SETTINGS.md), styled game UI, audio, checkpoints and compiled callbacks exist. [Relay Yard audio](evidence/m2-relay-yard-audio.json) combines licensed gameplay cues with continuous playback and active-sound checkpoints. [Traversal Yard](../examples/traversal-yard/README.md) combines steps, crouch clearance, a physical lift, objectives and checkpoints; compiled and relocated cooked-world runs complete its course. | [Relay Yard](../examples/relay-yard/README.md) exercises native pickups, animation/navigation, menus, settings, exact partial saves and fresh-process game completion. Representative performance and additional fresh-project workflows remain separate. Close blocking focus, input, text/layout and lifecycle defects; verify physical mouse/keyboard/controller interaction on Windows and qualify listening on actual audio hardware. |
| Observation and editor | Vulkan captures, CPU/GPU profiling, independent Scene/Game panels and typed inspection have evidence. [Displayed UI inspection](PLAYER_UI_OBSERVATION.md) adds native geometry/frame tags, with paused AMD and advancing NVIDIA replay checks; focused interactive input remains open. [Relay Yard iteration](evidence/m2-relay-yard-iteration.json) preserves same-context captures across compatible code reload and a separately authored edit. That historical authored edit requires explicit stop/start. [Guarded scalar material adoption](LIVE_MATERIALS.md) and [live texture adoption](LIVE_TEXTURES.md) have native, paired-render, same-window and retained-game evidence. The [0.0.158 scalar cohort](evidence/m2-live-material-iteration.json) meets the selected one-second target at 0.660 s p95 across twenty edits in the full Performance Yard. The [0.0.159 encoded-texture cohort](evidence/m2-live-texture-iteration.json) meets its selected target at 0.909 s p95, including decoding and cooking. General live scene apply remains unqualified. [Deferred selection reconciliation](evidence/m2-editor-selection-reconciliation.json) has production-linked model and actual unattached native-bridge checks; displayed/physical interaction remains separate. | Keep visual observations and semantic state aligned through edits, Play, reload and recovery. Close blocking scene selection/navigation/docking defects; provide legible defaults and the agreed wine branding. Editor polish must support the workflow without replacing core work. |
| Performance and delivery | Windows rendering and Linux headless fixtures run on the development laptop. [Bounded frame/memory capture](FRAME_PERFORMANCE.md) has native NVIDIA/AMD integrity probes. [Performance Yard](evidence/m2-performance-yard.json), the [AO comparison](evidence/m2-ao-optimization.json) and [camera-report comparison](CAMERA_OBSERVATION.md) record short workload results; AMD still misses p95. A [fixed-image reliability cohort](evidence/m2-performance-yard-reliability.json) using the retained 0.0.97 engine passes 40 checkpoint cycles, 1,801 cumulative unpaused seconds and fresh-process continuation. Its 12 sampled NVIDIA windows meet timing budgets; process-memory growth remains under review. The [earlier 0.0.96 failure](PERFORMANCE_YARD_RELIABILITY.md#first-longer-attempt) and unknown stall cause remain retained. The [current CoreCLR cohort](evidence/m2-relay-yard-iteration-156.json) meets the selected warm edit-to-first-verified-Tick target at 2.206 s p95 across 20 real edits; the historical 3.004 s record is unchanged. The [scalar-material cohort](evidence/m2-live-material-iteration.json) meets its selected target at 0.660 s p95, with verifier preparation recorded separately. No matched or causal end-to-end improvement is qualified. The [encoded-texture cohort](evidence/m2-live-texture-iteration.json) also meets its selected one-second target at 0.909 s p95. The [0.0.169 mixed CoreCLR cohort](PERFORMANCE_YARD_MIXED_RELIABILITY.md) qualifies twelve warm save/load cycles, six compatible code reloads, sixteen live material/texture adoptions and cold continuation with 1,801.192437 primary and 1,872.608762 total unpaused seconds. Independent review verifies 76 complete snapshots, 31 whole-file pairs, 22 full edit forecasts and 2,018 OS queries, with sixty fixed memory windows analyzed. Memory-growth causes and matched observer cost remain unqualified. Production scale, interactive edit coverage and the broader Alpha reliability/delivery gate remain open. Native bundle workflows and [installed NativeAOT/CoreCLR authoring](AUTHORING_TOOLS.md#qualification) have bounded qualification; clean-machine setup remains open. | Publish representative scene/game frame-time, memory and edit-to-play measurements with workload/settings/hardware identified. Verify a fresh installation and relocated exported game, complete dependency notices, reproducible instructions and a full alpha regression run. Tiny fixtures alone do not pass this gate. |

No gate is fully closed yet. Several foundations are implemented, but their
combined use, supported boundaries and delivery still need qualification.
The [implementation status](IMPLEMENTATION_STATUS.md) remains authoritative for
individual capabilities. A checklist row is not a claim that every feature
listed there is complete or production-ready.

The [0.0.200 hierarchy comparison](evidence/m2-runtime-hierarchy.json) preserves
complete state on the fixed 2,397-entity compiled workload across six Linux
headless owners. Two profiled pairs reduce mean inclusive native batch wall
time by 13.0% in Debug; separate warm/cold/relocated game checks also pass with
their original admission deadline. This is CPU runtime progress, not new
rendered frame-time, memory-growth, Windows, fresh character-intake or overall
Alpha qualification. The earlier failed full-game run retains its verdict.

The [0.0.201 animation rate controls](ANIMATION_RATES.md) let CLI and C# gameplay
retime base/layer playback during active fades. An original-character workflow
preserves all 26 gameplay fields through rollback and cold continuation; a
separate retained game also passes its original continuation checks.
[Qualification](evidence/m2-animation-rates.json) is headless: rendered gait,
foot locking and the separate fresh agent character-intake gate remain open.

The [Knight character-route fixture](evidence/fixtures/agent-character-intake/README.md)
adds a separately compiled portable-source export and independently verified
native gameplay, full checkpoints and relocated continuation. Its original
fresh agent attempt remains failed after two marker centres missed the frozen
placement requirement. The assisted correction and later rebuild do not close
the fresh character-intake requirement. [Evidence](evidence/m2-agent-character-intake.json).

The [0.0.205 fresh workflow](evidence/m2-agent-character-owned-workflow.json)
qualifies owned process closure and selected source/publication/export provenance
with runtime 0.0.204. Its original candidate fails the unchanged pad-placement
check and remains unmodified. The task permits optional capture, but its two-frame
hardware capture differs from the evaluator's intended CPU-only resource scope.
It does not close fresh character intake,
independent gameplay or the broader Alpha gates.

The 0.0.131 [fresh Switchback checkpoint](evidence/m2-agent-switchback-fresh.json) passes its original agent supervisor and thirteen independent gameplay groups with full save continuation, including the exported service with the original project unavailable. Separate Ready/Idle/Run Vulkan observations pass with the original project unavailable. The [0.0.135 portable-source rebuild](evidence/m2-agent-switchback-fresh-rebuild.json) also passes all thirteen groups and complete save comparisons with a newly compiled module using native 0.0.134. That checkpoint qualified one fresh provider run and preserved the earlier failures; the later repeated-workflow result is recorded below.

The [additional Switchback evaluation](evidence/m2-agent-switchback-repeat.json) independently passes source and relocated readonly gameplay, full save comparisons and rendered Idle/Run observation using native 0.0.134. Its original fresh workflow remains failed after incomplete historical owner proof and a forced Script Job cleanup. Those separate game results did not supply a second successful fresh workflow; the repeated-workflow criterion remained open at that checkpoint.

The [0.0.139 second fresh workflow](evidence/m2-agent-switchback-fresh-repeat.json) separately passes its original supervisor and independent source, readonly export and Vulkan checks. Together with 0.0.131, it supplies bounded repeated fresh-workflow evidence for the same frozen task. Its generic SDK/docs/lifecycle guidance differs from the earlier run and is disclosed; this is neither a controlled success-rate experiment nor a full Alpha qualification. Earlier failed workflow verdicts are preserved.

### Qualification target

Alpha 1 needs three compact games with different gameplay structures, rather
than one reference game alone. Use the following acceptance matrix:

| Game family | Required composition |
| --- | --- |
| Combat | Movement, imported animation, aiming, damage, death/retry and checkpoints. |
| Puzzle/adventure | Interactions, inventory, UI and saved world changes. |
| Simulation/management | Multiple entities, bounded collections, spawning and retained progress across a supported save-schema update. |

Each game starts from a fresh project and reaches a playable standalone Windows
export. Across the three workflows, both Codex and Claude must complete at least
one qualifying run. Freeze each task's functional requirements, assets, budgets
and allowed assistance before its run; do not supply its game implementation.
Independently verify gameplay, save/load and fresh-process continuation in the
source project and relocated export. Preserve unsuccessful attempts and separate
fresh-workflow results from assisted recovery and later source rebuilds.

These are small usability demonstrations, not three commercial games or a claim
of complete genre support. Existing fixtures may contribute only where their
actual recorded scope meets the selected requirements. They do not close this
matrix automatically. Keep one fixed representative game for comparable frame,
memory and reliability measurements; the three-game matrix separately checks
that the authoring workflow generalizes. The cross-engine benchmark is later
work and does not block this engine qualification.

Current bounded records cover combat through the [second fresh Codex Switchback
workflow](evidence/m2-agent-switchback-fresh-repeat.json), puzzle/adventure
through the [fresh Claude Pump House workflow](evidence/m2-agent-pumphouse.json),
and simulation/management through the [fresh Codex Beacon Foundry
workflow](evidence/m2-agent-beacon-foundry.json). Switchback exercises imported
animation, aiming, damage, death/retry and saves; Pump House adds inventory,
causal world mechanisms and full same-owner/cold checkpoint continuation.
Beacon Foundry adds a bounded native FIFO, real spawned work units and an
explicit V1→V2 save upgrade with warm and cold new-unit completion. Both agent
clients are represented, and the selected three-family matrix is qualified.

This closes the selected breadth requirement, not an entire Alpha release gate.
The separate contract, content, representative performance, physical-input,
editor and delivery gates remain open. Each workflow retains its own toolchain,
reproducibility and observation limits, and earlier failed attempts remain
failed. Return to the fixed representative performance/reliability workload;
additional genre demonstrations are not the next release requirement.

The initial alpha matrix is Windows native editor/player/rendering and Linux
native CLI authoring/simulation. Each needs a fresh-project regression and
source-independent cooked-content reopen. Linux desktop rendering, browser and
console deployment require later platform qualification.

The [Linux game-profile cohort](evidence/m2-linux-game-profile.json) separately
qualifies a fresh native project's physics and the full retained Performance
Yard CoreCLR game's save/continuation/cooked reopen. The fresh-project physics
check and retained-IL game are separate scopes; the complete Alpha gate remains
open.

Use a fixed, published small-game workload on the development laptop
(Ryzen 7 8845HS, RTX 4070 Laptop 8 GB, 16 GB system RAM). Record laptop power
mode, resolution, settings, asset/entity/light counts and cache state. Initial
acceptance targets are:

- 1080p at 60 genuinely rendered frames per second, without frame generation;
  p95 frame time at most 16.7 ms and p99 at most 25 ms during the declared
  steady-play workload. Report loading and save/reload spikes separately.
- Peak player memory at most 4 GB and VRAM at most 6 GB; editor plus engine
  processes at most 6 GB system RAM for the same project. These are alpha
  workload budgets, not promises for arbitrary user games.
- Compatible C# edit-to-first-updated-tick p95 at most 3 seconds; representative
  material/texture edit-to-visible-update p95 at most 1 second. Record cold
  builds and engine-code rebuilds separately.
- At least 30 minutes of play with repeated save/load, supported script reload,
  scene edits and fresh-owner continuation, with no crash, lost committed
  state, stranded process or unexplained continuing memory growth.

The current Relay Yard cohort meets the selected warm C# iteration budget,
and the NVIDIA scalar-material and encoded-texture cohorts meet their selected
edit-to-visible budgets. Other devices and the remaining targets still need
evidence at their declared scope. A workload must
exercise the shipped small game and its imported content; an empty scene or
deliberately undersized fixture cannot substitute for it. Budget changes need
an explicit technical reason and updated acceptance criteria.

Opt-in [process-memory timelines](MEMORY_TIMELINE.md) provide query intervals
and conservative phase association for further memory investigation. Their
instrumentation checks do not close representative memory-growth, VRAM,
reliability or recording-overhead gates.
The [background-game integration](evidence/m2-background-player.json) adds
two actual native-game owners, 29 process queries and full save continuation
over a short, complete-workload run. It leaves the focused frame budgets,
recording overhead, continuing memory growth and thirty-minute gate open.

[Managed callback metrics](MANAGED_CALLBACK_METRICS.md) add optional CoreCLR
allocation/GC observations for the mixed-reload investigation. Their bounded
fixture qualification does not close the representative-game, recording-overhead
or thirty-minute reliability gates.

[Passive managed memory snapshots](MANAGED_MEMORY.md) add context counts and
CLR heap estimates without forcing collection. Their compiled helper, strict
wire parser and bounded Windows RPC/save checks provide an observation
prerequisite for investigating the mixed-reload result. Memory-growth causes,
matched observation cost and representative reliability remain open.

The [0.0.168 CoreCLR integration](PERFORMANCE_YARD_CORECLR.md) exercises the
complete Performance Yard with actual compile/error/repair, compatible reload,
eight complete checkpoints and fresh-process continuation. Five fixed play
intervals pass conservative observed tick-cadence bounds and the 41-query memory
timeline passes. This closes the short full-workload CoreCLR consumer prerequisite;
it does not close the thirty-minute mixed-reliability or wider performance gate.

## Implementation order

1. Extend the qualified character/content consumer to representative imported
   locomotion and the supported rig/clip profile.
2. Close the practical agent/gameplay gaps revealed by that game, including
   UI, settings, audio and save continuation.
3. Qualify repeated fresh-project agent workflows and define the supported
   alpha contracts and upgrade behavior.
4. Measure a representative scene and game; fix material performance and
   reliability problems before freezing the alpha candidate.
5. Finish blocking editor issues, branding, installation and release checks.

Independent source/test/review work can run in parallel. Heavy native builds
and GPU qualification are scheduled to fit the development laptop. Findings
may change the order when they expose a shared dependency or blocker.

## Continuing beyond Alpha 1

Advanced lighting and graphics, production VFX and cinematics, comprehensive
water/weather/seasons, persistent surface simulation, richer deterministic
NPC worlds, advanced 2D, browser rendering, multiplayer and console backends
remain engine requirements. They continue through later milestones; Alpha 1
does not imply their completion. Console qualification needs the platform SDKs
and development hardware.

There is no qualified release-date forecast yet. Progress is assessed by
closed gates and runnable games, not patch count or lines of code.

The 0.0.143 [prospective runtime/save result checks](WORKFLOW_RESPONSE_CONTRACT.md)
validate the selected successful-result surface independently of request
projection. Retained and focused native traces cover all 21 selected methods,
with explicit remaining variant/error/behavior gaps. This advances the contract
gate without closing the broader Alpha criteria. [Evidence](evidence/m2-workflow-response-contract.json).

The 0.0.144 [compiled-component workflow](COMPONENT_GAME_WORKFLOW.md) adds
fresh Native AOT publication and typed scalar component consumption through the
actual exported executable, including save/load, cold continuation and compiled
callback rollback. Its selected observations and expected errors strengthen the
contract gate; the broader Alpha release criteria remain open.

The 0.0.145 [collection workflow](COMPONENT_GAME_WORKFLOW.md#bounded-collection-profile)
extends compiled readonly consumer evidence to all five capacity-two array kinds,
length changes, guarded edits, rollback and exact selected save continuation.
The original scalar consumer also passes. This closes that bounded consumer gap,
while the wider contract, performance, interaction and delivery gates remain open.

The 0.0.147 [retained-game consumer](evidence/m2-retained-save-recovery-upgrade.json)
adds verified previous-payload recovery, a 49-preserved/one-default global upgrade,
actual pickup and full cold restoration through the bundled executable. Its
postcallback comparison explicitly checks the three owner-observation globals
against native save status and keeps all other selected/native payload fields
exact. This closes the named consumer gap; additional game families, a released
runtime contract and the broader Alpha gates remain open.

The 0.0.151 [Windows launcher save check](evidence/m2-default-launcher-saves.json)
qualifies per-user storage without a manually supplied root, including actual
generated-launcher saves, cold loads, relocation and explicit overrides.
This addresses a standalone setup gap; the management-game workflow,
representative performance, physical interaction and fresh-install gates remain
open.

The 0.0.152 [GPU memory diagnostics](GPU_MEMORY.md) add actual per-process heap
allocation estimates and budgets, with sampling/lifetime checks on both laptop
GPUs and exact saved-state invariance. This supplies a measurement prerequisite;
it does not close the representative performance, peak-memory or reliability
gates. [Evidence](evidence/m2-gpu-memory.json).

The 0.0.162 [installed authoring workflow](AUTHORING_TOOLS.md#qualification)
qualifies binary SDK/analyzer/generator publication, native component callbacks,
full save continuation and relocated export on the development computer while
the original checkout pathname is unavailable. This closes the engine-checkout dependency for that NativeAOT path; CoreCLR bridge packaging, clean-machine setup,
physical interaction and the other Alpha delivery gates remain open.

The [full-workload passive memory investigation](MANAGED_MEMORY.md#full-workload-investigation)
adds CPU-only reload, warm/cold restore and unchanged advancing-tail observations
with complete state preserved. Retired contexts remain alive without a full
collection; their unloadability and the earlier rendered memory growth remain
separate investigations. This does not close rendered performance, observer cost
or the broader reliability gate.

The [separate explicit-collection diagnostic](MANAGED_MEMORY.md#explicit-collection-diagnostic)
observes four retired contexts reclaimed while complete current saved state stays
identical. This qualifies that selected unloading path; automatic GC timing,
the earlier rendered memory growth and the wider reliability gate remain open.

The [load-capacity fix](MANAGED_MEMORY.md#load-capacity-recovery) prevents already-dead
retired entries from permanently blocking load admission. Paired compiled tests
cover the real tracking boundary on Windows and Linux; a separate native host
with the corrected bridge preserves complete reload/restore state. The wider
reliability and rendered-memory investigations remain open.

The [full-game callback allocation cohort](evidence/m2-callback-allocation-workload.json)
separately records profiling-off/on Linux owners, complete saved-state forecasts
and about 273 allocated bytes per measured callback tick. This measures a
bounded amount of current-thread traffic; it does not establish retained memory
growth or close the broader reliability gate.
