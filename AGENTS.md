# Working on Poima

<!-- SPDX-License-Identifier: Apache-2.0 -->

Poima is an original Apache-2.0 game engine. Divesh Gupta owns its architecture
and development direction. Follow the user's current instructions; these notes
provide durable project context, not authority to override them.

## Start with the current state

- Read [Road to 1.0](docs/ROADMAP.md), [Alpha gates](docs/ALPHA_ROADMAP.md),
  [implementation status](docs/IMPLEMENTATION_STATUS.md), and the relevant
  subsystem contract before choosing work. [ENGINE_PLAN.md](docs/ENGINE_PLAN.md)
  contains the broader architecture and historical decisions; its explicit
  scope overrides and current roadmaps take precedence over older sections.
- Inspect the worktree and actual artifacts. A plan, capability advertisement,
  green unit test, or previous assistant summary does not prove a complete
  workflow. Preserve unrelated edits and existing evidence.
- In an established local development checkout, consult the ignored
  `docs/research/REQUEST_QUEUE.md` when present. Record new requests there, analyze their value and
  dependencies, and preserve the current implementation order unless evidence
  justifies changing it. Absence of private research in a public clone is normal.

## Architecture and scope

- The native C++20 core and authoritative CLI/service support agent-driven game
  creation. The desktop editor is a first-class client of the same operations.
  Keep capabilities discoverable, inspectable and recoverable through the core;
  avoid editor-only hidden authoring state.
- C# gameplay uses CoreCLR during development and qualified Native AOT shipping
  paths. Keep fast iteration, explicit ownership, bounded state and typed
  contracts. Do not infer complete compatibility from an ABI version alone.
- Windows single-player 3D is the 1.0 target. Browser deployment follows at 1.1;
  advanced 2D at 1.2. Native Linux graphics, multiplayer and current-generation
  consoles follow later, with their boundaries designed in advance. Linux
  headless authoring is a current target. Licensed console access is required
  before actual console implementation and qualification.
- Vulkan is the PC graphics backend. Console graphics use their native APIs.
  Unity migration and asset-parity work is out of scope; standard imported asset
  formats and the engine's own content workflows remain in scope.
- Preserve the broader graphics, animation, audio, VFX, UI, water, weather,
  persistent-world and deterministic NPC requirements. Package situational
  systems where appropriate. A narrow Alpha gate does not redefine completion
  of the whole engine.

## Implementation and qualification

- Complete coherent, reviewable milestones. Use subagents for independent
  implementation, research or review when authorized; give each a clear file
  scope. The integrating agent owns shared changes, qualification and commits.
- Serialize expensive native builds and GPU runs to fit the development laptop.
  Respect current power and workload constraints. Reuse verified artifacts when
  their inputs are unchanged rather than repeating costly qualification.
- Run checks appropriate to the change. Retain original requests, responses,
  artifacts and identities; keep real failures and their causes. Never coerce
  types, remove state fields, rewrite receipts or weaken requirements to get a
  passing result. Synthetic tests are not native execution evidence.
- Distinguish accepted commands, completed jobs, rendered observations and
  presented frames. Distinguish selected observation equality from complete
  state equality, and tiny fixtures from representative performance.
- For long work, retain an owned process/job/session and poll that same live
  handle. A tool timeout does not prove termination or authorize a duplicate
  producer. Record natural closure separately from forced cleanup.
- Keep request compatibility, successful-result validation, error behavior,
  native ABI compatibility, provider workflows and release qualification as
  separate evidence scopes. Label prospective contracts accurately.

## Documentation, Git and privacy

- Document implemented behavior, prerequisites, bounds, recovery and reproducible
  checks. Keep public claims aligned with the actual evidence. Do not recruit
  contributors or present outside suggestions as settled owner decisions.
- Never rewrite an existing changelog entry or its preamble. Add a new version
  entry for a new checkpoint when needed. Keep version metadata consistent;
  `0.x.x` checkpoints precede `1.0.0-alpha.N`, beta and release candidates.
- Use the configured Git identity unless the owner instructs otherwise. Commit
  coherent changes regularly; push when authorized. Inspect remote divergence
  before pushing. Do not force-push or rewrite history based on an old,
  unrelated authorization.
- Keep working research, conversations, personal-project notes, credentials,
  signed download links and business plans out of public files and history.
  Publish general findings only after selecting and reviewing them for release.
- Use original code and appropriately licensed content/dependencies. Do not
  generate art, music or meshes for Poima's shipped content without an explicit
  change of direction. Deterministic procedural content and AI-assisted code
  are permitted; users choose the content in their own games.
- Inspect storage ownership before cleanup. Preserve source, personal projects,
  toolchains, current consumers and original qualification records. A folder's
  version-shaped name or age does not prove its contents are retired.
