# Camera observation cost

The native runtime exposes `Runtime::camera_state(camera)` for current camera
pose, lens and lighting values. It returns owned values without extracting mesh
objects, animation skin palettes or UI presentation. Rendering continues to use
`Runtime::snapshot(camera)` with the complete scene.

Shared native player reports use the lighter query at the same owner boundaries
as before. `player.inspect` reads the cached report; revision guards, control
receipts, preference observations and the JSON contract remain unchanged. This
removes a full scene extraction from every report reconciliation, including
frames with no incoming inspection request.

Both queries share missing-entity, Camera-component and rigid-transform checks.
The projection query validates camera and lighting independently of unrelated
mesh/skin extraction failures. Full snapshots now validate the rigid camera
transform before extracting geometry. This can change which error appears first
when both the camera and geometry are malformed.

## Reproduce the comparison

Build the retained `0.0.95` source (`a7b2efdcae59c40a9add665e4443f5e7ac8d6cc0`)
and the current checkpoint separately. Use
the same Performance Yard world, native gameplay artifact, dependencies and
shader caches for each binary. Follow the [workload preparation](../examples/performance-yard/README.md)
instructions once; reuse the resulting world and artifact rather than recooking
content between binaries. With native Windows Python, run each binary on both
GPUs in separate output directories:

```powershell
python tests/performance_yard.py --binary reference/poima.exe --world yard/world.json --descriptor gameplay/native-gameplay.json --gpu 1 --warmup 30 --duration 60 --output reference-nvidia
python tests/performance_yard.py --binary candidate/poima.exe --world yard/world.json --descriptor gameplay/native-gameplay.json --gpu 1 --warmup 30 --duration 60 --output candidate-nvidia
```

Repeat with `--gpu 0` for the integrated GPU on the qualified laptop; discover
GPU indices on another machine. The window must retain focus. The harness
records actual power and owning-monitor mode, authentic native ticks and route
progress, complete GPU retirement, raw frame rows and process memory. Native
AOT gameplay follows its autonomous route; replay and per-tick RPC do not drive
the benchmark.

Compare `host_poll_gap`, `poll_wall` and present-return cadence separately.
Host gaps include reconciliation, IPC, waiting and scheduling. The CPU wall
stages overlap and must not be added together. Present-return cadence does not
measure scanout FPS or input latency. See [frame capture semantics](FRAME_PERFORMANCE.md).

## Regression scope

Native camera tests exercise independent lenses, moving camera-attached lights,
owned observation values, checkpoint restoration and continued motion, and
missing/component/scaled-camera failures. Shared-player, live-preference and
repeated checkpoint tests exercise the report consumer with actual Windows
Vulkan windows. The 40-restoration cohort uses a small physics/UI fixture
without compiled gameplay and is separate from the native performance game.
These checks do not qualify physical input, audible output,
production geometry, VRAM residency or the thirty-minute reliability gate.

Run the report-consumer checks with native Windows Python:

```powershell
python tests/player_service_contract.py --binary build/windows-runtime/poima.exe --capture --gpu 1 --output player-contract
python tests/player_live_settings_contract.py --binary build/windows-runtime/poima.exe --capture --gpu 1 --output live-settings
python tests/player_checkpoint_cycles.py --binary build/windows-runtime/poima.exe --gpu 1 --cycles 40 --output checkpoint-cycles
```

Use new output directories. The fixtures keep their original projection, UI and
pixel assertions; no tolerances were relaxed.

## Observed 0.0.96 comparison

Each retained binary ran the unchanged workload after a 30-second warm-up,
followed by a 60-second measurement at 1080p, medium GTAO and no reconstruction
or frame generation. The laptop was on AC and its owning monitor reported
1080p at 144 Hz. Both binaries preserved the normal 60 Hz simulation clock,
route progress and complete GPU retirement.

| Device | Mean host gap, reference → candidate | Mean total GPU, reference → candidate | Present-return p95, reference → candidate |
| --- | --- | --- | --- |
| RTX 4070 Laptop | 3.440 → 3.000 ms | 2.610 → 2.555 ms | 12.746 → 12.773 ms |
| AMD integrated | 3.952 → 2.997 ms | 15.538 → 13.221 ms | 21.877 → 18.281 ms |

The candidate meets both cadence budgets on NVIDIA. AMD meets the 25 ms p99
budget (19.172 ms), while still missing the 16.7 ms p95 budget. NVIDIA's p95
is effectively unchanged. GPU durations also vary despite the unchanged
renderer, so the entire AMD cadence difference cannot be isolated to camera
extraction. These are sequential pairs on one laptop without randomized
repeats or thermal control.

[Summary and source pins](evidence/m2-camera-observation.json) retain the
reference and candidate observations. [Raw native frames and memory](evidence/camera-observation-native.json.gz)
allow independent timing and accounting checks. These short measurements do
not close the performance or reliability release gates.
