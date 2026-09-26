# Poima Engine

A planned native 3D engine for the user's own games, with agent-native authoring, next-generation graphics, a headless API, CLI, optional editor and Vulkan graphics on PC. Permissive open source is a community benefit.

**Status:** M0 foundation work continues, with the first M1 authored-world service now implemented. The native CLI passes tests on Linux/WSL and Windows. An optional Vulkan/NVRHI experiment presents and captures a triangle on the laptop's NVIDIA and AMD GPUs. The persistent headless world service supports hierarchy, transform, camera and box-renderer transactions, with a Vulkan scene-capture path. An optional EnTT/Jolt runtime now supports fixed-step collision, capsule control and live captures. A continuous native player now supports keyboard/mouse input, deterministic visual replay and window resizing. Static glTF geometry now imports into editable hierarchies and renders with an initial PBR path supporting PNG/JPEG material maps, normal maps and mipmaps. Independent image imports and per-object texture overrides are exposed through the world API. Directional, point and spot lights, ambient fill, exposure and optional shadow maps are editable and inspectable through the same API. Conservative camera/shadow frustum culling, draw counters and optional CPU/GPU timing support measured iteration. Native collider raycasts and timed kinematic motion now support queryable, solid moving doors through steps and replay. An optional C# gameplay module now handles use-button interactions, with native-owned inspectable state and compatible-field reload. Native WAV import, acoustic material/emitter authoring and optional Steam Audio captures now expose sound obstruction and HRTF output from authored/live worlds. Native sound events, C# play/stop, temporal audio recording and optional player device output now use the same persistent mixer. Advanced graphics, production audio scheduling, the complete gameplay SDK and packaging remain unfinished. See [audio events](docs/AUDIO_EVENTS.md).

- [Build and run the bootstrap](docs/BUILD.md)
- [Import static glTF assets and edit materials](docs/ASSETS.md)
- [Inspect visibility and render timings](docs/RENDER_DIAGNOSTICS.md)
- [Configure and verify shadow maps](docs/SHADOWS.md)
- [Author and inspect scene lighting](docs/LIGHTING.md)
- [Author texture slots and normal maps](docs/MATERIAL_AUTHORING.md)
- [Open the native player or replay input](docs/PLAYER.md)
- [Author and capture native acoustics](docs/AUDIO.md)
- [Write and reload C# gameplay](docs/MANAGED_GAMEPLAY.md)
- [Query physics and move solid doors](docs/PHYSICS_INTERACTIONS.md)
- [Run physics and inspect live state](docs/RUNTIME.md)
- [Author and capture a 3D scene](docs/SCENE_CAPTURE.md)
- [Persistent world service and protocol](docs/WORLD_SERVICE.md)
- [Implementation status and evidence](docs/IMPLEMENTATION_STATUS.md)
- [Native module iteration experiment](docs/NATIVE_MODULE_LAB.md)
- [C# gameplay development experiment](docs/MANAGED_GAMEPLAY_LAB.md)
- [C# native shipping experiment](docs/MANAGED_SHIPPING_LAB.md)
- [C# gameplay direction and runtime research](docs/research/2026-09-26-csharp-gameplay.md)

- [Architecture and delivery plan](docs/ENGINE_PLAN.md)
- [Validation workloads and performance targets](docs/VALIDATION_PLAN.md)
- [Product requirements](docs/PRODUCT_BRIEF.md)
- [Research dossier](docs/research/README.md)

The bootstrap uses C++20, with NVRHI, SDL3 and DXC integrated for the optional graphics experiment. The tested Windows executable is cross-compiled from WSL with a pinned workspace-local LLVM-MinGW toolchain and runs natively on Windows. Authoring with Codex, Claude and the optional editor must fit the user's laptop. Shipped games have separate performance requirements across supported PC hardware tiers and GPU vendors, with current-generation consoles planned after platform access. The laptop is one player-performance reference. M0 measures build, reload and integration feasibility before the larger engine depends on those choices.
