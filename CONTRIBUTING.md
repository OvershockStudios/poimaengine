# Contributing to Poima

Poima is an early game engine designed for humans and agents. Its desktop editor and programmatic tools are first-class clients of the same native authoring core. Contributions are most useful when they improve a concrete workflow and include a way to verify the result. Start with the [README](README.md), [implementation status](docs/IMPLEMENTATION_STATUS.md) and the relevant subsystem's contract documentation.

For substantial architecture changes or new dependencies, explain the intended behavior and tradeoffs in an issue or draft pull request before investing in a broad implementation. Research and roadmap entries are not promises that an interface is ready to extend.

## Set up and validate

The smallest development loop runs on Linux or WSL without graphics or dependency downloads:

```sh
cmake --preset headless
cmake --build --preset headless
ctest --preset headless
```

Use `runtime-headless` for simulation changes. Windows graphics and desktop work uses additional pinned toolchains and dependencies; follow [BUILD.md](docs/BUILD.md) and [DESKTOP_EDITOR.md](docs/DESKTOP_EDITOR.md). Keep build artifacts under `build/` and downloaded tools under the existing `.cache/` workflow. Do not require a machine-specific absolute path in source or examples.

CTest covers the configured native and CLI contracts. GPU captures, Windows window lifecycle tests and desktop integration harnesses are separate, explicitly invoked checks. Run the checks relevant to your change and report the exact configuration and results. Do not describe a headless test, synthetic input or scripted GUI action as physical interaction or graphics qualification.

## Preserve the shared-service model

Authoring behavior belongs in the native world service so people using the editor, people using the CLI and external agents share validation, persistence and undo behavior. Develop human workflows and agent workflows together when a change affects both. Keep UI state such as unfinished Inspector drafts separate from committed world state.

- Validate complete changes before publication. Preserve state on invalid input, stale revisions and storage failure.
- Keep schema discovery, error behavior, retry semantics and documentation consistent when changing an API.
- Preserve stable entity identity, the distinction between authored and runtime state, and immutable presentation snapshots.
- Preserve build boundaries: headless configurations must not restore desktop dependencies, create a window or require a graphics driver. Keep audio and managed-gameplay integrations selectable. Keep desktop builds supported alongside headless configurations.
- Bound resource use and expose useful diagnostics. Performance claims need a reproducible workload and a stated measurement environment.

Follow the surrounding C++20 or C# style. Prefer a small, reviewable change over unrelated cleanup. Add regression tests for changed contracts or failure behavior; use an existing fixture where it meaningfully exercises the new behavior. Do not introduce tests that merely repeat implementation details.

## Submit a reviewable change

A pull request should state the problem, the resulting behavior, relevant limitations and how it was validated. Include a minimal reproducer for bug fixes. For editor or rendering changes, provide an actual capture from the modified application when possible; label mockups and untested behavior clearly.

Keep secrets, personal project content, downloaded SDK archives, local logs and build outputs out of commits. Use synthetic or redistributable test assets. Check screenshots and recorded evidence for private paths or content before adding them to public documentation.

New dependencies need a reason, a pinned source/version, integrity verification where the existing build supports it, and preserved notices. Update [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and package notice collection when relevant. Do not assume an asset or SDK may be redistributed merely because it can be downloaded.

When reporting a problem, include the engine version or commit, OS, build preset/options, reproduction steps and expected versus actual behavior. For graphics issues, also include the GPU/driver, selected Vulkan device and relevant diagnostics. Redact private paths and credentials. There is no required AI tool: contributions made with development agents should receive the same code review, licensing checks and verification as other contributions.
