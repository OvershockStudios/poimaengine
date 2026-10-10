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
| Agent authoring | Three recorded Codex/Claude game exercises plus [DEPOT RUN](evidence/fixtures/agent-depot/README.md): licensed imports, native publish/export and independent relocated-game checks after explicit infrastructure recovery. Focused discovery, transactions, guards, retries, observations and replay. [Workflows](AGENT_CLIENTS.md). | Repeat complete create → import → author → compile → playtest → repair → export workflows from fresh projects. DEPOT RUN's first attempt needed owner recovery; it does not close the repeated fresh-workflow gate. Report failed attempts and recovery, not only successful final runs. Every operation used must have documented machine-readable results and actionable errors. |
| Contracts and persistence | Selected [authoring-core v1](AUTHORING_API_COMPATIBILITY.md) is stable; C# components, reload, saves and bounded upgrades have recorded qualification. The [gameplay/content/save profile](ALPHA_GAMEPLAY_PROFILE.md) defines the current boundary and [Relay Yard upgrade](../examples/relay-yard/UPGRADING.md) exercises a retained real game. | Release the selected alpha contract and compatibility gates beyond stable authoring. Extend retained-game coverage where supported component/content changes are needed; clearly identify experimental APIs. Alpha does not freeze every engine API. |
| Content and characters | Bounded FBX/glTF intake, images, WAV, cooked assets, provenance and native animation have evidence. [Character Yard](../examples/character-yard/README.md) covers weighted GLB; [Locomotion Yard](../examples/locomotion-yard/README.md) covers original FBX body/takes, controller-driven idle/run, checkpoints and relocated Windows export. | Publish the supported character intake/rig/material profile and actionable unsupported-content diagnostics. Extend beyond the single pinned FBX pack; integrate this path into the fresh-project agent workflow. Contact/IK and broader exporter coverage remain separate gaps. |
| Playable systems | Physics, character controls, static navigation, keyboard/mouse/gamepad profiles, [portable and live native preferences](PLAYER_SETTINGS.md), styled game UI, audio, checkpoints and compiled callbacks exist. [Relay Yard audio](evidence/m2-relay-yard-audio.json) combines licensed gameplay cues with continuous playback and active-sound checkpoints. | [Relay Yard](../examples/relay-yard/README.md) exercises native pickups, animation/navigation, menus, settings, exact partial saves and fresh-process game completion. Representative performance and additional fresh-project workflows remain separate. Close blocking focus, input, text/layout and lifecycle defects; verify physical mouse/keyboard/controller interaction on Windows and qualify listening on actual audio hardware. |
| Observation and editor | Vulkan captures, CPU/GPU profiling, independent Scene/Game panels and typed inspection have evidence. | Keep visual observations and semantic state aligned through edits, Play, reload and recovery. Close blocking scene selection/navigation/docking defects; provide legible defaults and the agreed wine branding. Editor polish must support the workflow without replacing core work. |
| Performance and delivery | Windows rendering and Linux headless fixtures run on the development laptop. [Bounded frame/memory capture](FRAME_PERFORMANCE.md) has native NVIDIA/AMD integrity probes. [Performance Yard](evidence/m2-performance-yard.json) records a short combined workload; the [AO optimization comparison](evidence/m2-ao-optimization.json) reduces AMD GPU cost with bounded visual equivalence; the [camera-report comparison](CAMERA_OBSERVATION.md) removes redundant CPU extraction. AMD meets p99 but still misses the p95 timing budget. Production workload scale and the full reliability interval remain open. Native bundle workflows have bounded qualification. | Publish representative scene/game frame-time, memory and edit-to-play measurements with workload/settings/hardware identified. Verify a fresh installation and relocated exported game, complete dependency notices, reproducible instructions and a full alpha regression run. Tiny fixtures alone do not pass this gate. |

No gate is fully closed yet. Several foundations are implemented, but their
combined use, supported boundaries and delivery still need qualification.
The [implementation status](IMPLEMENTATION_STATUS.md) remains authoritative for
individual capabilities. A checklist row is not a claim that every feature
listed there is complete or production-ready.

### Qualification target

The initial alpha matrix is Windows native editor/player/rendering and Linux
native CLI authoring/simulation. Each needs a fresh-project regression and
source-independent cooked-content reopen. Linux desktop rendering, browser and
console deployment require later platform qualification.

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

These are targets to verify, not measurements already achieved. A workload
must exercise the shipped small game and its imported content; an empty scene
or deliberately undersized fixture cannot substitute for it. Budget changes
need an explicit technical reason and updated acceptance criteria.

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
