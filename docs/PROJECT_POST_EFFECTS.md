# Project post effects

Poima’s offline post-effect compiler accepts a project-owned HLSL color effect
and emits a versioned `.poimapost` artifact with its SPIR-V and source
identity. The current workflow ends at compilation and admission. The player,
editor and project exporter still need to consume the artifact.

## Compile the sample

Use the Linux x64 DXC already supplied by the [graphics toolchain](BUILD.md).
The compiler checks the executable and both packaged libraries against the
pinned release hashes. `--dxc-root` can select another copy of those same
files. [DXC release](https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.8.2505.1)

```sh
python3 scripts/bootstrap_tools.py --only shaders
python3 scripts/project_post_effect.py tests/fixtures/project_post_effect/effect.json \
    --output build/vignette.poimapost
```

Create the output parent first and choose a new filename for each artifact.
The compiler publishes the complete artifact only when that name is free.
The result prints the artifact, source closure and SPIR-V SHA-256 values.
Failures produce a JSON diagnostic and leave the destination absent or
preserve the existing file.

Use `--inspect` in place of `--output` to read the exact manifest and source
inventory hashes without running DXC. After reviewing the sources, pass the
returned `source_sha256` through `--expected-source-sha256` to compile the
reviewed snapshot. A changed manifest or include fails the guard before
compilation or publication.

## Author the effect

The [sample](../tests/fixtures/project_post_effect/effect.json) declares the
effect's ID, source, slot and engine input.

```json
{
  "format": "poima.project-post-effect",
  "version": 1,
  "id": "soft_vignette",
  "slot": "scene-linear-pre-exposure",
  "inputs": ["scene_hdr"],
  "source": "Vignette.hlsl"
}
```

Implement `float3 poima_post_effect(float3 hdr, float2 uv)`. The fixed
wrapper reads the current pixel from one resolved HDR texture at set 0,
binding 0. The function sees its scene-linear RGB and normalized
upper-left-origin pixel-center coordinates. The wrapper writes finite
RGB in the range 0–65,504 at color location 0.

The slot acts on resolved, unexposed linear radiance before exposure and
tone mapping. The vignette attenuates radiance toward the corners.
A film grade evaluated on exposed colors requires a different slot.
Effects such as depth of field and motion blur need declared depth, motion
or neighboring samples and their own render-stage contract. The existing
scene pipeline applies its display transform once and composes UI
afterward. [Scene HDR composition](HDR_COMPOSITION.md)

The manifest directory is the source root. Include a portable `.hlsli` path
relative to its containing shader with a quoted `#include`. The compiler
expands the includes itself and records each file’s exact hash. The source
profile uses ASCII HLSL, ordinary files, and literal include directives.
Portable paths stay below the source root. Cycles, repeated inclusions,
case collisions and file links are rejected.

## Artifact and admission

The artifact includes the authored manifest and its exact byte hash, a
sorted source inventory, the source closure hash, the generated HLSL hash,
the compiler identity and exact arguments, and the fragment interface. It
stores SPIR-V as base64 with its byte count, instruction count and SHA-256.
The same inputs on the tested host produce identical artifacts after moving
the source directory.

The compiler bounds the manifest to 8 KiB and the shader closure to 128 KiB,
16 files and eight levels. DXC runs in a temporary directory with CPU,
memory, output and wall-clock limits. Diagnostics preserve source-relative
file names and return up to 8 KiB of compiler output.

Admission checks SPIR-V 1.6 instruction framing, the fixed fragment entry
and exact resource interface. The accepted profile is one HDR fetch and
one color write, with bounded arithmetic and branching. Loops, extra
descriptor bindings, storage resources and additional stages are rejected.
The check operates on output from the pinned compiler. Use trusted local
project sources. General SPIR-V validation, driver isolation, GPU scheduling
and runtime trust policy require separate work.
[SPIR-V specification](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html)

## Verify compilation

```sh
python3 tests/project_post_effect_contract.py -v
```

The test runs the pinned compiler and checks relocation, changed include
identity, source and resource scope, portable diagnostics, and publication
without overwrite. Rendering the sample and qualifying its frame cost
require the runtime integration.
