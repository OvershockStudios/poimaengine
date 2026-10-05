# Build and use Poima

Poima is an early engine prototype with a native CLI, persistent world editing, shared local sessions, asset import, project packaging and optional simulation, Vulkan rendering and desktop editing. Animation, materials, physics and audio each have a bounded implemented subset. These features are not a production-complete engine; consult [implementation status](IMPLEMENTATION_STATUS.md) for current behavior and limitations.

Start with the dependency-free headless build below. Add the [fixed-step runtime](RUNTIME.md), [Vulkan scene renderer](SCENE_CAPTURE.md), [C# gameplay host](MANAGED_GAMEPLAY.md), [audio backend](AUDIO.md) or [desktop editor](DESKTOP_EDITOR.md) only when needed. [Native module](NATIVE_MODULE_LAB.md) and [C# gameplay](MANAGED_GAMEPLAY_LAB.md) experiments retain their separate measurements; they are not shipping performance guarantees.

## Requirements

Run commands from your repository checkout. Build outputs go under `build/`; optional downloaded tools go under `.cache/`. No particular checkout path or developer account is required. Historical evidence describes the machine and filesystem used for each measurement, not a general build-time promise.

- CMake 3.24+, Ninja and a C++20 compiler and C99 support for MikkTSpace.
- Python 3.9+ for the black-box tests, using only the standard library and repository fixtures. Disable tests with `-DPOIMA_BUILD_TESTS=OFF` if unavailable. Optional bootstrap and packaging scripts have their own requirements below; the native engine does not invoke Python.
- The `dev` preset fetches a hash-verified, commit-pinned Vulkan-Headers archive on first configuration. No Vulkan SDK installation or loader is needed to compile. A system loader/driver is needed only when requesting graphics inspection.
- The `headless` preset disables graphics inspection, does not fetch dependencies and does not require a graphics library, display or GPU. The JSON parser, cgltf static-model importer and stb_image PNG/JPEG decoder are bundled under MIT.

The presets use two build workers to limit memory pressure. Dependencies and their notices are recorded in [third-party notices](../THIRD_PARTY_NOTICES.md). The independent `poima-module-lab` experiment is built by default and can be disabled with `POIMA_BUILD_MODULE_LAB=OFF`; its [commands, tests and limits](NATIVE_MODULE_LAB.md) are separate from the main CLI. Linux CTest includes its bounded correctness fixture. The lab is not installed as a production engine tool.

## Linux / WSL

From the repository root:

```sh
cmake --preset headless
cmake --build --preset headless
ctest --preset headless
./build/headless/poima capabilities
./build/headless/poima doctor
```

This route requires no display, graphics driver, .NET SDK, optional middleware or downloaded engine dependencies. Configure with `-DPOIMA_BUILD_TESTS=OFF` for a C/C++/CMake/Ninja-only build. Use `dev` instead of `headless` to add Vulkan device inspection, then run `./build/dev/poima doctor --graphics`; that preset downloads the pinned Vulkan headers.

The dependency-free headless checks do not qualify simulation-enabled builds, Windows graphics drivers, either editor, or binary distribution. Optional GPU/GUI suites require a suitable machine and are run separately. Hosted CI is not currently configured in this public snapshot.

## Optional fixed-step runtime

Use `runtime-headless` to build the EnTT/Jolt simulation without graphics, or `windows-runtime` for simulation plus the native Windows Vulkan preview. These presets enable `POIMA_ENABLE_SIMULATION` and fetch pinned Jolt/EnTT sources on first configuration. They keep two build workers and workspace-local caches. The original `headless` and `windows-render` presets retain simulation-disabled defaults. [Runtime commands, components and limits](RUNTIME.md). The Windows runtime also includes the [continuous player and replay operation](PLAYER.md). To enable C# in either runtime preset, explicitly configure `POIMA_ENABLE_MANAGED_GAMEPLAY=ON` and `POIMA_DOTNET_HOST_HEADERS`, build the managed bridge/game projects, and select an existing platform-matching hostfxr at load time. Follow the [complete managed build instructions](MANAGED_GAMEPLAY.md#build); managed gameplay is off by default.

## Optional native audio

Run `python3 scripts/bootstrap_tools.py --only audio`, then configure either runtime preset with `-DPOIMA_ENABLE_AUDIO=ON`. This pins Steam Audio 4.8.1. The [audio guide](AUDIO.md) covers SDK/library installation, authoring and the sample. WAV import and audio component editing also work in the ordinary dependency-free headless build; propagation/capture requires the optional backend. Device playback remains unfinished.

## Native Windows executable from Linux / WSL

This tested route uses workspace-local LLVM-MinGW and DXC host tools. It cross-compiles an x64 Windows executable which runs on Windows and uses Windows GPU drivers. It is not a Windows-hosted build or a WSL Vulkan renderer.

```sh
python3 scripts/bootstrap_tools.py
cmake --preset windows-render
cmake --build --preset windows-render
./build/windows-render/poima.exe doctor --require-hardware
python3 tests/render_capture.py build/windows-render/poima.exe --windows-interop --gpu 0 --output build/render-evidence
```

The bootstrap script requires Linux x64 and Python with tarfile's `data` extraction filter (Python 3.12+ recommended). It downloads verified official archives into `.cache`, without modifying system packages or PATH. CMake fetches pinned Vulkan-Headers, NVRHI and SDL3 sources. An accessible Windows desktop and WSL executable interoperability are required for the final two commands; use Windows directly if interop is unavailable. The capture test converts its Linux output path to a Windows path automatically.

The default 120-frame smoke test opens a window and prefers a discrete GPU. `--gpu` selects the Vulkan enumeration index; inspect `doctor --graphics` first. `--capture` saves the final rendered image to an explicit BMP path; its parent must exist, and an existing file is overwritten. The shader is embedded in the executable, so there is no runtime source/shader working-directory dependency. For direct invocation on Windows:

```powershell
.\poima.exe render-smoke --frames 120 --gpu 0 --capture frame.bmp
```

This smoke test is deliberately serialized and uses FIFO presentation. It does not qualify game frame times or device-loss recovery. Use the [scene capture](SCENE_CAPTURE.md) and [player](PLAYER.md) workflows for authored geometry and simulation. Software Vulkan devices require `--allow-software`. NVRHI validation is enabled; this is not a claim that Khronos Vulkan validation layers were enabled.

## Windows-hosted compilation

An x64 Visual Studio Developer PowerShell with the C++ workload, CMake, Ninja and test Python is the intended native Windows-host route. Start with the `headless` configure/build/test preset commands above, then:

```powershell
.\build\headless\poima.exe capabilities
.\build\headless\poima.exe doctor
```

The native Windows executable is tested through the cross-build route above. Compilation hosted on Windows with MSVC remains unverified. To build graphics with that toolchain, use `dev` with `-DPOIMA_BUILD_RENDER_SMOKE=ON` and supply a Windows-host DXC executable through `POIMA_DXC`; the Linux bootstrap script does not provide that tool. The `windows-render` and `windows-runtime` presets specifically select the Linux-host cross toolchain, so they are not native Windows-host presets.

## Implemented commands

| Command | Result |
| --- | --- |
| `poima help` / `poima --help` / no arguments | Discover commands, arguments and exit codes. |
| `poima version` / `poima --version` | Version, compiler, compiler version and compilation target. |
| `poima capabilities` | Implemented command list and feature flags. Does not load a graphics driver. |
| `poima schema <command>` | JSON Schema for the named operation's request properties. These describe operation arguments, not a JSON-input transport. |
| `poima doctor` | Native host inspection. Does not load a graphics driver. |
| `poima doctor --graphics` | Also enumerate Vulkan devices and queue/API properties. |
| `poima doctor --require-hardware` | Also enumerate devices; require a discrete/integrated Vulkan 1.3+ device with a graphics queue. |
| `poima world <path>` | Persistent JSON-RPC authoring session; see [world service](WORLD_SERVICE.md). Parent directory must exist. |
| `poima serve <path> --endpoint <name>` | Shared headless world host; see [local sessions](SHARED_SESSIONS.md). |
| `poima connect <name> [--timeout-ms N]` | NDJSON client for a headless host or live editor. |
| `poima editor <path> [--endpoint <name>] [options]` | Optional native editor, with local client attachment when an endpoint is supplied. |
| `poima project ...` / `poima game ...` | Portable project validation/export and native game launch; see [projects and game bundles](PROJECTS.md). |
| `poima render-smoke [options]` | Optional bounded Vulkan presentation/capture test. Discover limits and defaults with `schema render-smoke`. |

Every one-shot invocation emits one compact JSON document to stdout, including invalid requests. The version-1 response contains `protocol_version`, `request_id`, `command`, `status`, `result` and `diagnostics`. `request_id` is null for these one-shot commands. The `world` command instead accepts newline-delimited JSON-RPC 2.0 requests and returns one response per request; notifications have no response. Startup failures use the one-shot error envelope and exit 4. Driver logs and unexpected internal-failure details go to stderr.

Exit codes: 0 means the request completed; 2 means invalid arguments; 3 means a required capability is unavailable (including an unbuilt render test); 4 means an execution/internal failure. Ordinary graphics inspection may return 0 with `unavailable` or `not_built`: read the nested status. Use `--require-hardware` when a hardware precondition should fail the command.

Enumeration does not create a logical device, render an image, test presentation, verify NVRHI requirements or qualify frame times. CPU Vulkan devices such as llvmpipe are explicitly labeled as software and cannot satisfy the hardware requirement. Virtual/unknown device types are conservatively excluded from that requirement.

Host memory is the physical memory reported to the current OS/VM, not a process or container budget. A zero CPU/memory value means the OS query did not supply it. WSL results describe WSL; they do not infer the Windows GPU path.

## Testing and local installation

The optional [retained desktop editor](DESKTOP_EDITOR.md) is built with `python3 scripts/build_desktop.py` and opened through `launch-editor.cmd`. It uses C#/Avalonia for controls and a native Vulkan child window for Scene. The older [ImGui editor](EDITOR.md) can be built with `-DPOIMA_BUILD_EDITOR=ON` and opened through `launch-imgui-editor.cmd`. Both are optional and absent from dependency-free headless builds. For an ImGui-enabled executable, add a fourth feature flag to the CLI test: `python tests/cli_contract.py build/windows-runtime/poima.exe 1 1 1`. GUI/GPU integration tests are separate from CTest.

The contract suite runs the compiled executable from an unrelated directory, checks discovery/schemas, malformed requests and JSON escaping, and forces a missing Vulkan manifest through child-process-only environment overrides. It verifies that graphics failure does not break headless discovery. CTest never opens a rendering window. The Windows cross preset disables CTest: run the contract suite with Windows Python so temporary paths and driver environment overrides use Windows semantics:

```powershell
python tests/cli_contract.py build/windows-render/poima.exe 1 1
python tests/world_contract.py build/windows-render/poima.exe
python tests/scene_capture.py build/windows-render/poima.exe --gpu 0 --output build/scene-evidence
python tests/render_failures.py build/windows-render/poima.exe
python tests/render_capture.py build/windows-render/poima.exe --gpu 0 --output build/render-evidence
```

`render_capture.py` explicitly opens a window, checks 12 presentations and GPU readback, parses the BMP with the standard library, verifies colored triangle coverage, and writes JSON evidence. `render_failures.py` checks unavailable GPU selection, missing driver and failed capture output. These are integration correctness tests, not performance benchmarks.

To create a local installation with notices:

```sh
cmake --install build/dev --prefix build/install
./build/install/bin/poima version
```

Substitute `build/windows-render` and a separate install prefix for the Windows executable; this includes NVRHI, SDL and compiler runtime notices. The build defaults do not install into system directories or modify agent configuration. Fetched sources remain under each build's `_deps`; after initial downloads, ordinary rebuilds do not need network access. Workspace tools remain under `.cache/toolchains`.

Static glTF import is compiled into all current builds. No additional download is required; cgltf is vendored with its MIT notice. Vulkan builds add indexed geometry and PBR material factors. [Asset workflow and qualification](ASSETS.md).

## Textured model example and verification

See [ASSETS.md](ASSETS.md) for the supported glTF profile and `examples/textured-grid.jsonl`. Headless CTest includes native mip filtering and importer/package checks. With the Windows runtime built, run from WSL:

```sh
python3 tests/texture_capture.py build/windows-runtime/poima.exe --windows-interop --gpu 0 --output build/texture-evidence
```

The test records scalar/GPU comparisons and a 120-tick continuous textured replay. Change the GPU index to test another available adapter. Its files are written under a fresh run directory; Python remains test-only.

Normal mapping and image override verification uses `tests/material_capture.py` with the same arguments as `texture_capture.py`. [Material authoring contract](MATERIAL_AUTHORING.md).
