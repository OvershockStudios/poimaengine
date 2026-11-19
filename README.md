# Poima Engine

**An open-source game engine for people building with coding agents.**

Poima lets you and an agent work on the same 3D game. You can build and inspect a scene in the desktop editor. Codex or Claude can edit it, drive the player, inspect what happened, and replay a test through the engine’s CLI or API. Poima uses a native C++20 core, Vulkan graphics, and C# gameplay.

[Get started](#get-started) · [Connect an agent](docs/AGENT_CLIENTS.md) · [Documentation](#documentation) · [Roadmap](docs/ROADMAP.md) · [Changelog](CHANGELOG.md)

## Winter Village

[![Watch the Winter Village showcase featuring a torch-lit village in Poima](https://i.ytimg.com/vi/OjqWUCXsNX8/maxresdefault.jpg)](https://www.youtube.com/watch?v=OjqWUCXsNX8)

[Watch the one-minute gameplay showcase](https://www.youtube.com/watch?v=OjqWUCXsNX8). Built and playtested with Codex in Poima, Winter Village follows a hunter through a snowy forest, a village, and down to the harbor. The clip is a continuous take from the native player. Some scene and gameplay effects were authored specifically for the showcase.

## Build and playtest with an agent

- **Edit safely.** Group scene changes into one transaction. Poima catches stale edits and can safely retry a request without applying it twice. [Guarded authoring example](examples/guarded-authoring)
- **See what happened.** Run player inputs and inspect committed gameplay state, poses, UI, and captures. The editor and automation clients use the same engine operations. [Live player](docs/LIVE_PLAYER.md) · [Runtime observations](docs/RUNTIME_OBSERVATION.md)
- **Verify and ship a build.** Replay the same inputs after a fix, save the game, and resume from a standalone Windows build. These workflows can run without opening the editor. [Example workflow](examples/locomotion-yard/README.md#author-playtest-and-export-without-a-window) · [Projects and exports](docs/PROJECTS.md)

Four agent-built 3D games passed independent gameplay, save continuation, and standalone Windows checks. These are small games with scoped verification. [Alpha release gates](docs/ALPHA_ROADMAP.md)

| Game | Agent | What was tested |
| --- | --- | --- |
| [Switchback Range](docs/evidence/m2-agent-switchback-fresh.json) | Codex | First-person combat, death and retry, and checkpoint continuation. |
| [Pump House](docs/evidence/m2-agent-pumphouse.json) | Claude | Inventory, interactions, and repairs that persist through a save. |
| [Beacon Foundry](docs/evidence/m2-agent-beacon-foundry.json) | Codex | A production queue, spawned entities, and an explicit save upgrade. |
| [Knight: Two Signals](docs/evidence/m2-agent-character-fresh.json) | Codex | An imported character, a two-stage route, and restored progress in a relocated build. |

## What works today

Poima is in pre-alpha. Current workflows target Windows for graphics and the desktop editor, with Linux headless authoring and simulation. See the [implementation status](docs/IMPLEMENTATION_STATUS.md) for tested configurations, limits, and evidence.

| Area | Current foundation |
| --- | --- |
| **Graphics** | Vulkan PBR, clustered lights, soft directional shadows, environment lighting, skinning, first-person viewmodels, and GPU profiling. [Renderer documentation](docs/RENDER_DIAGNOSTICS.md) |
| **Characters** | glTF/GLB and bounded FBX intake, layered animation, inertial blending, IK, foot placement, and physical rigs. [Animation](docs/RUNTIME_ANIMATION.md) · [Physical rigs](docs/PHYSICAL_RIGS.md) |
| **Gameplay** | Compiled C#, development reload, Native AOT Windows builds, physics, navigation, input actions, and save slots with explicit upgrades. [C# gameplay](docs/MANAGED_GAMEPLAY.md) · [Save system](docs/SAVE_SYSTEM.md) |
| **Player and tools** | Native game UI, keyboard/mouse/gamepad profiles, spatial audio, and a desktop editor with independent Scene and Game views. [Editor](docs/DESKTOP_EDITOR.md) · [Game UI](docs/GAME_UI.md) · [Audio](docs/AUDIO_EVENTS.md) |

Production graphics and animation, water and weather, and the rest of the Windows single-player workflow are tracked in the [roadmap](docs/ROADMAP.md).

## Get started

1. **Get Poima.** Use the [build and installation guide](docs/BUILD.md) to set up the current pre-alpha engine.
2. **Make a project.** With Poima on your path, run `poima project create MyGame --name "My Game"`.
3. **Open the `MyGame` folder in Codex or Claude Code.** [Connect Poima once](docs/AGENT_CLIENTS.md#connect-a-project), then describe what you want to play. You can use the desktop client or launch the coding agent from that folder.

> Make a small 3D puzzle room with three switches that unlock a door. Playtest the solution and tell me what you checked.

Open the same project in the [Poima editor](docs/DESKTOP_EDITOR.md#build-and-launch) whenever you want to inspect, edit, or play it yourself. To edit together live, connect your agent to the [shared session](docs/SHARED_SESSIONS.md). You can also work directly in the editor.

### Examples

- **Build a small game:** [Collection Room](examples/collection-game) shows first-person movement, interactions, HUD, and save continuation.
- **Import a character:** [Locomotion Yard](examples/locomotion-yard) connects imported clips, movement, and a standalone export.
- **Try physical animation:** [Physical Character Yard](examples/physical-character-yard) demonstrates impacts, physical takeover, and controllable joint targets.

## Documentation

- [Set up Codex or Claude](docs/AGENT_CLIENTS.md) and follow the [agent iteration guide](docs/AGENT_ITERATION.md).
- [Discover the world API](docs/WORLD_SERVICE.md) or use the [Python client](docs/PYTHON_CLIENT.md).
- [Find a task playbook](docs/PLAYBOOKS.md) for authoring, testing, profiling, and export.
- [Track Alpha 1](docs/ALPHA_ROADMAP.md) and see the [reference coverage](docs/DOCUMENTATION_COVERAGE.md).

## Credits and license

Created and architected by Divesh Gupta ([Legendile7](https://github.com/Legendile7)), publishing as Overshock Studios. Implementation with Codex, including GPT-6-Astra, under his direction.

Poima is licensed under [Apache-2.0](LICENSE). See [third-party notices](THIRD_PARTY_NOTICES.md) and the [branding notice](TRADEMARKS.md).
