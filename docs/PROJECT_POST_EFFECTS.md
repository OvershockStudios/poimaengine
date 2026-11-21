# Project post effects

Poima’s offline post-effect compiler accepts a project-owned HLSL color effect
and emits a versioned `.poimapost` artifact with its SPIR-V and source identity.
Import that artifact into a world and select it by hash. Vulkan capture and
playback can use the compiled effect without the shader source or DXC.

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

## Add the effect to a world

Call `asset.post-effect.import` through the world service with `source` set to
the compiled `.poimapost` path and `expected_sha256` set to the compiler’s
`artifact_sha256`. The reply’s `asset` is the ID to author. The service
checks the exact artifact bytes and publishes them into the world’s asset
store. Import a new artifact after a shader change. Imports leave the world
revision unchanged.

Use `world.transact` at the current revision to replace the scene’s complete
`LightingEnvironment` component. Add `"post_effect": "<asset>"` to its value.
Read back the selection with `world.lighting`. `world.dependencies` and the
project export inventory include the selected `.poimapost` by its exact hash.
Omit `post_effect` to disable it. A fresh load requires the selected file and
matching hash. An already running simulation owns the validated bytes it
loaded. Restart the scene preview or player to apply a changed selection.

Headless tools can author and verify package dependencies. Rendering requires
the Vulkan renderer. The installed runtime must advertise
`features.project_post_effects` for `project build` to export affected worlds.

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

The slot acts on resolved, unexposed linear radiance after world reconstruction
and viewmodel composition, before bloom, exposure and tone mapping. Bloom
uses the effect’s output as its input. HUD and overlays draw after tone
mapping, and diagnostic views bypass the effect. The target has the same
extent as the selected HDR input and a 128 MiB payload cap per renderer
context. UVs are relative to that input texture.

The vignette attenuates radiance toward the corners. For a curve over
exposed HDR, use the authored [output grade](#grade-exposed-scene-color).
Effects such as depth of field and motion blur need declared depth, motion
or neighboring samples and their own render-stage contract.
[Scene HDR composition](HDR_COMPOSITION.md)

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
The native reader repeats the artifact, provenance and instruction-profile
checks. The renderer runs Khronos SPIRV-Tools validation for Vulkan 1.3
before creating a shader module. Use trusted project sources. Bounded
instruction admission and semantic validation leave GPU driver isolation and
whole-scene performance qualification to separate controls.
[SPIR-V specification](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html)

## Grade exposed scene color

Set `LightingEnvironment.output_grade` in a guarded `component.set` operation.
Provide `input_max` in [0.125, 64], `strength` in [0, 1], and exactly 33
`knots`. Each knot contains three finite RGB values in [0, 16]. The
first knot must be [0, 0, 0] so black stays black. Any order is valid for
the remaining values.

The renderer applies the grade after bloom and exposure, before display tone
mapping. Each channel samples its own curve at its exposed intensity. Knot
indices 0 through 32 span `0` through `input_max` uniformly. Values between
knots interpolate linearly. Above `input_max`, each curve continues from its
last knot with unit slope. Strength blends the original exposed value and
the graded value. HUD rendering follows this display transform.

The following Python example creates a grade with a slight warm tint.

```python
grade = {
    "input_max": 4,
    "strength": 0.6,
    "knots": [[4 * i / 32 * gain for gain in [1.06, 1.0, 0.9]]
              for i in range(33)],
}
```

`world.lighting`, `runtime.lighting` and capture lighting reports return the
selected grade, including an explicit strength of zero. Zero gives an identity
transform. Omit the field to remove the selection. A running session keeps its
frozen grade until replaced.

The grade travels in the authored world document. Headless tools can validate
it and report the `needs_output_grade` content requirement in
`world.dependencies`, project inspection and verified bundle inspection.
Export and bundle reopening check for a runtime with
`features.output_grade: true` whenever the world selects the grade.

## Verify compilation

```sh
python3 tests/project_post_effect_contract.py -v
```

The test runs the pinned compiler and checks relocation, changed include
identity, source and resource scope, portable diagnostics, and publication
without overwrite. Run `tests/world_post_effect_contract.py` with the native
CLI and compiled artifact to verify authoring, dependency closure and restart
behavior.

On a Windows graphics host, the native image fixture compares a compiled
vignette against an independent linear-color reference. It checks the order
with bloom, the exact restored baseline, optional native UI, and a relocated
export. Pass the runtime installed from the same build as the executable.

```sh
python3 tests/project_post_effect_capture.py build/installed/bin/poima.exe \
    --artifact build/vignette.poimapost --output build/post-effect-captures \
    --runtime build/installed --check-ui --windows-interop
```

The command uses WSL path translation. Omit `--windows-interop` when running
Python directly on Windows. [Windows fixture results](evidence/m2-project-post-effect-350.json)

The headless suite validates grade authoring and capability discovery.

```sh
ctest --preset headless -R '^output_grade_authoring_contract$' --output-on-failure
```

On Windows, the grade fixture compares presented pixels with an independent
RGB reference. It checks the grade with bloom and the compiled vignette,
reconstruction, the runtime HUD, and a standalone export.

```sh
python3 tests/output_grade_capture.py build/installed/bin/poima.exe \
    --output build/output-grade-captures --artifact build/vignette.poimapost \
    --runtime build/installed --check-ui --reconstruction fsr3_quality --windows-interop
```
