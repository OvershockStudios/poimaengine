# Performance Yard

A reproducible small-game workload built on [Relay Yard](../relay-yard/README.md).
It retains the imported character, compiled game, navigation, UI and licensed
audio, and adds 24 total animated rigs, 768 shared box props, 64 lights and 32
dynamic bodies. Four spot shadows and the sun's cascades use eight shadow views.

This exercises actual native rendering and simulation. The geometry is simple,
the additional characters play imported Idle clips, and the boxes can settle.
Spectators can overlap the camera near circuit corners. It does not represent
production FPS art, crowd AI or sustained complex physics.

## Publish the game

Use a matching-host .NET SDK and native linker, as described in
[native gameplay](../../docs/NATIVE_GAMEPLAY.md). Select the workload module:

```powershell
python scripts/publish_native_gameplay.py `
  --project examples/performance-yard/Poima.PerformanceGame.csproj `
  --type Poima.Examples.PerformanceGame --rid win-x64 `
  --output build/performance-artifact --work build/performance-publish
```

`PerformanceGame` delegates the existing Relay gameplay and save callbacks.
Its optional controller circuit runs native navigation queries and stages
movement on the normal fixed simulation clock. The component records committed
travel, motion ticks, path plans and complete laps. It does not advance simulation
from a Python loop or a recorded input replay.

## Prepare the world

First author an audio-enabled Relay Yard using its launcher and original licensed
Kenney sources, with `--author-only`. Supply that world and the newly published
component manifest to the preparation tool:

```powershell
python examples/performance-yard/prepare.py `
  --binary build/windows-runtime/poima.exe `
  --base-world build/relay-play/world.json `
  --manifest build/performance-artifact/game.poima-components.json `
  --output build/performance-world --autonomous
```

The tool copies the world and cooked store to a new directory and authors through
the native API. It verifies original entities, navigation, UI, component schemas,
audio and license declarations, and records exact asset hashes and native counts
in `manifest.json`. It leaves the supplied baseline intact. Preparation does not
download, compile, render or start gameplay.

`--autonomous` enables the compiled circuit. Omit it for manual control; the route
component defaults to disabled. A caller must omit `player.start.controller` when
the circuit is enabled, because the compiled game then owns that controller's
input. For manual play, use controller `00000000000000000000000000000064` and
camera `00000000000000000000000000000065` through the
[live player API](../../docs/LIVE_PLAYER.md).

## Measure

Use native Windows Python and a free desktop window:

```powershell
python tests/performance_yard.py `
  --binary build/windows-runtime/poima.exe `
  --world build/performance-world/world.json `
  --descriptor build/performance-artifact/native-gameplay.json `
  --output build/performance-measurement --gpu 1 --warmup 30 --duration 60
```

The default settings are 1920×1080, two frame slots, deferred lighting, medium
GTAO and native resolution. No frame reconstruction or generation is enabled.
The test requests foreground focus for its own window and observes actual focus;
losing focus invalidates the interval. It pauses only outside measurement for
guarded state snapshots and images, then stops and drains the frame recorder.

Results retain raw rows, CPU/GPU distributions, completed-submission draw counts,
route progress, process working set/private commit and owner shutdown status.
The test checks approximately 60 simulation ticks per second, a complete native
circuit, real weighted geometry and light work, and absence of hidden interventions.
Frame-time budget results are separate from functional success. A missed budget
stays a miss.

Timings measure application present-call returns, not display scanout or input
latency. A short run does not establish the
[30-minute reliability gate](../../docs/ALPHA_ROADMAP.md#qualification-target),
VRAM usage, physical input, audible output or clean-machine support. See
[frame capture](../../docs/FRAME_PERFORMANCE.md) for timing and memory definitions.

## Recorded checkpoint

The [0.0.94 measurements](../../docs/evidence/m2-performance-yard.json) use
30 seconds of warm-up and one minute of measurement per GPU on one Windows
laptop. Both runs maintain approximately 60 simulation ticks/s and complete
three circuits. The application present-return p95 is 12.444 ms on RTX 4070
Laptop and 27.409 ms on AMD integrated graphics; AMD misses the timing budgets.
Compressed raw rows and process-memory samples accompany the results.
