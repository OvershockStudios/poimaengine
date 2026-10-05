# Third-party notices

## Optional retained desktop frontend

`desktop/Poima.Editor/packages.lock.json` pins the Avalonia 12.1.3 and Dock 12.1.0.6 dependency graph. The Windows desktop package uses a self-contained .NET runtime, Skia/HarfBuzz and Inter 4.1. The older ImGui frontend retains Source Sans 3. These dependencies belong to the optional frontend; native headless builds do not restore them. `scripts/build_desktop.py` collects resolved package licenses, runtime/apphost notices and the font license into the desktop payload. Packages that provide only an SPDX expression are supplemented by unchanged upstream license texts pinned to their package source commits in `third_party/desktop_notices/provenance.json`. The package manifest records payload and notice hashes; this inventory is not console or clean-machine distribution qualification.

Original Poima source is licensed under Apache-2.0; see [LICENSE](LICENSE).

## JSON for Modern C++ (bundled)

- Publisher: Niels Lohmann and contributors; [source](https://github.com/nlohmann/json/releases/tag/v3.12.0).
- Release: `v3.12.0`, single header `third_party/nlohmann/json.hpp`.
- Header SHA-256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`.
- License: MIT; original `third_party/nlohmann/LICENSE.MIT` is retained and installed.
- Use: authored-world JSON documents, schema descriptions and JSON-RPC transport. No runtime dependency download.

## Vulkan-Headers (optional inspection/render build)

- Publisher: The Khronos Group Inc. and individual contributors.
- Source: https://github.com/KhronosGroup/Vulkan-Headers
- Release reference: `v1.4.352`.
- Pinned commit: `015e25c3c91b70eb1a754d36fb14c4ba6ad9b0b9`.
- Archive SHA-256: `666700236101d288dea8bb080c6f8039467cb1d29ea1e1aae6f93a819f4f9029`.
- License: Apache-2.0 OR MIT for the included Vulkan headers, as specified in their SPDX notices. The archive also contains files under the individual licenses identified by its `LICENSE.md`; no dependency helper scripts are used at runtime.
- Integration: C declarations for device inspection and C++ bindings for the rendering experiment. The system supplies its own loader/driver. No driver or SDK binary is redistributed by this bootstrap.

## NVRHI (optional render build)

- Publisher: NVIDIA Corporation; [source](https://github.com/NVIDIA-RTX/NVRHI).
- Commit: `d0c8e30d5f8d58c3b838b06aa1d8d0a912bea076`.
- Archive SHA-256: `2f62a5115ff8a11384b0d10c336b97aad73a78175addc11ca94c385eff94a452`.
- License: MIT; original `LICENSE.txt` is installed.
- Statically linked Vulkan backend and NVRHI validation layer. DirectX, NVAPI, RTXMU and Aftermath integrations are disabled.

## SDL3 (optional render build)

- Publisher: Sam Lantinga and contributors; [source](https://github.com/libsdl-org/SDL).
- Release: `3.4.16`.
- Archive SHA-256: `7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68`.
- License: zlib; original `LICENSE.txt` is installed. Individual upstream source files retain their notices.
- Statically linked window/events, Vulkan surface creation and BMP output. SDL's own renderer/GPU/audio APIs are disabled in this experiment.

## Jolt Physics (optional runtime)

- Publisher: Jorrit Rouwe and contributors; [source](https://github.com/jrouwe/JoltPhysics/tree/v5.4.0).
- Release: `5.4.0`; pinned commit `036ea7b1d717b3e713ac9d8cbd47118fb9cd5d60`.
- Archive SHA-256: `4427d6ce190e049b186bb88dbfb8692c2373a7fc042c984b5f15396194aac958`.
- License: MIT; original `LICENSE` is installed. Upstream sample applications/assets are not shipped.
- Use: native collision, rigid bodies, capsules and internal state checkpoints. Double positions, deterministic build option, SSE2 baseline, static library, no LTO/debug renderer/object-stream serialization. The build supplies the standard `<type_traits>` header for LLVM-MinGW compatibility without changing upstream source.

## EnTT (optional runtime)

- Publisher: Michele Caini and contributors; [source](https://github.com/skypjack/entt/tree/v3.16.0).
- Release: `3.16.0`; pinned commit `b4e58bdd364ad72246c123a0c28538eab3252672`.
- Archive SHA-256: `3e996cf255b09527faf995c7c36e3daf92cefa0fb37540a4b9c359af557e19cd`.
- License: MIT; original `LICENSE` is installed.
- Use: private native runtime entity/component storage. Engine identity, schema, scheduling, transactions and presentation snapshots remain Poima responsibilities.

## Workspace build tools

The optional `experiments/managed_gameplay` launcher also includes `hostfxr.h` and `coreclr_delegates.h` from a caller-supplied .NET SDK. Those files carry the .NET Foundation MIT notice; the tested headers come from the Windows host pack 10.0.8. The lab loads an existing CoreCLR runtime and does not redistribute a .NET SDK/runtime or install this launcher in the engine package. Runtime packaging will require pinning its distribution and preserving its full license/dependency notices. The initial measured SDK/runtime are 10.0.204/10.0.11; this is recorded development evidence, not a shipping dependency approval.

These tools are fetched only by the explicit bootstrap script, not by the headless preset. See `scripts/bootstrap_tools.py` for exact official download URLs and checksums. Compiler binaries are not included in the engine installation.

The optional Linux C# shipping experiment uses SDK **10.0.401** and runtime **10.0.12**. `python3 scripts/bootstrap_tools.py --only dotnet` downloads the official Linux x64 SDK into the workspace, verified with publisher SHA-512 `51c8b999af9e8dd9998c9edc5944e19a90788862068acd38694e098889054ce8c23d4f0c5cccfa16bf187d044562359e5ee69a9f8ad0bbe913ba90311fbce25b`. Its root `LICENSE.txt` is the .NET Foundation MIT license; `ThirdPartyNotices.txt` retains the additional notices. The shipping verifier copies both alongside the Native AOT experiment, which includes runtime code. NuGet resolves the compiler/runtime packs from nuget.org into a workspace-local cache; the verifier records the resolved asset-manifest hashes. This does not qualify console redistribution or replace a production dependency/license inventory. The bootstrap still defaults to the graphics tools; the SDK is opt-in.

| Tool | Pinned release and archive SHA-256 | Use and notices |
| --- | --- | --- |
| LLVM-MinGW, Linux x64 host/UCRT | `20260922`; `bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21` | Cross-compiles native Windows executables. The distribution's `LICENSE.TXT` and MinGW `COPYING*` texts are installed with builds using the supplied toolchain because compiler runtime code is linked statically. |
| Microsoft DirectX Shader Compiler, Linux x64 host | `v1.8.2505.1`, asset `linux_dxc_2025_07_14.x86_64.tar.gz`; `f2213da1fc99dc8778c8823078e16ba97c7f80f86a1d4520ab1adf4b462bc48c` | Compiles Poima HLSL into embedded SPIR-V during the build. The downloaded distribution retains `LICENSE-MS.txt` and `LICENSE-LLVM.txt`; DXC is not linked into or redistributed with the engine executable. |

CMake verifies downloaded source archive hashes and installs dependency licenses beside the executable. Explicit `FETCHCONTENT_SOURCE_DIR_*` overrides use the supplied local source directories instead; callers are responsible for their provenance. The headless preset uses the bundled JSON header and fetches no third-party sources. Other planned engine libraries are not integrated yet.

The optional main-engine integration (`POIMA_ENABLE_MANAGED_GAMEPLAY`) now uses caller-supplied `hostfxr.h` and `coreclr_delegates.h` as well. Both native builds were compiled with Linux SDK 10.0.401 host-pack headers 10.0.12, including their MIT source notices. Runtime tests load existing Windows CoreCLR 10.0.11 and Linux CoreCLR 10.0.12; the managed bridge/sample have no additional NuGet package dependencies. This integration still does not bundle a runtime. The [build guide](docs/MANAGED_GAMEPLAY.md) identifies the explicit paths and current development-only qualification.

## cgltf 1.15

- Source: https://github.com/jkuhlmann/cgltf/tree/v1.15
- Commit: `360db1a95480fe102ae9c69b27c5d101167ff5ba`.
- License: MIT; retained in `third_party/cgltf/LICENSE` and installed in `share/poima/licenses/cgltf/LICENSE`.
- The unmodified header is bundled for native static glTF import. Exact file hashes are recorded in `third_party/cgltf/PROVENANCE.json`.

## stb_image 2.30

- Source: https://github.com/nothings/stb/tree/2c980bb59875b0d32144a71867fbdebb2f77cd20
- Selected license: MIT (the upstream distribution also offers the Unlicense alternative). The unmodified header and full upstream license are retained in `third_party/stb`; `LICENSE` is installed into `share/poima/licenses/stb`.
- Native PNG/JPEG decoding only, memory input, no stdio, no SIMD-specific decoder path. Poima supplies bounded allocation callbacks and image limits. Exact hashes are in `third_party/stb/PROVENANCE.json`.

## MikkTSpace

- Source: https://github.com/mmikk/MikkTSpace/tree/3e895b49d05ea07e4c2133156cfa94369e19e409
- Copyright (C) 2011 by Morten S. Mikkelsen; zlib-style permissive terms are preserved in both source headers. The full header containing the notice is installed in `share/poima/licenses/mikktspace/mikktspace.h`.
- The unmodified C/header pair generates tangent frames during native import. Exact hashes are recorded in `third_party/mikktspace/PROVENANCE.json`. Poima's callback/reindexing integration is separate original code.

## Steam Audio (optional acoustic observation backend)

- Official SDK release: [4.8.1](https://github.com/ValveSoftware/steam-audio/releases/tag/v4.8.1), published 2026-02-11.
- Archive: `steamaudio_4.8.1.zip`, SHA-256 `4a0aa5ec1176f38f0b0993a37c2259d9e86f27e22d5e24f83ec4c3cb9a1d5449`, supplied by the release API and verified by the optional bootstrap.
- Valve source/header license: Apache-2.0, retained in `third_party/steam_audio/LICENSE.md`; original notices remain in SDK headers. The distributed binaries include additional dependencies covered by their bundled `THIRDPARTY.md`, including Intel IPP terms. This is not an assertion that all binary dependencies use Apache-2.0.
- `POIMA_ENABLE_AUDIO` links the selected native Windows/Linux x64 library. Installation includes that library plus the full license and third-party notices under `share/poima/licenses/SteamAudio`. Other SDK platforms/tools are not included in the engine installation.
- Use: native CPU direct-path propagation and HRTF output for synchronous authored/runtime audio snapshots. Device playback, reflections/pathing and console qualification remain unfinished. No proprietary Vercidium Audio code, binaries or implementation knowledge is incorporated.

## Dear ImGui (optional native editor)

- Source: [Dear ImGui v1.91.9b-docking](https://github.com/ocornut/imgui/releases/tag/v1.91.9b-docking), pinned commit `52fe0a05a7b1aa180a202bb24f0f2a049a9c1b7d`.
- Archive SHA-256: `84196b24c66cd3be22cb0b313ef70941481238ec33e595401b07976baeb7c3db`.
- License: MIT; upstream `LICENSE.txt` is installed in `share/poima/licenses/DearImGui`. Core/widgets/drawing/tables and the upstream SDL3 input backend are unmodified. Poima supplies its own NVRHI rendering integration.
- Fetched only with `POIMA_BUILD_EDITOR=ON`. This pinned static-font API baseline is an explicit integration choice, not a claim to use the newest upstream release. It does not provide the shipped-game UI/accessibility framework.

## Source Sans 3 (optional native editor)

- Source: [Adobe Source Sans 3 release 3.052R](https://github.com/adobe-fonts/source-sans/releases/tag/3.052R), commit `ed1808970eb3c7301c9a523bee26473ba0bb62fa`.
- License: SIL Open Font License 1.1; unmodified Regular and Semibold TTF files and the complete license are retained under `third_party/source_sans`. Provenance and hashes are in that directory's README. The license is installed in `share/poima/licenses/SourceSans3`.
- Fonts are embedded only in editor-enabled builds. Rasterized at runtime through Dear ImGui's font atlas; no Inter font is included.
