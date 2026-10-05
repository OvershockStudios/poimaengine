# Vulkan compute skinning

Poima 0.0.18 adds a compute skinning pass to the shared scene renderer. It consumes immutable mesh vertices/influences and a separate palette per instance, then writes the existing 48-byte vertex format. Opaque materials, normal maps and every shadow view use that same output. Asset pose capture exercises this path; 0.0.21 also supplies palettes from [ordinary editable rigs and native runtime playback](RUNTIME_ANIMATION.md). Blending remains unfinished.

## Agent controls

`asset.animation.capture` now defaults to `skinning:"gpu"`. Select `skinning:"cpu"` for the double-precision reference calculation. Both modes use the same authored camera, lighting, model and sampled clip time. The result identifies `source:"asset_animation"` and `animation.skinning` as `gpu_compute` or `cpu_reference`. No fallback changes the requested mode silently. The [animation asset contract](ANIMATION_ASSETS.md) supplies the other parameters. `world.describe` schema revision 19 advertises the selector; `gpu_skinning` capability follows renderer availability.

`render_diagnostics.last_draws` adds `skinned_instances` and `skinned_vertices`: the number of compute dispatches and vertices dispatched in the last submission. An instance visible to multiple shadow views is deformed once. An instance culled from the camera still runs if a shadow view needs it. An instance rejected by all views skips deformation. With profiling enabled, `gpu.skinning` reports its own interval; `gpu.total` includes skinning, shadows, opaque drawing and resolve/readback intervals. These are serialized diagnostic timings, not game frame-time qualification.

A numerical failure reports the instance ID and an offending vertex index through the normal capture error (`-32020`). The error record is read after GPU completion; a failed capture does not publish an image file. The reported vertex is whichever failing thread wins the atomic record, so it is not a deterministic choice when several vertices fail. Inspect the same clip/time and vertex page with `asset.animation.sample` to investigate. CPU evaluation can represent some numbers that fail in GPU float arithmetic.

## Data flow and ownership

1. Upload source geometry and a separate 32-byte influence record once per mesh. Static vertices retain their existing layout and static meshes need no influence buffer.
2. Cache vertex-position bounds for each joint's positive-weight influences. Sampled palettes and instance identities arrive in immutable `SceneSnapshot` data.
3. Transform those joint bounds by the current palette, conservatively union them, then apply the instance world transform for camera/shadow culling. Padding accounts for accepted weight-sum error and float rounding. The per-frame bound evaluation depends on joint count rather than vertex count.
4. Upload each needed instance's palette. Dispatch groups of 64 vertices on the graphics queue before shadow drawing. The shader blends affine matrix rows, transforms positions, computes inverse-transpose normals and orthogonalized tangents, and accounts for reflected handedness.
5. Use the output buffer for all raster passes. NVRHI tracks the UAV-to-vertex-buffer and error-readback transitions. A small atomic status record rejects singular blends, nonfinite directions and out-of-range positions.

Independent instances share immutable source vertices and influences but own distinct palettes and output buffers. Their output buffers persist in the renderer context and are reclaimed when the instance disappears. Source assets remain cached for the context lifetime, consistent with the current frozen-runtime asset policy. The initial aggregate instance output/palette budget is 128 MiB. Asset preview retains its existing million-vertex/three-million-index bound.

The native `SceneObject::skin` owns an immutable `SkinPose` with mesh-local affine palettes. Consumers must supply a weighted mesh, a complete palette and unique instance identity. Authored `AnimationRig`/`RigNode`/`SkinnedMesh` components now produce these bindings in runtime snapshots; ordinary authored capture uses the rest baseline. [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) supplies the existing Vulkan resource/compute interfaces; no additional middleware or vendor-only graphics API is required. The pinned NVRHI revision remains unchanged.

## Qualification and limits

`tests/animation_native.cpp` tests joint bounds against 100,000 independently calculated float vertex blends, including general affine transforms, scale and shear. Additional palette-rounding cancellation and 1,200 four-joint stress cases cover large coordinate/coefficient ranges with both fused and separately rounded float arithmetic. `tests/gpu_skinning_capture.py` compares real images against CPU reference deformation and checks:

- Plain and normal-mapped meshes, nonuniform joint scale and rotation.
- Two poses sharing source geometry while retaining separate output buffers.
- Camera-culling equivalence and an off-camera skinned caster producing a visible shadow.
- A singular blend failing without an image, followed by a valid capture succeeding.
- Source-independent rendering and unchanged authored state.

`tests/animation_capture.py` retains the independently baked geometry comparison. The committed [GPU evidence](evidence/m2-gpu-skinning.json) records the actual executed hardware, binary/source hashes and comparison results.

This is a rendering foundation. Native fixed-tick playback and runtime ownership are documented separately in [runtime animation](RUNTIME_ANIMATION.md). Root motion, retargeting, IK, compressed sampling, motion vectors and previous-frame skinning remain unimplemented. The renderer still serializes frames and waits for GPU completion; the status check uses that existing wait. No crowd throughput, frame-latency, animation streaming or production memory target is established. Native Linux graphics and console graphics remain unqualified.
