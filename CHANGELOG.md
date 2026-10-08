# Changelog

Version numbers identify source checkpoints. Feature qualification and packaged editor availability are recorded separately in [Implementation status](docs/IMPLEMENTATION_STATUS.md). During prototype development, APIs and file formats can change between checkpoints.

## 0.0.55 — Unreleased

This checkpoint adds native inertial animation transitions with persistent motion history. The installed desktop package remains unchanged.

- Opt-in `transition_mode:"inertial"` preserves the exact starting pose and estimated recent output motion, then settles onto the destination over fixed ticks. Translation, quaternion and logarithmic positive-scale corrections use analytic incoming derivatives; interruptions retain one correction rather than a recursive blend tree. [Contract](docs/RUNTIME_ANIMATION.md#inertial-transitions).
- Immutable distinct-tick history and corrections participate in batch rollback and nested animation-state v2 saves. Fresh-process and whole-runtime restoration checks include immediate re-interruption. Crossfade-only version-1 save bytes are retained; an actual 0.0.54 saved interrupted fade restores and continues unchanged.
- Windows/Linux native and protocol checks pass, as do authoring-only checks and the existing compiled C# crossfade/reload/rollback suite. Both laptop GPUs pass 48 exact reference pairs across 96 inertial/legacy captures at 640×480 with 1×/4× MSAA and zero reported NVRHI errors. [Evidence](docs/evidence/m2-animation-inertial.json).
- Discovery advances to revision 52. Gameplay services ABI 7 and authoring-core v1 are unchanged. C# and desktop controls still select crossfades; the new mode currently uses C++ or the world protocol. Layers, IK, foot locking, phase matching and game-scale animation performance remain unfinished.

## 0.0.54 — Unreleased

This checkpoint adds coherent, bounded runtime observations for agent playtesting. The installed desktop package remains unchanged.

- `runtime.observe` joins selected live entity state and native custom-component values in one guarded owner dispatch. Field projection, shared schema metadata, sorted membership pages and revision pins reduce repeated reads without changing simulation. [Protocol](docs/RUNTIME_OBSERVATION.md).
- A reproducible Windows comparison runs the same compiled Workshop Relay game with both observation paths. It preserves thirteen game checks, thirteen route events and six complete checked snapshots, normalizing only runtime session IDs. Native RPCs fall from 2,549 to 1,292; returned JSON bodies are 16.9% smaller under the documented reserialization measure. [Evidence](docs/evidence/m2-runtime-observation.json).
- Nine native protocol tests pass on Windows and Linux. Shared/read-only scope checks, an actual authoring-only build and selected stable authoring-contract gates pass. Discovery advances to revision 51; gameplay ABI, save formats and authoring-core v1 remain unchanged. This provider-free comparison does not measure agent tokens, latency or game performance.

## 0.0.53 — Unreleased

This checkpoint adds a recorded agent-authored inventory and recipe game, with a provider-free public replay. The installed desktop package remains unchanged.

- Codex creates and playtests Workshop Relay through native MCP authoring: a generated three-slot int32 inventory, actual spawned pickups, recipes, deferred Drop, wine-styled HUD/menu and compiled controls. Independent tests observe resources directly, use genuine controller inputs and check full checkpoint restoration and fresh-process continuation. [Source and reproduction](docs/evidence/fixtures/agent-workshop/README.md).
- The retained source rebuilds on Windows with zero warnings or errors. Its replay passes 2,549 native requests on the final host; both owned native processes exit cleanly. The original exercise has separate verification and two inspected NVIDIA Vulkan checkpoints. [Evidence](docs/evidence/m2-agent-workshop.json).
- Public documentation records the generated game's fixed Drop-placement and premature save-label limitations. This is one bounded prototype exercise, with no claim of general agent success rate, physical input, production performance or new gameplay/API implementation.

## 0.0.52 — Unreleased

This checkpoint qualifies the existing generated component collections in Windows Native AOT games. The installed desktop package remains unchanged.

- The actual inventory fixture publishes with its generated component manifest and dependency notices. Compiled capacity-four entity/int32 buffers pass the unchanged 151-RPC mutation, rejection, rollback and fresh-process save contract.
- Exported native games run seven ticks after relocation outside the checkout on AMD and NVIDIA. Both retain all 56 bundle files, report disabled dynamic code and zero NVRHI errors, and contain no managed PE/bridge/CoreCLR payload. Separate save worlds also pass through each bundled runtime. [Evidence](docs/evidence/m2-component-collections-native-windows.json).
- A reusable bounded Windows bundle qualification runner records owned command exits and cleanup. This is fixture qualification, with no new gameplay implementation, ABI or catalog change; clean-machine deployment, other collection kinds, capacity migration and production performance remain unqualified.

## 0.0.51 — Unreleased

This checkpoint qualifies the existing bounded component collections on Windows CoreCLR with the actual compiled inventory fixture. The installed desktop package remains unchanged.

- Matching SDK, source generator and bridge build the fixture and extract its real component manifest. Compiled callbacks exercise ordered entity/int buffers, capacity rejection, published reads and actual template spawning.
- Reference/index/exception failures and a later-tick batch failure preserve complete saved payload bytes. Nonempty inventories restore and continue in two fresh processes, including atomic reference repair and second-generation saves. [Windows evidence](docs/evidence/m2-component-collections-windows.json).
- Documentation distinguishes bounded generated buffers from arbitrary managed arrays. Historical Linux qualification is retained separately; Native AOT collections, global-state collections, nested buffers and capacity migration remain unqualified. No gameplay ABI or API catalog change is introduced.

## 0.0.50 — Unreleased

This checkpoint gives agents typed control over native runtime HUD/menu layouts and styling. The installed desktop package remains unchanged.

- Optional frozen layout/style fields support responsive dp/percent dimensions, anchored placement, flex rows/columns, spacing, visual ordering, palettes, borders, font size and color states. Native validation rejects malformed metadata and unsupported rounded clipping. Older definitions retain absent fields and their default presentation. [UI contract](docs/GAME_UI.md#authored-layout-and-styling).
- `world.ui.layout` observes a revision-pinned virtual viewport with bounds, clipping and hit eligibility when the native UI backend is built. An explicit runtime-capture density matches layout measurements independently of monitor DPI, while omission retains the earlier behavior. Runtime inspection exposes frozen metadata; these UI interfaces remain development APIs outside authoring-core v1.
- Pass-through canvases let HUDs leave gameplay input free. Focus follows visual traversal; pointer and keyboard gestures cancel across action or occlusion changes while retaining release ownership. Confirmation cannot activate a fully occluded control.
- A centered menu/HUD fixture exercises transactions, history, retries, runtime freezing, save-content binding and rendered presentation. Theme assets, images, inventory/text widgets, UI animation, visual authoring and broad accessibility remain unfinished. Both laptop GPUs pass the styled capture suite; seven default-UI capture pairs on AMD are pixel-identical to 0.0.49. [Evidence](docs/evidence/m2-authored-ui.json).

## 0.0.49 — Unreleased

This checkpoint releases authoring-core v1 for a selected authored-world API. The broader engine remains a prototype, and the installed desktop package remains unchanged.

- Native session discovery advertises the contract identity, selected methods/Transform scope and mode-specific mutation availability across full and focused views. Catalog revisions remain independent; runtime, rendering, game saves, gameplay ABI and world formats are outside this guarantee. [Contract](docs/AUTHORING_API_COMPATIBILITY.md).
- Canonical v1 request/response artifacts and a hashed release manifest preserve the earlier candidate schemas and paths. The 0.0.49 Python wheel uses the canonical response resource while retaining the historical one.
- A separate conservative response gate checks that new producer outputs fit old consumer requirements. Recognized required fields, types, bounds and extras are directional; unsupported interacting context fails closed. This does not replace native behavioral qualification.
- Focused native conformance checks caller IDs/errors, old transaction/undo/redo receipts after later edits and restart, true hierarchical shear, keep-local reparenting, pinned query boundaries and storage rejection without consuming history or retry IDs. Windows/Linux request-scope, shared-host and client checks qualify this bounded release. [Evidence](docs/evidence/m2-authoring-core-v1.json).

## 0.0.48 — Unreleased

This checkpoint adds focused mutation discovery for external agents and automation. The installed desktop package remains unchanged.

- `world.describe` can select a transaction operation and optional component type while preserving revision/receipt guards, previews and operation bounds. Read-only/shared restrictions apply first; registered custom types and removal enums keep their original schemas. Full and existing focused views remain available. [Discovery contract](docs/WORLD_SERVICE.md#focused-discovery-development).
- MCP advertises the new selectors, and the 0.0.48 Python client adds `discover_mutation()` with contextual response checks. Native Windows/Linux tests cover advertised selections, observational errors, scope restrictions and unchanged mixed transactions. Installed-wheel checks qualify the updated package.
- Conservative projection retains complete unions when references or unfamiliar schema context prevent pruning. An indexed-reference regression rejects the old contract exporter. The unchanged candidate baseline passes a narrow closed-object union-extension proof; this does not release full-engine API stability.
- A Windows Transform selection produces 1,318 native response bytes versus 27,779 for the complete transaction method; its MCP response is 2,910 bytes versus 60,276. Counts include JSON-RPC envelopes and exclude line delimiters. Response bytes are not provider tokens or measured development speed. [Evidence and reproducible requests](docs/evidence/m2-scoped-mutation-discovery.json).

## 0.0.47 — Unreleased

This checkpoint replaces headless owner retry sleeps with transport readiness waits. The installed desktop package remains unchanged.

- Windows servers retain overlapped connect/read/write operations with private completion events; Linux servers wait on eligible socket readiness. Owner polling still bounds each client's I/O, dispatches requests serially and preserves deferred reply ordering. Pending Windows operations are canceled and reaped before buffer/slot reuse. [Transport contract](docs/SHARED_SESSIONS.md#transport-and-limits).
- Native regressions cover fragmented frames, synchronous completions, budget continuation, deferred pipelines, full client capacity, slow readers and endpoint reuse. Windows/Linux client and shared-host checks, candidate request gates and ten Windows desktop ABI checks pass. The unchanged compiled defense game reproduces all recorded outcomes across 3,208 calls with clean exits.
- Four matched Windows trial pairs reduce shared read-query median from 16.03 ms to 0.159 ms. Five-second idle CPU probes on Windows and Linux record smaller CPU-time deltas, with finite accounting granularity. These are authoring and idle-host measurements, not game FPS, GUI latency or power measurements. [Evidence and limits](docs/evidence/m2-host-readiness.json), [idle probe](tools/benchmark_shared_idle.py).

## 0.0.46 — Unreleased

This checkpoint replaces Windows client exchange retry sleeps with completion waits. The installed desktop package remains unchanged.

- Overlapped pipe reads and writes share one exchange deadline. Timeout or failure cancels and reaps pending operations before releasing buffers, closes the client and never replays work. Kernel cleanup can outlast the requested deadline; server polling and editor cadence are unchanged. [Transport contract](docs/SHARED_SESSIONS.md#transport-and-limits).
- Native Windows regressions cover stalled responses, write backpressure, disconnects, fragmented deadlines and endpoint recovery. Shared-host, SDK, MCP and native desktop ABI checks pass. The unchanged compiled defense game preserves every recorded checkpoint and win/loss outcome with clean process exits.
- Four matched Windows trial pairs reduce shared read-query median from 31.4 ms to 15.9 ms; an old-build repeat remains at 32.2 ms. This is same-machine authoring latency, not game FPS or GUI responsiveness. [Measurement and limits](docs/evidence/m2-client-completion-waits.json).

## 0.0.45 — Unreleased

This checkpoint removes an avoidable local reply wait and adds a reproducible authoring-latency probe. The installed desktop package remains unchanged.

- Native replies attempt immediate nonblocking delivery within a 256 KiB budget, retaining partial frames for later owner polls. No extra request polling, transport threads, busy spin, automatic retry or global timer change is introduced. [Transport contract](docs/SHARED_SESSIONS.md#transport-and-limits).
- Windows/Linux transport regressions cover immediate small/empty replies, slow readers, other-client progress and pipelined frame ordering. The new regression rejects the old queued-only backend. Shared-host, SDK, MCP and native desktop ABI checks pass; the compiled defense game also passes shared-host checkpoint continuation with clean process exits.
- Four matched Windows trial pairs per build reduce the shared read-query median from 47.6 ms to 31.8 ms on the development machine. Client sleeps and editor dispatch cadence remain unchanged; this is authoring latency evidence, not game FPS or GUI responsiveness. [Measurement and limits](docs/evidence/m2-immediate-replies.json), [probe](tools/benchmark_shared_session.py).

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
