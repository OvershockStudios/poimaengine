# Executable render schedule

The renderer builds and validates a typed schedule before recording each frame. The schedule drives the existing rendering operations in a fixed order; it is not a second description maintained separately from execution.

The portable builder lives in [render_schedule.hpp](../include/poima/render_schedule.hpp). It uses resource descriptions and identities without depending on Vulkan or NVRHI. The renderer supplies descriptions from its actual buffers, textures, draw inputs and selected frame slot after preparation. The swapchain image is bound after acquisition, then the renderer checks the actual framebuffer and helper bindings against the validated schedule before recording commands.

## Passes

The fixed order is skinning, light assignment, shadows, scene clear, sky, opaque geometry, MSAA resolve, display output, game UI, hosted overlay, legacy editor composition, capture and presentation export. Settings determine which optional passes and resource accesses are present. Stage boundaries can remain even when no corresponding work is needed.

Existing helpers still record their draws and compute dispatches. Resource IDs directly select scheduled uploads, clears, resolve targets, capture copies and presentation transitions. Before execution, binding checks connect helper resources to the same identities described by the schedule.

Each executed schedule stage records a CPU scope named `render.pass.<stage>`, such as `render.pass.opaque` or `render.pass.light_assignment`. These measure CPU command-recording work, not GPU execution. Existing GPU timestamp intervals and delayed submission attribution remain separate; see [Profiler](PROFILER.md).

## Resource contracts

The schedule distinguishes buffers from textures and records supported uses, resource identity, initialization, dimensions, sample count and ownership. Ownership categories cover immutable imported data, shared GPU resources, frame-slot readbacks, capture storage and the acquired swapchain image.

Validation checks include:

- Required resource roles, unique registered identities, permitted uses and fixed pass order.
- Producers before reads or partial writes. A blended or clipped raster pass cannot claim to initialize an entire attachment.
- Buffer used-byte bounds separately from physical allocation size. Light tables and dynamic UI uploads can initialize only a used prefix.
- Cluster-member storage paired with initialized count storage. Only the entries selected by each count are meaningful; unused list capacity is not declared initialized data. The existing overflow path evaluates the complete light table.
- Diagnostic copy sizes and ownership of the selected frame-slot readback. Readback storage is not a GPU shading input.
- Attachment dimensions and sample counts, resolve source/destination compatibility, and capture before presentation export.

The zero-shadow case retains the clear of its fallback depth texture. Skin deformation resources are described for dispatched instances rather than treating every allocated skin buffer as initialized. When clustered assignment is inactive, its bound fallback buffers are not declared as logical shader reads.

Game UI textures uploaded during preparation enter the schedule as initialized imported resources. Dynamic vertex/index uploads describe their used prefixes. Game UI and overlay composition preserve their dependency on existing attachment contents. The legacy editor's Scene image and acquired swapchain image remain distinct resources.

These checks validate the declared contract and its renderer bindings. They do not inspect arbitrary shader memory accesses or replace runtime validation of shader outputs.

## Synchronization boundary

NVRHI continues to manage resource-state transitions and Vulkan barriers. The renderer retains its explicit cross-submission memory dependency for shared GPU scratch, bounded frame-slot retirement and presentation lifetime handling. The schedule does not infer parallel execution, automatically alias transient allocations or remove synchronization because two resources have different names.

This change preserves the current forward renderer. It does not implement a deferred renderer, GI, automatic render-graph optimization or a performance improvement by itself. CPU validation and pass instrumentation also have costs.

## Reproduce checks

From a configured build with tests enabled:

```sh
cmake --build build --target poima-render-schedule-test
ctest --test-dir build -R '^render_schedule_native$' --output-on-failure
```

The standalone native target exercises the portable builder without creating a GPU device. Its fixtures cover valid schedules and rejected ownership, initialization, usage, attachment and ordering combinations.

For before/after renderer comparison, provide preserved reference and candidate **frame-execution fixture executables**, not the main engine executable:

```sh
python tests/render_schedule_capture.py REFERENCE_FIXTURE CANDIDATE_FIXTURE \
  --gpu 0 --output build/render-schedule-comparison
```

Repeat with the other GPU index where available. When running the Python harness under WSL against Windows fixture executables, add `--windows-interop`. The harness requires matching actual devices, immutable input hashes and exact captured-image agreement. Capture observations are renderer readbacks, not physical-input or editor usability tests.

## Qualification status

The [qualification record](evidence/m2-render-schedule.json) binds the tested sources, binaries and raw reports by SHA-256. Linux and Windows native tests passed 204 valid schedule variants and 250 rejected contract mutations per run.

On AMD Radeon(TM) Graphics and NVIDIA GeForce RTX 4070 Laptop GPU, the reference/candidate comparison passed 16 exact image comparisons per device across submission limits of one and two. The triangle path also passed with Khronos synchronization validation enabled. Final checks passed ten renderer regression groups, eight native UI/player/hosted integration groups, and four legacy editor layout groups covering both devices at 1× and 4× MSAA. The layout checks exercise scripted docking, floating and hidden Scene views, reset, persistence and sampled Scene compositor color agreement.

These are bounded correctness observations. GPU images are renderer readbacks; the checks do not qualify physical input, Avalonia editor usability, Linux rendering or broad hardware compatibility. They establish no performance improvement.

Single-sample scenes also clear and write a world-space normal/validity attachment. Diagnostic output samples it and scene depth; the sky uses a color-only framebuffer to preserve invalid background normals. The schedule checks product formats, required writes/reads and attachment-role aliasing. See [scene products](SCENE_PRODUCTS.md) for the output contract and qualification status.
