# Ordered draw instancing

Compatible adjacent objects can share one Vulkan geometry draw command while
retaining independent transforms and material values. This guide covers
CPU-planned submission. [GPU camera visibility](GPU_VISIBILITY.md) uses the
same compatibility rules for an optional indexed indirect path.

## Controls and observations

`draw_instancing` is an optional strict Boolean, default `true`, on
`world.capture`, `runtime.capture`, `asset.animation.capture`, `runtime.play`
and `player.start`. Set it to `false` for the direct reference path. The native
C++ `RenderOptions` exposes the same selection; no standalone game CLI switch
is added. Player settings retain this non-preference option.

```json
{"jsonrpc":"2.0","id":1,"method":"world.capture","params":{"revision":1,"camera":"00000000000000000000000000000001","path":"build/instanced.bmp","draw_instancing":true}}
```

Schema revision 117 advertises the control. The graphics build advertises
`draw_instancing` in capability discovery; a headless build can validate requests
but cannot render them. Invalid types fail with `-32602` before rendering.
The option is observation configuration, not authored or saved game state.
Existing revision, session, tick and destination guards still apply.

`render_diagnostics.draw_instancing` echoes the selection. Completed
`last_draws.camera_batches` and `last_draws.shadow_batches` count actual geometry
draw commands. Existing `camera_draws`, `shadow_draws` and triangle counts retain
their object/view-instance meaning. Foreground diagnostics expose their own
`draws.camera_batches`; foreground commands are separate from world-camera
commands. Counts belong to retired submissions, rather than inferred plans or
presented-frame totals. See [render diagnostics](RENDER_DIAGNOSTICS.md).

## Eligibility and ownership

A batch contains only immediately adjacent visible objects with consecutive
GPU object indices and the exact same vertex, index, previous-vertex and material
binding resources, element count and selected graphics pipeline. Planning never
sorts objects or bridges a culled slot. Different per-object transform, color,
roughness and other scalar material values remain in the existing object buffer.
Independently authored texture overrides share a material binding when all five
maps, their samplers and mip views, and the four optional surface layer images
resolve to the same resources. Scalar overrides such as normal strength and
layer weight remain per object. Use the same cooked texture asset across repeated
objects, and set the filter and wrap modes to suit the material.

Camera, each shadow view and first-person foreground have separate plans.
The shadow planner builds all layer plans in one pass over the source objects,
visiting only the layers in each caster's visibility mask. A missing caster in
a layer still breaks adjacency in that layer. Casters that share a layer also
need the same first index and index count in their submitted ranges.
Masked shadows share bindings when the base color texture, mip view and sampler
match. Normal maps, other camera-only textures and per-object alpha cutoffs can
vary within a masked shadow batch. Opaque shadows use the shared depth-only
binding. Skinning plans use the final current/history buffer handles selected
for that submission. Independently skinned objects have separate geometry
streams.

Every native draw uses `firstInstance: 0`; `SV_InstanceID` offsets the pushed
base object index. A flat resolved index carries the object's identity through
fragment material and alpha tests, including masked shadows. Object data uses
272 bytes and draw push constants use 16 bytes. Existing per-object render
schedule resource declarations, GPU slot ownership and completion fences remain
in use. The implementation follows conventional [Vulkan indexed/instanced
drawing](https://docs.vulkan.org/spec/latest/chapters/drawing.html) and
[instance-index semantics](https://docs.vulkan.org/refpages/latest/refpages/source/InstanceIndex.html).

The prototype C++ options/diagnostic structures gain fields and native C++
consumers must be rebuilt together. This checkpoint does not change the C# host
services table, gameplay save encoding or cooked-model format. It does not make
a general promise of prebuilt C++ binary compatibility.

## Qualification

[Shared material binding evidence](evidence/m2-material-sharing-472.json) compares
0.0.471 with 0.0.472 in a dense 1,024-plant fixture. The plants share geometry
but have individually authored texture overrides, with controls for color
textures, filters and normal maps. Both AMD forward and NVIDIA deferred
render 1,025 camera objects and 3,072 shadow caster/view pairs. Camera commands
fall from 1,025 to 5 and shadow commands from 3,072 to 9. Each adapter's BMP
matches both its previous checkpoint and the current direct render byte for
byte. The evidence includes frozen-scene timestamps and retained regression
checks. Across two 24-frame runs per version on each adapter, the median of the
mean CPU command-recording time fell from 1.75 to 0.56 ms on AMD and from 1.89
to 0.52 ms on NVIDIA. The camera and simulation state were fixed during
these measurements.

[Checkpoint evidence](evidence/m2-render-instancing.json) records native 0.0.211
builds, six portable planner groups with 1,969,893 checks on Linux and Windows,
and strict Boolean discovery/admission. Eleven selected Linux suites pass across
the initial and corrected-schema records; the unchanged compiled weapon workflow
also preserves all 33 fields through rollback and cold save continuation.
Seven Windows native executables pass separately.

AMD Radeon integrated graphics and the NVIDIA RTX 4070 Laptop GPU each pass
32 new captures and 16 exact complete-RGB/available-probe pairs, including
indexed and built-in nonindexed geometry, per-object materials, cutout shadows,
rigid/weighted motion and separate foreground. The dense case retains all 1,024
visible instances while reducing actual camera commands from 1,024 to one.
A further 450 retained alpha, reconstruction and motion captures pass: 514 total.
Accepted Windows Jobs retire empty and unforced.

Original failed test-camera setup, test-only linker configuration and invalid
empty-index fixture attempts remain failed. Their minimal test/tool corrections
and separate artifact authorities are recorded; the successful engine/core/shader
build is unchanged. A reduction in commands alone does not establish a frame-time
improvement; representative performance remains a separate measurement.

## Run the focused checks

Use a configured [native build](BUILD.md). The portable planner needs no GPU:

```sh
cmake --build build/linux-game --target poima-render-batches-test -j 2
./build/linux-game/poima-render-batches-test
```

In a configured Windows Vulkan build, compile `poima-render-instancing-test`.
From PowerShell at the checkout root:

```powershell
$out = Join-Path (Get-Location).Path 'build\instancing-render-check'
& .\build\windows-runtime\bin\poima-render-instancing-test.exe $out 0
```

The arguments are a **new absolute output directory** and a hardware GPU index;
the parent directory must exist. Use a fresh output for each adapter/run. The
fixture retains original BMPs and `evidence.json`, requires 32 captures and 16
full RGB/probe comparisons, and has an internal 180-second admission bound.
Unattended qualification additionally uses an enclosing process-tree owner;
the recorded Windows runs use a Job with unforced empty closure. An internal
clock check alone cannot terminate a stuck graphics driver.

The nonindexed controls use the renderer's shared built-in geometry. Imported
`MeshAsset` controls retain explicit triangle indices. Raw scene-product
comparisons cover requested pixel probes, rather than complete floating-point
attachments. Forward 1×/4× and deferred 1× are separate cases; foreground uses
its own 4× samples.

The shared-binding fixture uses Windows Python and requires the Vulkan
validation layer. Make the Khronos layer available to the loader before
starting the check. [Enable Vulkan validation](https://github.com/KhronosGroup/Vulkan-ValidationLayers#ways-to-enable-the-layer)

```powershell
$env:VK_INSTANCE_LAYERS = 'VK_LAYER_KHRONOS_validation'
$env:VK_LOADER_DEBUG = 'layer'
$candidate = 'C:\poima\candidate\bin\poima.exe'
$reference = 'C:\poima\0.0.471\bin\poima.exe'
python -B tests/material_binding_capture.py $candidate --reference $reference --gpu 0 --lighting-path forward --output build/material-binding-check
```

For diagnostic timing, reuse the generated scene and capture 24 identical
frames with `profile:true`, `samples:1` and the same camera and lighting path on
both executables. Disable validation-layer overhead for the timing run. The
CPU preparation metric samples the initial snapshot upload and preparation
once. Command recording is sampled every frame.
