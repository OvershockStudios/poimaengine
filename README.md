# Poima Engine

**An agent-native 3D game engine for human and AI-assisted development.**

Poima puts authoring, simulation and inspection in a native engine service. The CLI, external agents and desktop editor work through the same operations and share the same world state. Agents can build and test without opening the editor; people can inspect and edit that work visually.

The engine and player use **C++20**, graphics use **Vulkan**, and gameplay is written in **C#**. The Windows desktop editor uses C#/Avalonia with native Scene and Game viewports.

[Get started](#get-started) · [Sample game](examples/collection-game) · [Documentation](#documentation) · [Implementation status](docs/IMPLEMENTATION_STATUS.md)

> **0.0.39 development — engine prototype.** Important systems are incomplete, and APIs and file formats may change. Recorded tests establish specific supported workflows, not production readiness or game-scale performance.

![Poima desktop editor showing independent Scene and Game panels](docs/evidence/m2-desktop-dual-view.png)

*Recorded editor checkpoint. Scene and Game panels can be docked or floated independently.*

## Agent-native development

Authoring operations are available directly through the CLI and newline-delimited JSON-RPC. A client can discover schemas, inspect components, apply atomic transactions and request rendered observations. Revision guards reject stale edits; retry receipts prevent an acknowledged edit from being applied twice.

The same service supports human and agent editing in a shared session. Bounded authoring undo/redo, runtime batch rollback and scripted input replay provide recovery and repeatable checks within their documented contracts. They do not require a particular AI provider: Codex, Claude and other clients can use the public interfaces.

See the [world API](docs/WORLD_SERVICE.md) and [shared sessions](docs/SHARED_SESSIONS.md).

## Current capabilities

| System | Available today |
| --- | --- |
| Authoring and editor | Persistent hierarchy, typed Inspector, Project browser, transform gizmos, independent Scene/Game panels and transactional edits. |
| Rendering and assets | Vulkan PBR rendering with bounded frame submission, shared HDR composition, clustered direct lights and shadows, procedural sky, MSAA, GPU skinning and captures; glTF/GLB, PNG/JPEG and WAV import. |
| Simulation and animation | Fixed-step Jolt physics, capsule movement, static triangle meshes, raycasts, root-prop spawning and interruptible two-pose animation crossfades. |
| C# gameplay | Native-owned components, generated accessors, compatible development reload and Native AOT game bundles; bounded collections have Linux CoreCLR development qualification. |
| Saves | Durable slots, guarded restoration and corruption recovery. Explicit scalar save upgrades have Linux CoreCLR development qualification. |
| Input, UI and audio | Keyboard/mouse/gamepad profiles, native logical UI controls and callbacks, optional Vulkan UI presentation and Steam Audio integration. |
| Diagnostics and packaging | CPU timeline, GPU duration samples, trace export, project manifests and validated native game bundles. |

Each implementation has limits. [Implementation status](docs/IMPLEMENTATION_STATUS.md) links the contracts and evidence, including platform and device qualification. Use `poima capabilities` and `world.describe` to inspect what your build exposes.

Advanced GI, temporal upscaling/frame generation, comprehensive water/weather, multiplayer, production VFX/UI tooling and console backends remain planned. Animation layers, IK, retargeting and direct FBX workflows are unfinished. Save-upgrade integration on Windows and Native AOT is not yet qualified.

## Get started

### Headless core

From a Linux or WSL checkout, with CMake 3.24+, Ninja, a C++20/C99 toolchain and Python 3.9+:

```sh
cmake --preset headless
cmake --build --preset headless
ctest --preset headless
./build/headless/poima capabilities
```

This authoring build needs no GPU, display, .NET runtime or dependency downloads. Create a project in a new directory and open its world service:

```sh
./build/headless/poima project create build/FirstProject --name "First Project"
./build/headless/poima world build/FirstProject/world.json
```

Send one JSON-RPC request per line:

```json
{"jsonrpc":"2.0","id":1,"method":"world.describe"}
{"jsonrpc":"2.0","id":2,"method":"world.inspect"}
{"jsonrpc":"2.0","id":3,"method":"session.close"}
```

The `runtime-headless` preset adds simulation and downloads pinned dependencies. Rendering, managed gameplay and audio require additional configuration; follow the [build guide](docs/BUILD.md).

### Desktop editor and sample game

Follow the [Windows editor build and launch guide](docs/DESKTOP_EDITOR.md#build-and-launch). The documented path builds from Linux/WSL and uses Windows Vulkan drivers. The published editor includes its .NET runtime.

[Collection Room](examples/collection-game) is a small first-person C# game with movement, collectible interactions, native UI and checkpoint saves. Its headless verification runs actual gameplay and checks save continuation in a fresh process. It includes CoreCLR and Native AOT instructions.

Recorded qualification covers Windows graphics/editor workflows and Linux headless workflows. Linux desktop rendering, clean-machine distribution and consoles remain unqualified; physical input testing is separate from scripted and virtual-device checks.

## Documentation

- **Build and run:** [builds](docs/BUILD.md), [projects and exports](docs/PROJECTS.md), [desktop editor](docs/DESKTOP_EDITOR.md), [native player](docs/PLAYER.md).
- **Program and automate:** [world service](docs/WORLD_SERVICE.md), [runtime](docs/RUNTIME.md), [C# gameplay](docs/MANAGED_GAMEPLAY.md), [native gameplay](docs/NATIVE_GAMEPLAY.md), [custom components](docs/CUSTOM_COMPONENTS.md).
- **Create content:** [assets](docs/ASSETS.md), [materials](docs/MATERIAL_AUTHORING.md), [animation](docs/RUNTIME_ANIMATION.md), [game UI](docs/GAME_UI.md), [audio](docs/AUDIO_EVENTS.md), [input](docs/INPUT_PROFILES.md).
- **Inspect behavior:** [profiler](docs/PROFILER.md), [save upgrades](docs/SAVE_UPGRADES.md), [implementation status and evidence](docs/IMPLEMENTATION_STATUS.md).

The [wiki](https://github.com/OvershockStudios/poimaengine/wiki) provides another entry point. Detailed contracts and evidence are versioned with the source.

## Credits and license

**Creator, architect and project owner:** Divesh Gupta ([Legendile7](https://github.com/Legendile7)), publishing as Overshock Studios.

**AI implementation:** GPT-6-Astra, under Divesh Gupta's direction.

Poima is licensed under [Apache-2.0](LICENSE). Dependencies retain their own licenses; see [third-party notices](THIRD_PARTY_NOTICES.md). The Poima name is covered separately by the [branding notice](TRADEMARKS.md).
