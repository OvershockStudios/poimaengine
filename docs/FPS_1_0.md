# Windows FPS capability target for 1.0

Poima 1.0 must support authoring and delivering a complete Windows single-player
FPS with AA/AAA-style production needs. This is an acceptance target, not a
claim about the prototype's current graphics or shipped games. The
[release roadmap](ROADMAP.md) and [implementation status](IMPLEMENTATION_STATUS.md)
separate planned work from recorded capabilities.

The engine provides the underlying technologies and shared authoring contracts.
Reusable packages provide situational gameplay systems. An agent or human
developer defines the actual game's rules, assets, pacing and presentation.
Quality labels alone are insufficient: each supported feature needs an
observable workload, declared hardware tier and measured acceptance evidence.

## Capability matrix

| Area | Engine responsibility | Reusable package or game responsibility | Acceptance examples |
| --- | --- | --- | --- |
| Multithreaded jobs | Bounded workers, task dependencies, parallel work over owned/frozen data, deterministic result commitment, cancellation and profiling. | Jobs for authored systems using supported lifetimes and data boundaries. | Representative integrated workloads run with one and multiple workers; no state races, stranded tasks or unbounded oversubscription; publish scaling and frame-time costs. |
| Weapon actions | General input actions, animation events/sockets, physics queries and typed gameplay state. | Weapon definitions, attachments, ammo, reload/chamber/equip states and an example weapon framework. | Interrupt reload/equip/fire, switch attachments, save/load and replay without duplicate ammo or events. |
| Recoil and camera feel | Fixed-tick state with responsive interpolated camera/presentation controls. | Authored recoil patterns, procedural sway/bob, shake, ADS transitions and sensitivity curves. | Rapid action changes, variable frame rates, controller/mouse input, save and replay preserve declared behavior. |
| First-person representation | Separate viewmodel projection/FOV and render policy, world-body attachment and observation. | Separate arms/body rigs or true full-body FPS; weapon lowering/retraction and camera posture rules. | Near-wall corners, extreme FOV, reloads and crouching avoid clipping; body shadows/reflections and rendered/query geometry follow explicit policies. |
| Scopes | Secondary camera/render targets, resource budgets and material/lens support. | Picture-in-picture scope, reticle, zoom, lens distortion/occlusion and scope-specific post effects. | Aim transitions preserve correct scope/world alignment; report additional render cost and allocation lifetime. |
| Locomotion | Root-motion ownership, IK/constraints, character collision and motion-matching data/search. | Mantle, slide, lean, vault and animation/movement policies. | Slopes, stairs, moving platforms, obstructed vaults and changing stance; inspect trajectory selection and foot/hand contacts. |
| Hit detection | Precise mesh queries, bone-attached shapes, collision groups and continuous projectile queries. | Per-bone hitboxes, damage zones, armor and damage rules. | Fences/apertures, fast projectiles, animated targets and scaled rigs; distinguish visual, movement and ballistic collision. |
| Ballistics | Material queries and bounded integration/sweeps with inspectable results. | Drop, penetration, energy loss, ricochet and authored ammunition/material parameters. | Known trajectories and layered materials verified independently; impacts do not tunnel or duplicate on replay. |
| Ragdolls and reactions | Joint constraints/motors, pose handoff, hit impulses and recovery ownership. | Damage-directed reactions, partial ragdolls, death/recovery rules. | Animation-to-physics transitions, repeated hits, stairs, recovery and save restoration without explosive impulses or pose jumps. |
| Controller aim | Device/actions, response curves and queryable target state. | Configurable slowdown, rotational help and target-selection rules; opt-out. | Occlusion, competing targets and changing input devices; behavior must use permitted game state and match displayed settings. |
| Character appearance | Skin subsurface response, eyes/cornea, skin microdetail, hair/cards/strands and cloth shading, quality tiers. | Licensed character materials, rigs, facial shapes, grooming and clothing. | Close-up moving faces under varied light/exposure; preserve detail and plausible eyes, avoiding excessive smoothing or waxy scattering. |
| Character animation | Facial/body deformation, retargeting diagnostics, cloth/secondary motion and LOD. | Imported-character customization and animation assets. | Facial/body joints, clothing interactions, near/far transitions and animation/physics ownership; a full human creator is a later milestone. |
| Dense worlds | GPU culling/instancing, LOD/streaming and evaluated cluster virtualization. | Scene assembly, material/geometry budgets and artist-controlled detail. | Dense moving scenes on declared tiers; measure cooking RAM/disk, VRAM, streaming hitches and material/collision fidelity. |
| Terrain | Streamed terrain/mesh products, terrain material layers, height/normal queries, collision and navigation integration. | Terrain authoring package: curated sculpting/stamps, roads/splines, erosion and repeatable recipes. | Tile seams, steep surfaces, roads, edits/recooking, save/reopen and traversal at representative scale with bounded memory. |
| Foliage | GPU instances, coverage/alpha policy, shadows, LOD, wind deformation and motion history. | Vegetation placement, biome/density rules and interaction/bending presets. | Dense grass/trees under camera motion and weather; test shimmer, overdraw, shadow cost, interaction and streamed-instance persistence. |
| Large-world coordinates | Explicit authoritative precision, camera-relative GPU data, bounded physics regions and spatial streaming. | World cells, travel and origin/region policy. | Far-from-origin aiming, small contacts, scopes, vehicles, saved positions and region transitions remain coherent without visible jumps or lost precision. |
| Vehicles | Physics constraints/queries, camera/input/audio/animation ownership and spatial/save integration. | Optional vehicle systems for suspension, wheels, handling, seats and enter/exit; watercraft integrate buoyancy when enabled. | Fast movement, uneven terrain, passengers, hit queries, sleep/wake, save/load and streamed-region transitions; exact vehicle types belong in the supported package manifest. |
| Lighting and post effects | Scalable lights, GI/reflections/shadows, motion vectors, post-processing and temporal reconstruction. | ADS depth of field, motion blur, transient muzzle lights and style profiles. | Scope/viewmodel history, rapid muzzle flashes and moving occluders; inspect active effects and quality/cost. |
| Shader and pipeline warmup | Incremental shader products, pipeline inventories, cache validation and background preparation. | Project-specific shader/material variants and warmup workload. | Cold/fresh-driver and warm launches, variant changes and first encounters; report hitches separately from steady frame rate. |
| Input latency | Input/simulation/render/present markers, frame-pacing controls and a portable low-latency baseline; qualify Reflex separately. | Game policies for sampling, buffering and responsiveness. | Trace software stages; actual click-to-photon claims require external display/input measurement, not CPU timestamps alone. |
| Perception | Queryable visibility and gameplay sound events/propagation paths separate from audio playback. | Sight cones, awareness, hearing, memory and fair knowledge rules. | Doors/walls, travel delay, remembered versus current target position, save/replay and bounded many-NPC cost. |
| Cover and tactical position | Navigation, traversal, spatial/line-of-fire queries and authored metadata. | Cover/peek/stance candidates, exposure scoring and reservations. | Dynamic blocking, crouched/standing exposure and unreachable cover; avoid inconsistent navigation, sight and bullet policies. |
| Squads | Stable identities/events, inspectable shared state and scheduled queries. | Roles, flanking, suppression, search, support and friendly-fire avoidance. | No duplicated reservations, stable ties, bounded replanning and no knowledge unavailable to the squad. |
| Combat sound and VFX | Low-latency event scheduling, spatial propagation, persistent surface state and effect observation. | Layered gunshots/tails, impact feedback, material effects and authored mix rules. | Shots, reloads and impacts align with animation/gameplay; doors and materials affect propagation; residue survives saves. |
| Weather and surface response | Shared environmental time/state, material response and sparse persistent surface data. | Weather package drives exposure-aware wetness/drying, rain/snow accumulation, puddles/runoff and seasonal effects. | Sheltered surfaces, indoor/outdoor transitions, material-dependent response, time acceleration and save/load remain consistent with water, audio and physics. |
| Level blocking | Headless/human geometry operations, deterministic CSG/greybox products and collision/navigation recooking. | Layout, brushes/stamps, encounters and reusable level recipes. | Agents build and revise playable spaces with stable IDs; openings remain geometrically valid and navigation updates coherently. |
| Settings and player UX | Portable preferences, typed settings, input focus, accessibility/localization and scalable UI. | FOV, sensitivity curves, presets, HUDs, menus and inventory presentation. | Physical mouse/controller, controller-only menus, changed resolution/settings and persisted/rebound controls. |
| Streaming and actor scale | Background I/O/cooking, resource retirement, simulation/animation/audio LOD and memory budgets. | Encounter populations and quality policies. | No unbounded work per distant actor; representative combat/streaming frame pacing rather than empty-scene FPS. |
| Delivery and recovery | Compiled gameplay, reload boundaries, supported saves/migrations, packaging and diagnostics. | Progression, checkpoints, death/retry and game completion. | Fresh installation, relocated export, repeated save/reload, interrupted jobs and sustained play without lost committed state. |
| Crash reporting | Bounded local crash/failure records, build/symbol identity, native/managed diagnostics and GPU/worker breadcrumbs. | Optional report upload backend and project reporting policy. | Deliberate native/managed failure and device-loss cases produce inspectable, symbolizable records without relying on a paid service; committed project state remains recoverable. |
| Documentation and playbooks | Version-matched manual/API reference, runnable examples and capability discovery. | Task playbooks and compact Codex/Claude skills for installed packages and game workflows. | Documented fresh-project setup, authoring, debugging and export succeed without hidden steps; errors/recovery and package limits are covered. |

## Package boundaries

Situational functionality is installed per project rather than forced into every
installation or player. Candidate official packages include FPS gameplay,
tactical AI, advanced character tools, water/weather, cinematics and other
specialized systems. Core jobs, identity/state ownership, resource/lifecycle
services, local crash diagnostics and package discovery are shared foundations.

Packages use explicit dependencies, pinned versions and compatibility/capability
requirements. Projects need reproducible lockfiles, offline builds, dependency
notices and target-specific compilation/cooking. The editor and agents discover
and operate installed package types through the same schemas and guarded
commands. A package must not become an opaque GUI-only workflow.

Supported 1.0 packages are qualified with the package installed and enabled;
projects that do not use them should not carry their code/content cost.
Optional installation does not reduce a required capability to an untested
placeholder. Native extensions require controlled ownership, safe shutdown and
resource retirement; arbitrary hot-unloading is not an implied guarantee.

Animation uses code-first playback and composable solver workflows, taking
Animancer and Final IK as usability references. Direct clips, transitions,
layers/masks, events, parameters and IK constraints must be inspectable through
the shared service and compiled gameplay APIs. Material research similarly
compares established artist controls with physically based models, layering,
terrain/foliage needs and bounded shader variants. Proprietary asset code is
not a dependency of these independently implemented systems.

## Dependencies and qualification order

1. Finish imported characters/clips and the compiled gameplay consumer.
2. Establish the core job system and integrate real CPU work with profiling,
   dependency/cancellation checks and single-worker/multiple-worker comparison.
3. Extend general input, weapon state/events, precise hit queries and rig/root
   motion ownership; integrate recoil/ADS/viewmodel and movement foundations.
4. Develop character shading, dense rendering, scoped cameras, pipeline warmup
   and latency controls alongside representative workloads.
5. Integrate perception, cover and squad packages with sound, navigation,
   ballistics and animation; qualify complete encounters and persistence.
6. Close the complete game, supported-package, hardware/performance and delivery
   gates before beta and release candidates.

Profiling, documentation, agent access and meaningful tests accompany every
stage. New research can adjust dependencies; it does not replace the current
verified workflow or erase previous requirements.
The [documentation standard](DOCUMENTATION.md) defines human manuals, API
reference, task playbooks and skills as complementary release deliverables.

The separate full character creator remains planned after the imported-character
pipeline. Browser deployment targets 1.1.0 and advanced 2D 1.2.0; Linux graphics,
multiplayer and consoles follow later, with their architectural boundaries
preserved before 1.0.
