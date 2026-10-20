# Player lens presentation

An interactive player can interpolate world and first-person FOV between its
final two consecutive committed ticks. Gameplay remains authoritative at 60 Hz:
gameplay camera-lens reads and checkpoints still observe committed lenses.
Simulation and collision queries retain their current fixed-tick state. Camera
pose, input, geometry and animation are not buffered by this option.
[Camera effect interpolation](PLAYER_POSE_PRESENTATION.md) is a separate option.

This adds up to one fixed tick of FOV presentation lag. It does not add an ADS
timer or change the transition duration. Use the caller-owned
[`CameraFovTransition` helper](CAMERA_LENSES.md) to author deterministic easing
over ticks; presentation interpolation fills the gaps between those values.

## Enable it

`runtime.play` and shared-service `player.start` accept the optional strict
Boolean `lens_interpolation`. Omission or `false` retains exact committed-lens
presentation and omits the additional lens metadata.

Example parameters for `runtime.play`:

```json
{
  "session_id":"<session>", "request_id":"<request>",
  "expected_tick":0, "camera":"<camera>",
  "mode":"interactive", "lens_interpolation":true
}
```

For `player.start`, also supply `expected_generation` using the current player
generation. The usual session, tick, structure and lifecycle guards still apply.
Select `controller` when the player should route movement input.

Published games accept the same option at launch:

```text
poima game run game.json --lens-interpolation true
```

The CLI accepts `true` or `false`. Replay rejects interpolation before graphics
initialization: recorded replay retains exact committed lenses. The option is
per player launch and does not change a stored settings profile or gameplay ABI.

## Sampling and resets

For endpoint FOVs `previous` and `target`, in radians, the player uses:

```text
alpha = remaining fixed-tick accumulator / fixed-tick duration
q = exp((1 - alpha) * log(tan(previous / 2))
                  + alpha * log(tan(target / 2)))
rendered_fov = 2 * atan(q)
```

The interpolation is linear in log perspective scale; easing remains gameplay
code. Endpoint and unchanged FOVs are returned exactly. After catch-up, only the
last consecutive pair is retained. Lightweight lens observations supply each
committed tick without extracting a full scene snapshot per tick.

The world result then composes with the player's preferred neutral FOV using
the [authored-baseline zoom ratio](CAMERA_LENSES.md#player-fov-and-zoom).
First-person FOV is independent of that preference. Near/far planes are never
interpolated; a clipping change resets the pair.

Initialization, runtime/session/source or camera changes, material epochs,
authored FOV baseline changes, first-person lens presence changes and skipped or
repeated ticks reset to the current target. External owner edits and stepping
also reset instead of inventing a committed interval. Pause, inactivity, focus
loss, unavailable drawables, resizing, FOV preference changes and presentation
generation/view-cut changes invalidate the old presentation interval.

A reset reports `previous_tick == target_tick`, `alpha: 1` and
`interpolating: false`, with an explicit `reset_reason`. A configured player
continues reporting `enabled: true` while paused. The next valid consecutive
pair clears the reset reason. Several reset boundaries can coincide; the reason
records the reset chosen by the current presentation preparation.

## Inspect the lens actually used

Opt-in observed, submitted and presented player material receipts include
`lens_presentation`:

- `previous_tick`, `target_tick`, `alpha`, `enabled`, `interpolating` and
  `reset_reason` describe the sampling interval.
- `previous_world` and `target_world` are committed native lenses;
  `rendered_world` is the interpolated, preference-composed lens.
- `previous_viewmodel`, `target_viewmodel` and `rendered_viewmodel` describe the
  independent foreground channel, or are null when it is absent.
- `authored_fov` and `preference_fov` expose the world composition baselines.

The renderer scene receipt's `camera_lens` and foreground lens are the exact
rendered values. Owner/session, source, tick, player generation, frame sequence
and submission tags retain their existing meanings. A committed target is not
evidence that the renderer used that target lens.

When a shared settings owner is present,
`live_settings.fields["camera.vertical_fov"].effective` describes the current
committed target composed with the neutral preference.
`live_settings.application.effective_vertical_fov` describes the prepared
rendered FOV observed by the player. Use the exact presented scene receipt to
associate that lens with a completed presentation.

## Captures and persistence

`player.capture` retains its required exact `tick` guard. Use it when you need
a particular committed tick, such as a paused checkpoint. A tick mismatch
rejects; the method does not silently select a newer tick.

The separate `player.capture_current` operation chooses the current committed
tick atomically inside the shared headless owner dispatch. It forbids a `tick`
parameter, so a running player can be captured without a tick-read race:

```json
{
  "player_id":"<player>", "request_id":"<request>",
  "expected_control_revision":0, "session_id":"<session>",
  "path":"captures/ads.bmp"
}
```

Both operations require the current player identity, control revision and
session. The applicable `expected_structure_revision`, `expected_ui_revision`
and `expected_material_revision` guards remain unchanged. Both are available
only through the pumped shared headless player service, and require a ready
window and an exclusive new output path in an existing directory.

`player.capture_current` additionally returns `captured_tick` and the complete
raw `committed_camera_lens` for that tick: `entity`, `world`, nullable
`viewmodel`, and immutable `authored_world`. This state is returned whether or
not interpolation is enabled. It is separate from the actual rendered
`capture.lens` and optional `capture.lens_presentation`.

Both methods freeze one owned presentation snapshot before rendering. Lens
metadata, the exact scene receipt and BMP belong to that same sample. Neither
method advances a tick or the interpolation clock, or pauses playback. Native
capture preparations are tied to one window lifetime and preparation sequence;
later polls, preparations or pause boundaries invalidate them. Ownership,
preferences and committed lens endpoints must still match when the preparation
is submitted.

An acknowledged retry with the same method, request ID and parameters returns
its original receipt, including the original selected tick and lens state for
`player.capture_current`, even if gameplay has since advanced. Reusing that
request ID with different parameters or another method rejects rather than
capturing a different sample.

Checkpoints continue storing committed camera lenses and caller-owned gameplay
transition fields. They do not store a player's fractional render phase or lens
history. A restored owner starts a fresh presentation interval.

These receipt boundaries do not establish scan-out or input-to-photon latency.
This option does not promise continuous FSR history, camera-pose interpolation,
weapon collision handling or whole-game frame-time performance.
