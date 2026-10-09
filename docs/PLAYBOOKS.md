# Task playbooks

Choose a task, check its prerequisites, then follow the linked guide from the
repository root. Discover the current build's capabilities and focused schemas
before making changes. A checked-in example does not enable features missing
from your executable.

For a small context budget, select one task, request capability discovery and
then retrieve only its relevant method, mutation and component schemas. Read
the linked subsystem invariants before changing state; load deeper reference
when a prerequisite, error or observation calls for it. Follow the
[focused discovery guide](WORLD_SERVICE.md) rather than repeatedly sending the
complete command catalogue to an agent.

Compatibility has several separate parts: the engine build version, the
[authoring-core contract](AUTHORING_API_COMPATIBILITY.md), the required gameplay
service prefix/features and cooked asset formats. A matching authoring contract
does not establish that a gameplay artifact or renderer is compatible. Use
the guide's recorded evidence and the actual installed capabilities together.

## Start with a complete example

| Result | Guide | Prerequisites | Completion check |
| --- | --- | --- | --- |
| Create and safely reconcile a small hierarchy | [Guarded scene edit](../examples/guarded-authoring/README.md) | Native authoring executable, Python 3.9+, installed Python client, new world path; no GPU or .NET required | Script prints `passed: true` after preview, conflict rejection, deliberate reconciliation, receipt replay and fresh-owner persistence. |
| Play a first-person collection game and continue a checkpoint | [Collection Room](../examples/collection-game/README.md) | Simulation, game UI and the matching compiled C# gameplay backend; .NET 10 SDK to build gameplay; Python for the launcher. CoreCLR also needs the matching bridge and hostfxr. Interactive play needs Windows Vulkan. | `--verify` checks collection through native input/raycast and continuation in a fresh process. Optional captures need a renderer and chosen GPU. |
| Run a guard's patrol, investigation and pursuit | [Patrol Room](../examples/managed/PatrolGame/README.md) | Simulation, managed gameplay, game UI, matching CoreCLR bridge/hostfxr, .NET 10 SDK and Python. Interactive play needs Windows Vulkan. | The documented headless verifier checks decision outcomes, wall occlusion, compatible reload and fresh-process checkpoint continuation. |
| Import a licensed weighted character and inspect its poses | [Imported character](IMPORTED_CHARACTER.md) | Native executable, Python 3.9+, pinned original GLB and a new output directory; runtime poses need simulation, optional images need Windows Vulkan | Compare source-derived transforms and geometry, inspect the converted root and retained skin, and verify source-independent fresh-owner content. |
| Control an imported character and export its compiled game | [Character Yard](../examples/character-yard/README.md) | Matching 0.0.71 engine/SDK, pinned original GLB, Python and .NET 10 SDK; simulation, navigation and the selected compiled gameplay backend. Windows graphics need Vulkan and game UI; export needs a matching installed runtime and Windows Native AOT artifact. | Independent source-pose and native route checks, reload/rejection policy, same/fresh-owner checkpoints; relocated exported player runs before a separate bundled-runtime contract. |
| Build and export controller-driven idle/run locomotion from original FBX clips | [Locomotion Yard](../examples/locomotion-yard/README.md) | Matching 0.0.75 engine/SDK, pinned original CC0 FBX body and separate takes, Python and .NET 10 SDK; simulation, navigation and the selected compiled gameplay backend. Windows captures need Vulkan and game UI; export needs a matching installed runtime and Windows Native AOT artifact. | Independent original-source pose/skin and cumulative playback checks, native physical detour, compatible reload and exact midfade save continuation; relocated source-free player runs before separate bundled-runtime qualification. |
| Author a blockout and capture its appearance | [Courtyard scene](SCENE_CAPTURE.md#try-the-example) | Native authoring executable and new world path; capture additionally needs a Windows Vulkan renderer, display and supported hardware | Inspect the transaction result and capture metadata, then view the saved image. Linux authoring-only builds can create the scene but cannot capture it. |

The game examples use deliberately small fixtures. Scripted input and renderer
readbacks do not establish physical mouse/controller behavior or game-scale
performance. Collection Room's development directory is not an exported game
bundle; its guide links to the separate native-gameplay and project export
contracts. Character Yard includes a tested Windows export verifier that removes
only its owned source project before running the relocated game. Its single
stationary source take does not supply a locomotion library. Locomotion Yard
extends the compiled workflow to separate original idle/run clips, explicit
retargeting and controller-derived playback rates; it does not supply foot
locking, IK or automatic stride fitting.

## Continue an existing project

These guides provide operations to compose into your own workflow. Supply the
project, entity IDs, revisions and assets they require; they do not build a
complete game for you.

| Task | Guide and prerequisites | Check the result |
| --- | --- | --- |
| Edit a session already open in an editor or headless host | [Shared sessions](SHARED_SESSIONS.md) and [Python connection/recovery](PYTHON_CLIENT.md#share-an-editor-or-headless-host); running host, endpoint name and matching native bridge executable | Read the current revision, submit guarded edits and inspect their result. Detaching a client leaves the owner running. |
| Compose separate character clips and diagnose a rejected skeleton | [Separate clips](FBX_IMPORT.md#compose-separate-clips) and [structured recovery](FBX_IMPORT.md#inspect-a-composition-failure); native authoring build, actual base/donor files and an open world client | Inspect the cooked clip list on success. On rejection, inspect the first donor's bounded node/frame report and confirm world revision, history and published assets remain unchanged; correct source compatibility before retrying. This does not retarget clips. |
| Convert different bone axes at an explicit common reference pose | [Reference-frame conversion](ANIMATION_FRAME_TRANSFER.md); supporting native build, a skinned base, matching named ancestry, coincident reference joint origins and exactly uniform mapped scales | Inspect original reference selectors and parsed-model fingerprints, sample target joint/skin motion, then observe a render. Reject changed proportions or stance; root motion stays in the visual rig. The guide includes source-independent contract and optional Vulkan checks. |
| Inspect original takes and retarget their orientations onto a target body | [Explicit rotation retargeting](ANIMATION_RETARGETING.md); supporting native build, original base/donor sources, matching named ancestry and chosen reference poses | Guard every selection with inspected original-model fingerprints, declare target positions/scales and any selected translation deltas, then compare sampled and rendered motion. Import preserves target geometry/binds; controller speed, loop quality and foot contacts require separate authoring. |
| Bake and apply a procedural material | [Procedural materials](PROCEDURAL_MATERIALS.md#generate-inspect-and-apply) and [original recipes](../examples/materials/README.md); writable world, supported recipe and target geometry with suitable UVs/tangents | Poll the actual terminal job result, apply returned bindings through a guarded transaction and inspect the effective material. Observe a render when checking appearance. |
| Find authored users of an asset | [Asset references](ASSET_REFERENCES.md#query); current world and asset or owner identity | Follow every page at the observed revision. This reports authored bindings; use dependency validation separately to establish that resources resolve. |
| Investigate native execution cost | [Profiler](PROFILER.md#command-service) and [native jobs](JOBS.md#profiling); matching build and reproducible workload | Run the workload between recording start/stop, inspect the sealed capture and export it. Account for waits, overlapping intervals and dropped records. |
| Package a project with a native runtime | [Projects and export](PROJECTS.md#install-a-runtime-and-export); valid project manifest, cooked dependencies, installed feature-compatible runtime and exact matching engine version | Inspect the exported bundle and use the documented launch/verification workflow. Compiled gameplay requires the matching native artifact; saves use external mutable storage. |

## Compose and recover

Read the [authoring-core contract](AUTHORING_API_COMPATIBILITY.md) and
[deliberate recovery rules](PYTHON_CLIENT.md#failure-and-deliberate-recovery)
before handling stale edits or unknown outcomes. Retain the original request
and receipt when recovering an interrupted mutation. A successful command
acknowledgement is not always completion of a background operation.

Longer workflows should compose these guides and verify both semantic state
and the relevant image, sound or gameplay outcome. Future terrain, FPS and
other capability plans are in the [roadmap](ROADMAP.md); they are not runnable
instructions. Codex and Claude skills will serve as compact entry points to
qualified workflows under the [documentation standard](DOCUMENTATION.md).
