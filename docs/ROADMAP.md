# Road to Poima 1.0

Poima is being built as a general-purpose engine for agent-driven and human
game development. Its first usable milestone is a small 3D single-player game
workflow. The complete destination also includes advanced rendering, reactive
worlds, deterministic simulation, 2D, browser deployment, multiplayer and
current-generation consoles.

This roadmap segments that work by dependencies and observable results. It
does not assign dates or equate development version numbers with completion.
The [implementation status](IMPLEMENTATION_STATUS.md) records what works now;
the [Alpha 1 checklist](ALPHA_ROADMAP.md) defines the first release gates.

## Version and release stages

| Version | Meaning | Entry condition |
| --- | --- | --- |
| `0.x.x` | Pre-alpha development checkpoints | Individually implemented, reviewed and qualified changes; supported boundaries can still be incomplete. |
| `1.0.0-alpha.N` | Usable engine candidates while planned features are being built | Alpha 1's game workflow passes its gates; later candidates integrate coherent additions and preserve earlier supported workflows. |
| `1.0.0-beta.N` | Feature-complete candidates for the agreed 1.0 scope | The scope manifest is complete, required platforms and systems have end-to-end evidence, and contracts and migration policies are ready for stabilization. |
| `1.0.0-rc.N` | Candidates for the final release | Beta hardening passes; only release-blocking fixes remain. |
| `1.0.0` | Supported release | The exact release artifacts pass the correctness, performance, compatibility, installation and documentation gates. |

Prerelease numbers count candidates. A work segment below can take several
alpha candidates, and one candidate can close work from several segments.
Alpha becomes beta when the feature-complete gate passes, regardless of its
candidate number.

## Scope ownership

The full target below remains the planning baseline. Alpha 1's smaller workload
does not remove requirements from that destination. Single-player PC work comes
first; multiplayer and console integration follow it. Console implementation
needs licensed SDKs and approved development hardware.

The 1.0 acceptance target is a complete Windows single-player FPS workflow,
with Linux headless authoring and the engine systems below. Browser deployment
is targeted for 1.1.0 and advanced 2D for 1.2.0, before Linux graphics,
multiplayer and consoles. Their
portability boundaries and feasibility work begin earlier; their implementation
and platform qualification do not block 1.0.

Before beta, a versioned 1.0 support manifest must name every included feature,
platform, hardware tier and compatibility guarantee, with its acceptance
evidence. Any narrower release scope needs an explicit decision and a visible
later milestone for the deferred requirements. A missing SDK or an unfinished
system cannot be silently counted as complete.

Situational systems may be installed as project packages rather than included
in every engine/player. Their discovery, authoring, runtime/save integration
and qualification remain required when enabled. The
[FPS capability matrix](FPS_1_0.md) assigns engine and package responsibilities.
Core jobs, state ownership and resource/lifecycle services remain shared
foundations. Optional hosted
services and community adoption are not release gates.

## Segmented implementation

### 1. Complete the first usable game workflow

Build a complete small 3D game from a fresh project using Codex or Claude,
compiled C# gameplay and imported content. Exercise menus, input, settings,
save/load, failure recovery and game completion, then export and run it from a
different location. Humans must be able to inspect and change the same project.

Close native character intake and separate animation-clip composition, practical
UI/input/audio/lifecycle blockers, supported save evolution, clean installation
and blocking editor interaction. Preserve compact discovery, revision guards,
retry receipts, joined observations, compile diagnostics and replay throughout
the workflow. Record attempts and recovery as well as the final successful game.

**Exit:** the [Alpha 1 gates and workload budgets](ALPHA_ROADMAP.md) pass.
Current foundations include three recorded agent game exercises, compiled
gameplay, saves, navigation, animation transitions, a renderer and desktop
editor. Their combined alpha workflow still needs qualification.

### 2. Parallel runtime foundation

Make dependency-aware multithreaded jobs a core service with bounded workers,
owned inputs/results, cancellation, resource retirement and profiler visibility.
Integrate actual CPU work before claiming a parallel runtime; compare one and
multiple workers for correctness, throughput and frame pacing. Authoritative
edits and result commitment retain explicit ordering even when preparation is
parallel. Compiled gameplay uses the supported lifetime/data boundaries.

**Dependencies:** state/resource ownership, fixed-tick contracts and observation.
**Exit:** representative engine workloads use the shared scheduler, preserve
declared state/results across worker counts, retire safely on reload/shutdown
and show measured scaling without oversubscription. This begins during the
first usable workflow after the current character-import slice, ahead of new
large graphics, AI and simulation systems.

### 3. Production character and content pipeline

Extend standard-format intake with bone mapping, retargeting diagnostics and
reusable animation assets. Complete code-first and hierarchical state-machine
playback, layers, IK, foot placement and explicit root-motion ownership.
Add motion-matched locomotion with inspectable pose/trajectory features,
transitions and animation-database cost. Integrate ragdolls, damage-directed
hit reactions and recovery with explicit animation/physics ownership.
Harden movement for slopes, steps, crouching, moving platforms and swimming;
extend navigation with dynamic obstacles, traversal links and avoidance.
Provide code/data-driven recoil, ADS camera transitions and weapon feedback.
First-person viewmodels need separate FOV, collision-aware presentation and
wall-clipping protection, with animation and rendering policies independent
of the world camera.

Add hierarchical spawning, prefab composition, variants and inspectable
override origins. Projects need pinned packages, dependency queries, incremental
imports/cooking, material instances and reproducible reuse between games.
Evaluate optimized storage paths such as DirectStorage behind portable I/O,
with measured streaming benefits and a functioning ordinary-file path.
Licensed-content connectors must preserve creator, source, license, attribution
and input/cooked hashes, with offline builds and ordinary user-owned imports.

**Dependencies:** stable identities, pose/physics ownership, schemas and content
closure. **Exit:** an imported animated character game and a second project
reuse the pipeline; unsupported rigs/materials fail with useful reports, and
retained games, packages and supported saves continue to work.

### 4. Scalable rendering and atmosphere

Develop one renderer with quality tiers: PBR/material layers, transparent
materials, scalable lights and shadows, GPU culling/instancing, LOD and content
streaming.
Dense-scene research compares virtualized geometry with GPU-driven instancing
and LOD under actual content/frame/VRAM budgets. Select and qualify the measured
approach; a particular engine's geometry technique is not itself the requirement.
Integrate dynamic indirect lighting and reflections, including a
reduced tier for hardware without ray tracing. Add inspectable post effects,
robust temporal reconstruction and separately qualified DLSS, FSR and NVIDIA
frame generation; XeSS remains a bonus.

Build day/night lighting, atmospheric scattering, fog, clouds, wind and dense
environmental motion around shared world time. New research techniques need
controlled moving-scene quality/cost comparisons against the functioning
baseline. Novelty alone does not establish improved graphics or performance.

**Dependencies:** render scheduling, scene products, reliable motion/history,
material/content contracts and profiling. **Exit:** a representative atmospheric
scene runs within declared CPU/GPU/VRAM budgets across supported tiers, with
measured quality comparisons. Frame-generation-off rendering and latency remain
separate acceptance measurements.

### 5. Reactive detail, persistent effects and acoustics

Implement breakable props, doors and glass, plus persistent scorch, blood,
wetness, snow tracks, residue and bullet marks. Saved surface state must stream
and remain consistent through gameplay and restoration. Procedural authoring
adds geometry-aware wear, exposed substrates, erosion and controlled edge/dent
deformation, with explicit collision/navigation recooking when geometry changes.
Terrain tools add curated stamps, splines, modular meshes and constraint-aware
placement with repeatable recipes. Movement colliders and precise weapon
queries have separate policies, including real apertures and explicit coverage
masks where visible fence/cutout openings must remain shoot-through.

Provide compiled CPU/GPU VFX authoring, previews, simulation/bounds/overdraw
inspection and gameplay event integration. Extend native audio with buses,
ducking, snapshots, streaming music, voice management and propagation:
occlusion, reflections, diffraction, reverberation, direction/elevation,
distant delay and indoor/underwater transitions.

**Dependencies:** authoritative events and sparse saved state, precise queries,
geometry generations, materials and asynchronous audio ownership. **Exit:**
doors, windows and breakable objects change collision, light, sound and
navigation coherently; persistent marks survive saves; audio and VFX behavior
have semantic, visual/listening and performance evidence. Full building and
terrain destruction are outside the current destruction baseline.

### 6. Connected water, weather and seasons

Combine reactive oceans, lakes and rivers with shore/beach response, buoyancy,
wakes and underwater behavior. Support blocked/redirected flow, connected
reservoirs, room flooding and filling/draining containers. Assign one owner to
each transfer and bound active simulation regions so visual detail can scale
without losing accounted water quantities.

Storms, lightning, precipitation, wind and seasons share world state with water,
audio and surfaces. Longer-term effects must reach crops, resources and travel;
accelerated time must preserve accumulated effects.

**Dependencies:** time ownership, saved environment state, physics, rendering
and audio. **Exit:** a coastal/river scene plus independent conservation tests
verify overflow, redirected flow, containers, save/load and time acceleration.
Visible and queried water surfaces agree within declared tolerances.

### 7. Deterministic living worlds

Provide authored utility/HTN/behavior-tree/state-machine decisions, authored
dialogue, persistent identity, memory, relationships and rivalry. Extend daily
work, trade, resources and schedules with simulation levels for distant people.
Promotion into active simulation preserves possessions, commitments and history.
NPC behavior has no generative-model dependency.

Controllers, navigation, seasons and resource constraints must affect decisions
through inspectable native state. Keep reusable engine systems separate from a
particular game's era, location or narrative.

**Dependencies:** explicit time/random ownership, conserved transfers, durable
schemas and character/navigation systems. **Exit:** a living settlement passes
long-run replay, inventory/resource conservation, time acceleration, saves and
active/offscreen promotion checks under a declared population budget.

### 8. Complete authoring, player UX and creative packages

Finish agent access to hierarchy, components, animation, VFX, UI and world
systems, with focused queries, diagnostics, observations and low-volume results.
The desktop editor uses the same core operations. Close selection, camera,
docking, Inspector/Project/Hierarchy, context navigation/locking, console,
profiler, gizmo and Play-edit retention workflows. Add the agreed wine identity
and simple/advanced workspaces with external agent clients.

Player UX includes HUDs, inventories, menus, diegetic screens, typed bindings,
controller focus, rebinding, haptics and portable graphics/audio/input settings.
Localization needs string-table round trips, shaping, fallback, RTL/IME and
accessibility. Creative packages add shot-intent cinematics, reusable feedback,
tweening and inspectable post-processing. Native Git/GitHub tools must preserve
ordinary source-control behavior and explicit account ownership.

**Dependencies:** shared schemas, lifetime/cancellation, saved preferences and
runtime/editor observation. **Exit:** both agents and a human perform the
required workflows; physical keyboard/mouse/controller, translated UI and
accessibility checks pass. These improvements start earlier and are qualified
together here.

### 9. Complete Windows single-player FPS workflow

Build and export a complete FPS with imported animated characters and weapons,
recoil/ADS, motion-matched movement, separate-FOV viewmodels, hit reactions and
ragdolls. Exercise combat encounters and navigation, progression, menus,
settings, save/checkpoints, death/retry and game completion. Demonstrate dense
geometry, atmosphere, sound and persistent world effects within the declared
hardware tier, rather than qualifying isolated systems alone.

**Dependencies:** the integrated 3D runtime, character/content, rendering,
audio/VFX/UI and agent workflows above. **Exit:** agents can author, playtest,
repair and export the game; humans can inspect and edit it; the relocated
Windows build runs without the development checkout. Repeated combat,
viewmodel near-wall movement, animation/physics transitions, save/load and
long-play checks pass with published frame-time and memory budgets.

## Planned after 1.0

These requirements remain in the full engine roadmap. Their absence does not
prevent the Windows single-player 1.0 release.

Browser deployment is the target for **1.1.0**, followed by advanced 2D for
**1.2.0**, before the other three tracks. Each minor release ships when its
acceptance criteria pass; the numbers do not assign dates. Shared rendering,
content, input and UI boundaries are designed for them before 1.0, without
adding their full implementation to 1.0.

| Track | Deliverable | Acceptance |
| --- | --- | --- |
| 1.1.0: browser | Compiled WebAssembly core/gameplay, WebGPU renderer and an evaluated WebGL2 reduced tier; browser input/audio, asynchronous content/storage and deployment. | Actual browser/version matrix, downloaded-size/memory/frame budgets and save lifecycle. Desktop C# compilation is not browser qualification. |
| 1.2.0: advanced 2D | Sprites, tilemaps, sorting/layers, animation, appropriate physics, normal/height/PBR-aware lighting and soft shadows, mixed 2D/3D scenes. | A complete 2D game exercises authoring, lighting, input, save and export; an orthographic camera alone is insufficient. |
| Linux graphics | Qualify the Windows player through Proton and evaluate native Vulkan editor/player deployment. | Actual driver, input, audio, save and performance checks; Linux headless success does not qualify either graphical route. |
| Multiplayer | Self-hosted authority, replication/interest management, prediction/reconciliation, co-op followed by extraction-scale play, reconnect/late join and acoustic proximity voice. | 4–8-player co-op and a representative 24–32-player extraction workload with AI pass latency/loss, authority and match-persistence checks. |
| Consoles | Xbox Series X/S and PS5 native graphics, shader, audio, input, users, storage/save, lifecycle and packaging adapters. | Actual approved SDK/hardware execution and platform requirements; this track is gated by developer access. |

Before 1.0, keep renderer/platform services behind native interfaces, separate
authoritative simulation from presentation, and define stable entity/event/time
ownership and serialization boundaries. Use bounded portability experiments to
identify incompatible dependencies and APIs early. These boundaries reduce
later rework; they do not claim that a platform port or multiplayer is complete.
Console graphics use the platform's native API. Each later destination gets its
own gameplay compilation, content, lifecycle and delivery qualification.
Browser graphics use supported browser interfaces; PC graphics remain Vulkan.

## Beta entry: feature completeness

Beta entry requires the explicit 1.0 support manifest and its complete evidence
matrix. Each included system must work in the declared end-to-end games, be
authorable through the shared engine, have meaningful diagnostics/observation,
and support its declared persistence and platform behavior.

Large missing systems remain alpha work. A required capability cannot be
counted complete merely because its package is optional to install. The supported contracts,
save/content evolution and migration policy must be reviewed before freezing
the beta boundary.

## Beta: hardening

- Run representative games and scenes on supported NVIDIA/AMD/Intel and older
  non-RT profiles. Measure pacing, CPU/GPU cost, RAM/VRAM, I/O, shader hitches,
  cooking and edit-to-play latency.
- Test retained projects/games/saves, supported upgrades, failed reloads,
  interruption, storage corruption, device loss and resource retirement.
- Perform sustained play/authoring soaks, security/resource-budget checks on
  untrusted assets and native real-time audio checks.
- Complete physical input, controller-only UI, localization, accessibility,
  documentation and clean installation/relocated-export qualification.

Logical determinism, physics numerics, GPU effects and pixel identity have
different guarantees. Each supported guarantee needs evidence at that scope.

## Release candidates and 1.0

Release candidates use exact source, binary, content and dependency inventories.
They undergo the full supported-platform regression, migration, installation,
offline rebuild and exported-game checks. Fixes repeat affected gates; unresolved
critical data loss, contract breakage or platform failures prevent release.

The final release publishes its feature/platform/hardware matrix, compatibility
policy, documentation and reproducible artifacts. Research continues afterward,
and any explicitly deferred requirements keep their named milestones.

## Rules across every segment

- Every capability includes documentation, shared authoring/discovery,
  diagnostics, appropriate compiled gameplay access and meaningful tests.
- Authoritative state has recovery, save/load and supported migration;
  visual/audio caches are reconstructed from that state.
- Independent references verify physics, animation, water and simulation.
  Moving captures, listening evidence and semantic checks remain distinct.
- Parallel implementation uses clear file ownership and independent review;
  builds and qualification fit the development machine's resource budget.
- Research prioritizes recent primary sources and preserves useful established
  methods. A technique is promoted only after correctness, licensing,
  integration and measured quality/cost evidence.

Detailed tasks emerge from implementation and real games. Scope, dependency
order, release criteria and unresolved risks remain visible as that detail grows.
