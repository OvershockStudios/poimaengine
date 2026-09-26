# Third-party notices

Original Poima source is licensed under Apache-2.0; see [LICENSE](LICENSE).

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

## Workspace build tools

The optional `experiments/managed_gameplay` launcher also includes `hostfxr.h` and `coreclr_delegates.h` from a caller-supplied .NET SDK. Those files carry the .NET Foundation MIT notice; the tested headers come from the Windows host pack 10.0.8. The lab loads an existing CoreCLR runtime and does not redistribute a .NET SDK/runtime or install this launcher in the engine package. Runtime packaging will require pinning its distribution and preserving its full license/dependency notices. The initial measured SDK/runtime are 10.0.204/10.0.11; this is recorded development evidence, not a shipping dependency approval.

These tools are fetched only by the explicit bootstrap script, not by the headless preset. See `scripts/bootstrap_tools.py` for exact official download URLs and checksums. Compiler binaries are not included in the engine installation.

The optional Linux C# shipping experiment uses SDK **10.0.401** and runtime **10.0.12**. `python3 scripts/bootstrap_tools.py --only dotnet` downloads the official Linux x64 SDK into the workspace, verified with publisher SHA-512 `51c8b999af9e8dd9998c9edc5944e19a90788862068acd38694e098889054ce8c23d4f0c5cccfa16bf187d044562359e5ee69a9f8ad0bbe913ba90311fbce25b`. Its root `LICENSE.txt` is the .NET Foundation MIT license; `ThirdPartyNotices.txt` retains the additional notices. The shipping verifier copies both alongside the Native AOT experiment, which includes runtime code. NuGet resolves the compiler/runtime packs from nuget.org into a workspace-local cache; the verifier records the resolved asset-manifest hashes. This does not qualify console redistribution or replace a production dependency/license inventory. The bootstrap still defaults to the graphics tools; the SDK is opt-in.

| Tool | Pinned release and archive SHA-256 | Use and notices |
| --- | --- | --- |
| LLVM-MinGW, Linux x64 host/UCRT | `20260922`; `bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21` | Cross-compiles native Windows executables. The distribution's `LICENSE.TXT` and MinGW `COPYING*` texts are installed with builds using the supplied toolchain because compiler runtime code is linked statically. |
| Microsoft DirectX Shader Compiler, Linux x64 host | `v1.8.2505.1`, asset `linux_dxc_2025_07_14.x86_64.tar.gz`; `f2213da1fc99dc8778c8823078e16ba97c7f80f86a1d4520ab1adf4b462bc48c` | Compiles Poima HLSL into embedded SPIR-V during the build. The downloaded distribution retains `LICENSE-MS.txt` and `LICENSE-LLVM.txt`; DXC is not linked into or redistributed with the engine executable. |

CMake verifies downloaded source archive hashes and installs dependency licenses beside the executable. Explicit `FETCHCONTENT_SOURCE_DIR_*` overrides use the supplied local source directories instead; callers are responsible for their provenance. The dependency-free headless preset fetches no third-party sources. Other planned engine libraries are not integrated yet.
