# Camera effect presentation

An interactive player can interpolate the saved [camera effect offsets](CAMERA_POSES.md)
between its final two consecutive committed ticks. This smooths recoil, bob,
sway and shake authored at the fixed simulation rate. Native look, stance,
movement, animation, hit queries and `BaseWorld` remain current. This option does
not interpolate the player's whole body, the scene or the native look input.

Effect presentation adds up to one fixed tick of lag. Games still own their
spring state, accepted-shot index, seed and effect timing. Presentation history
is local to the window and is never serialized into gameplay checkpoints.

## Enable it

`runtime.play` and shared-service `player.start` accept strict Boolean
`pose_interpolation`. It defaults to false. Omission or false retains the
committed-camera path and omits `pose_presentation` history metadata. An authored
[foreground clearance profile](VIEWMODEL_CLEARANCE.md) still evaluates current
physics and supplies its clearance and actual-matrix receipts on that path.

```json
{
  "session_id":"<session>", "request_id":"<request>",
  "expected_tick":0, "camera":"<camera>",
  "mode":"interactive", "pose_interpolation":true,
  "lens_interpolation":true
}
```

Supply the normal launch guards, including `expected_generation` for
`player.start`. [Lens interpolation](PLAYER_LENS_PRESENTATION.md) remains an
independent option. Both work with the background elapsed clock, which deliberately
continues simulation without hardware input or physical window focus. Replay
rejects pose interpolation before graphics initialization.

Exported games use the same launch option:

```text
poima game run game.json --pose-interpolation true
```

## Sampling and transforms

For each channel, local position blends linearly. Quaternion rotation follows
the shortest unit-quaternion arc. Endpoints and unchanged complete channels are
returned exactly; interior rotations normalize copies of the endpoints.
The fraction is the remaining fixed-tick accumulator divided by the fixed-tick
duration. Catch-up retains only the final consecutive pair. Lightweight samples
observe each committed tick without extracting a full scene per tick.

Only the selected camera's effect offset changes during presentation. Native
extraction computes a separate owned transform table from current committed
local transforms, with that offset substituted. Geometry, camera-attached lights
and skin palettes use the same table. Other cameras retain their committed
offsets. Camera selection then applies the foreground effect once in rendered
camera space. Physics, audio listener state and logical entity transforms are
not rewritten. An exactly current override follows the original snapshot path.
That read path also preserves existing physical or animation-owned cameras at
their current offsets; it does not grant permission to write effects to them.

Custom native `PlayerSession` adapters must implement `pose_sample` with owned,
lightweight committed values and `pose_snapshot` with consistent owned geometry.
A missing provider rejects instead of silently displaying an unbuffered camera.
Default-off players never call these optional providers.

Initial samples, discontinuous ticks, owner/session/source/camera changes,
material epochs and foreground membership changes reset the pair. External
changes detected outside the player's own consecutive tick progression also
reset it. Pause/resume, interactive focus/inactivity, unavailable interactive
drawables, resize, FOV preference changes and presentation/view cuts invalidate the
old interval. Background mode retains its deliberate independent simulation
policy while focus or drawability changes.

A reset reports equal `previous_tick` and `target_tick`, `alpha: 1`,
`interpolating: false` and an explicit `reset_reason`. `enabled` remains true
while paused. A new valid consecutive pair clears the reset reason.

## Observe and capture

Opt-in observed, submitted and presented player material receipts contain
`pose_presentation`:

- `previous_tick`, `target_tick`, `alpha`, `enabled`, `interpolating` and
  `reset_reason` identify the sampling interval.
- `previous_world`, `target_world`, `rendered_world` describe the world-camera
  effect in local metres and X,Y,Z,W quaternion rotation.
- `previous_viewmodel`, `target_viewmodel`, `rendered_viewmodel` describe the
  independent foreground effect, or are null without a foreground channel.
  `rendered_viewmodel` is the desired interpolated offset before optional
  collision clearance.
- `target_base_world` is the current committed base camera matrix; it is never
  interpolated.

The corresponding renderer scene receipt adds `camera_pose`, containing the
actual prepared `camera_world` matrix and a sorted `viewmodels` list of
`entity`, `incarnation` and `world` matrices. These are the matrices submitted
with that scene, rather than a promise that the target effect was displayed.
Frame sequence and submission tags retain their existing meanings.

With authored `Camera.viewmodel_clearance`, native preparation resolves the
desired foreground against current Jolt physics after interpolation. The
`viewmodel_clearance` receipt records that capsule query and any retraction or
suppression. `camera_pose.viewmodels` contains the resulting draw matrices;
they can differ from the composition of `rendered_viewmodel` by the safe
camera-local +Z translation. If the anchor or final check is blocked, the list
is empty and suppression is explicit. The world-camera matrix, committed
offsets and gameplay aim remain unchanged. There is no release spring, and the
history does not feed a resolved clamp back into its next target.

[Player captures](PLAYER_LENS_PRESENTATION.md#captures-and-persistence) freeze
both pose/lens metadata and owned scene geometry under one window preparation.
They do not advance the simulation or render clock. Tick-guarded capture still
rejects a different tick; `player.capture_current` chooses its committed tick
inside owner dispatch. Stale preparation, changed committed endpoints/base,
source/material changes and another window's preparation reject before capture.
An acknowledged retry returns the original captured sample even after later
ticks. Checkpoints retain committed camera poses and game-owned state, without
fractional presentation phase or history.

This evolving launch option is outside authoring-core v1. It does not establish
whole-game animation interpolation, temporal reconstruction continuity,
arbitrary weapon-mesh clearance, scan-out timing or input-to-photon latency.

## Reproduce the compiled-game check

Build the Windows Vulkan/simulation/game-UI runtime and the CoreCLR game and
bridge from [FPS Feedback Range](../examples/fps-feedback/README.md). Use native
Windows Python from the checkout root, with an existing `build` directory.
Assign `engine`, `hostfxr`, `bridge` and `assembly` to the actual compatible
absolute paths. Both output directories below must be new:

```python
from pathlib import Path
import sys
sys.path.insert(0, "tests")
from switchback_ownership import gated

result = gated([
    sys.executable, "tests/fps_pose_player.py", "--binary", engine,
    "--hostfxr", hostfxr, "--bridge", bridge, "--assembly", assembly,
    "--output", "build/fps-pose-player", "--gpu", "0",
], Path("build/fps-pose-owner"), 240)
assert result["passed"] and not result["forced_cleanup"], result
```

The fixture owns a shared native host and a separate SDK connector; ordinary
world stdio cannot pump this live player. It seeds saved spring state, runs the
compiled game's relaxation and captures four real 640×360 MSAA4 frames. It
checks current-base/offset composition, shortest-arc rotation, independent FOV
interpolation, exact paused endpoints, stale capture rejection and retry after
a later tick. Two complete paused save comparisons retain all 59 typed fields.
Original calls, connector/host logs, saves, images and terminal lifetimes remain
in the output on success or failure. Retain the outer Job record as well.
The [qualification record](evidence/m2-player-pose-presentation.json) separates
these compiled-game checks from native transform and synthetic-window fixtures.
