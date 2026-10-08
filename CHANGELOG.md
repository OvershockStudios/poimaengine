# Changelog

Version numbers identify source checkpoints. Feature qualification and packaged editor availability are recorded separately in [Implementation status](docs/IMPLEMENTATION_STATUS.md). During prototype development, APIs and file formats can change between checkpoints.

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
