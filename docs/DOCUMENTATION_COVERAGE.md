# Documentation coverage inventory

This inventory maps supported engine mechanisms to learning material, reference,
tasks and checks. It is a gap inventory, not a claim of complete manual coverage.
Use [implementation status](IMPLEMENTATION_STATUS.md) for qualified behavior and
platform limits, then inspect the actual executable's capabilities. A linked
test describes an oracle; its presence does not prove it ran on your build.

The reference source for command fields is focused `world.describe` and
`poima schema`. Native public headers and the compiled C# SDK supply their own
interface facts. Subsystem guides explain ordering, ownership and restrictions
that field schemas cannot express. A searchable, generated, release-versioned
native/C# symbol reference is still needed; current Markdown pages and live
discovery do not replace it.

## Supported workflow map

Each row names an available documentation route and a concrete remaining gap.
The check links are representative, rather than an exhaustive test inventory.
Build options and source licenses in the linked guides remain prerequisites.

| Capability | Manual and reference | Task and check | Human workflow or coverage gap |
| --- | --- | --- | --- |
| Build, capability discovery and compatibility | [Build](BUILD.md), [world service](WORLD_SERVICE.md), [authoring contract](AUTHORING_API_COMPATIBILITY.md) | [Guarded scene edit](../examples/guarded-authoring/README.md); [contract conformance](../tests/authoring_core_conformance.py) | A consolidated release/toolchain/package matrix and generated reference navigation are needed. The nine-method authoring contract does not freeze the whole engine. |
| Hierarchy, components, atomic edits and history | [World service](WORLD_SERVICE.md), [editor service/history](EDITOR.md), [custom components](CUSTOM_COMPONENTS.md), [collections](COMPONENT_COLLECTIONS.md) | [Guarded scene edit](../examples/guarded-authoring/README.md); [world history](../tests/world_history_contract.py) | [Desktop controls](DESKTOP_EDITOR.md) cover current panels; advanced hierarchy, prefab/variant and selection workflows must not be inferred from the scene service. |
| Frozen hierarchy recipes and runtime instances | [Runtime instances](RUNTIME_INSTANCES.md), [compiled lifecycle](GAMEPLAY_LIFECYCLE.md) | [Recipe authoring/resolution task](RUNTIME_INSTANCES.md#author-a-recipe); [world-service source oracle](../tests/world_instances_contract.py), [compiled consumer](../tests/managed_instance_gameplay/ManagedInstanceGame.cs) | Native and compiled references explain local/live identity, reservation and deletion. A desktop prefab editor, variants and visual recipe editing are not supplied; test sources are not qualification results. |
| Shared clients and deliberate recovery | [Shared sessions](SHARED_SESSIONS.md), [Python transport](PYTHON_CLIENT.md), [MCP](MCP.md), [agent setup](AGENT_CLIENTS.md) | [Shared-session contract](../tests/shared_session_contract.py), [MCP contract](../tests/mcp_contract.py) | Discovery and retry guides exist. A packaged compact skill with evaluated failure recovery is not yet supplied. Provider authentication belongs to the selected client. |
| Projects, dependency closure and export | [Projects](PROJECTS.md), [asset references](ASSET_REFERENCES.md), [provenance](ASSET_PROVENANCE.md) | [Locomotion Yard export](../examples/locomotion-yard/README.md); [project contract](../tests/project_contract.py) | Desktop Project controls are narrower than the native packaging service. Add a complete human packaging walkthrough and clean-machine install guide as those workflows qualify. |
| Static geometry, textures and materials | [Assets](ASSETS.md), [material authoring](MATERIAL_AUTHORING.md), [FBX intake](FBX_IMPORT.md) | [Courtyard](SCENE_CAPTURE.md#try-the-example); [asset contract](../tests/asset_contract.py) | Desktop import/material controls are documented subsets. More recipes for UV, normals, tangents and material conversion failures are needed across supported formats. |
| Procedural material recipes | [Procedural materials](PROCEDURAL_MATERIALS.md) | [Original recipes](../examples/materials/README.md); [procedural contract](../tests/procedural_material_contract.py) | Job completion and effective bindings have a task route. A human recipe editor and arbitrary procedural geometry workflow are not established by this feature. |
| Imported skeletons, skinning and separate takes | [Animation assets](ANIMATION_ASSETS.md), [GPU skinning](GPU_SKINNING.md), [imported character](IMPORTED_CHARACTER.md), [FBX recovery](FBX_IMPORT.md) | [Character Yard](../examples/character-yard/README.md); [independent source oracle](../tests/imported_character_contract.py) | Add an interactive rig/weight/clip diagnostic walkthrough when those editor controls exist. Source-relative reference selection is currently an explicit authoring operation. |
| Explicit frame conversion and rotation retargeting | [Frame transfer](ANIMATION_FRAME_TRANSFER.md), [rotation retargeting](ANIMATION_RETARGETING.md) | [Locomotion Yard](../examples/locomotion-yard/README.md); [rotation policy contract](../tests/animation_rotation_retarget.py) | Reference selection, source fingerprints and scope are documented. Contact fitting, stride fitting and a visual retarget editor are separate gaps. |
| Playback, blending, inertialization and layers | [Runtime animation](RUNTIME_ANIMATION.md), [managed animation](MANAGED_GAMEPLAY.md) | [Locomotion Yard](../examples/locomotion-yard/README.md); [layer contract](../tests/runtime_animation_layers_contract.py) | Inspector playback controls are a subset. A complete animation debugging tutorial and generated C# playback reference are needed; IK and animation graphs are not implied. |
| Physics, character input, precise static mesh queries | [Runtime](RUNTIME.md), [interactions](PHYSICS_INTERACTIONS.md), [mesh collision](MESH_COLLISION.md), [character input](CHARACTER_INPUT.md) | [Physics room](RUNTIME.md#try-the-physics-room); [mesh collision contract](../tests/mesh_collision_contract.py) | Add scene-oriented collider/query troubleshooting and movement tuning guides. Static triangle queries do not establish full production character movement or arbitrary dynamic triangle collision. |
| Navigation and deterministic NPC decisions | [Navigation](NAVIGATION.md), [C# navigation interface](MANAGED_GAMEPLAY.md) | [Patrol Room](../examples/managed/PatrolGame/README.md), [Locomotion Yard](../examples/locomotion-yard/README.md); [navigation binding](../tests/navigation_binding_contract.py) | The native bake/query path has instructions. Tactical AI, general behavior authoring and a visual navigation workflow remain separate work. |
| Compiled C# gameplay, reload and native shipping | [Managed gameplay](MANAGED_GAMEPLAY.md), [native gameplay](NATIVE_GAMEPLAY.md), [editor gameplay](EDITOR_GAMEPLAY.md), [lifecycle](GAMEPLAY_LIFECYCLE.md) | [Collection Room](../examples/collection-game/README.md), [Locomotion Yard](../examples/locomotion-yard/README.md); [native publication requirements](../tests/native_gameplay_publish_requirements.py) | Generate reference for every public SDK interface, including negotiated service requirements, ordering and staged reads/writes. Each example currently supplies only its own gameplay build recipe. |
| Portable saves, compiled requests and schema upgrades | [Gameplay saves](GAMEPLAY_SAVES.md), [persistence metadata](GAMEPLAY_PERSISTENCE.md), [upgrades](SAVE_UPGRADES.md), [editor saves](EDITOR_SAVES.md) | [Checkpoint examples](PLAYBOOKS.md#start-with-a-complete-example); [runtime save contract](../tests/runtime_save_contract.py) | Add a complete game-version upgrade tutorial covering field/component changes, corruption and rollback. Existing explicit upgrade mechanisms do not promise arbitrary automatic migration. |
| Input profiles, gamepad assignment and player preferences | [Input profiles](INPUT_PROFILES.md), [gamepads](GAMEPADS.md), [player settings](PLAYER_SETTINGS.md) | [Settings task](PLAYER_SETTINGS.md); [input profile](../tests/input_profile_contract.py), [player settings](../tests/player_settings_contract.py) | Editor device controls have a guide. In-game remapping/settings widgets and live preference changes are not supplied by next-launch profile persistence. |
| Logical/styled game UI and compiled callbacks | [Game UI](GAME_UI.md) | [Collection Room](../examples/collection-game/README.md); [compiled control contract](../tests/runtime_ui_control_contract.py) | Current panel/label/button layout and callback behavior have reference. Text input, image widgets, localization, inventory widgets and a visual UI authoring workflow need separate implementation and documentation. |
| Continuous playback and iterative agent observation | [Player](PLAYER.md), [live shared player](LIVE_PLAYER.md) | [Live observation recipe](LIVE_PLAYER.md#a-guarded-observation-recipe); [shared player contract](../tests/player_service_contract.py) | Native/headless and desktop owners have separate lifecycles. Physical input, focus, minimize behavior and scaled UI need human walkthroughs and recorded checks appropriate to each frontend. |
| Direct lighting, shadows, sky and material appearance | [Lighting](LIGHTING.md), [shadows](SHADOWS.md), [materials](MATERIAL_AUTHORING.md), [HDR composition](HDR_COMPOSITION.md) | [Scene capture](SCENE_CAPTURE.md); [lighting contract](../tests/lighting_contract.py) | Add an artist-facing lighting/exposure/material tutorial and visual debug-view guide. Procedural sky does not imply clouds, weather, seasons or a day/night simulation. |
| Renderer paths, AO, reconstruction and diagnostics | [Deferred path](DEFERRED_RENDERING.md), [AO](AMBIENT_OCCLUSION.md), [reconstruction](RECONSTRUCTION.md), [scene products](SCENE_PRODUCTS.md), [diagnostics](RENDER_DIAGNOSTICS.md), [schedule](RENDER_SCHEDULE.md) | Render checks and limitations are linked from each guide | A consolidated quality-tier/platform matrix and workflow for comparing correctness, image quality and frame cost are needed. Isolated checks do not establish production GPU performance. |
| Audio propagation, events and output | [Audio](AUDIO.md), [audio events](AUDIO_EVENTS.md), [editor audio](EDITOR_AUDIO.md) | Contract and listening fixtures linked in the audio guides; [audio contract](../tests/audio_contract.py) | Add a coherent sound-authoring/mixing/propagation debugging tutorial as the available tools permit. Small wave captures do not qualify every audio device or gameplay condition. |
| Job scheduling and CPU/GPU profiling | [Jobs](JOBS.md), [profiler](PROFILER.md), [development jobs](DEVELOPMENT_JOBS.md) | [Profiling task](PLAYBOOKS.md#continue-an-existing-project); [profiler contract](../tests/profiler_contract.py) | Native tracing has a task path. Managed allocation/GC analysis and a full game-scale performance tutorial remain gaps; pool contracts are not workload speed claims. |
| Desktop authoring and agent chat | [Desktop editor](DESKTOP_EDITOR.md), [editor gameplay](EDITOR_GAMEPLAY.md), [agent clients](AGENT_CLIENTS.md) | Documented panel workflows and linked bounded UI/agent evidence | Headless checks do not cover physical selection, docking or device interaction. Complete contextual help, accessible navigation and a guided editor tutorial need their own coverage. |

## How to close a coverage gap

For a supported capability, add the missing concept explanation, exact reference
or task recipe beside its existing guide. Give every runnable task its required
build/package versions, native paths, input/output ownership and completion
observations. Link recovery to the relevant revision, receipt and lifecycle
rules. Verify the documented command or code against the matching executable;
record the scope of that run in the guide's evidence rather than inferring it
from another subsystem's success.

Human workflows need instructions for the actual exposed controls and a visual
or physical check when the task depends on them. Agent workflows need compact
discovery, semantic checkpoints and relevant image/audio observations. One
cannot substitute for the other. Compact Codex/Claude skills should select and
link these qualified tasks, not duplicate schemas or supply missing features.

Checked-out Markdown is usable locally today. A searchable manual, generated
symbol reference, release-versioned documentation site, coverage comparison
against live schemas, and evaluated installable skills remain documentation
deliverables. See the [documentation standard](DOCUMENTATION.md) for release
requirements. Planned terrain, weather, FPS packages and additional platforms
belong in the [roadmap](ROADMAP.md) until an implemented task has a real guide.
