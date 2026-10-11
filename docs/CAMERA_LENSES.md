# Gameplay camera lenses

World cameras and first-person arms/weapons have independent, persistent lens
controls. Use them for ADS, optical zoom and changes to near/far clipping.
Controls run on the fixed tick, alongside gameplay, and survive checkpoints.
They do not edit the authored scene or player preference profile.

## Agent API

Read a committed camera with `runtime.camera_lens`:

```json
{"session_id":"<session>","tick":12,"entity":"<camera>"}
```

The result contains `source: "runtime"` and `state` with `entity`, `world`,
nullable `viewmodel`, and immutable `authored_world`. Each lens contains
`vertical_fov`, `near` and `far`. A living entity without Camera returns
`state: null`; a missing entity rejects. The exact session and tick are required.
Reading does not advance simulation or acknowledge a rendered frame.
The getter is available while an interactive player is running. Its tick guard
can conflict if another player poll commits a tick between inspection and the
query; inspect again and retry. Direct `runtime.step` mutations still require
pausing that player.

Write through the existing guarded, retryable `runtime.step` operation:

```json
{
  "session_id":"<session>", "request_id":"<request>",
  "expected_tick":12, "ticks":1,
  "camera_lenses":[{
    "entity":"<camera>",
    "world":{"vertical_fov":35,"near":0.025,"far":250},
    "viewmodel":{"vertical_fov":50,"near":0.005,"far":5}
  }]
}
```

Provide one or both complete lenses; omitted channels retain their values.
An empty command or null lens rejects. FOV must be finite and within 5–150
vertical degrees, with `0.001 <= near < far <= 10,000,000` meters. A camera
without an authored first-person lens cannot gain one through gameplay.
Per-entity Camera presence, CameraDraw bindings and lens presence remain frozen;
configured instance births and retirements can change live camera membership.

Caller commands apply before the first Tick callback in a batch. Gameplay
setters stage until their callback succeeds; subsequent reads in that callback
return the committed values, including any caller controls. Commands are
limited to 64 unique camera targets per tick across caller and gameplay;
conflicting targets reject. Every lens change rolls back if any later part
of the batch fails. Controls on a reserved camera birth publish with its
instance; conflicting retirement rejects. A new birth starts from its own
recipe, never from a retired camera's state.

## C# gameplay

Declare `ICameraLensGame` to negotiate the independent `camera_lens_v1`
extension. It appends two callbacks to the existing epoch-7 service table,
requiring a 320-byte prefix. Older consumers retain their declared prefixes
and permissions.

### Fixed-tick ADS easing

`CameraFovTransition.Evaluate(startFov, targetFov, elapsedTicks, durationTicks)`
evaluates a smoothstep curve in logarithmic perspective magnification. FOV is
vertical degrees within 5–150; both counters must be within 0–3,600 ticks.
At 60 ticks per second, a 12-tick transition takes 0.2 simulation seconds.
Zero duration selects the exact target. A completed transition, its start and
an unchanged FOV return exact endpoint values.

Keep the transition recipe and phase in primitive `Game<TState>` fields:

```csharp
// Fields in your TState: EntityId Camera; double From, To;
// int Elapsed, Duration, Zooming; // Zooming is 0 or 1.
var camera = context.GetCameraLens(state.Camera)
    ?? throw new InvalidOperationException("Camera is missing.");
if (state.Zooming != 0)
{
    state.Elapsed = Math.Min(state.Elapsed + 1, state.Duration);
    double fov = CameraFovTransition.Evaluate(
        state.From, state.To, (uint)state.Elapsed, (uint)state.Duration);
    context.SetCameraLens(state.Camera,
        world: new CameraLens(fov, camera.World.Near, camera.World.Far));
    state.Zooming = state.Elapsed < state.Duration ? 1 : 0;
}
```

When starting or reversing a transition, seed `From` from the currently
committed FOV, set `To`, validate `Duration` within 0–3,600 and reset `Elapsed`.
Set `Zooming` to 1 even for zero duration so the next callback publishes its target.
This preserves position continuity when interrupted; velocity continuity is
not promised. World and viewmodel targets can use the same elapsed counter
with separate start/target fields. Preserve their near/far planes unless a
clipping change is intended.

The helper retains no hidden state and requires no service marker to evaluate;
publishing lenses still requires `ICameraLensGame`. Saves, compatible reload
and failed-tick rollback preserve the game-owned phase together with committed
lenses. Paused rendering, queries and captures do not advance it. A new camera
or an externally changed lens needs an explicit new transition recipe.

This is authoritative fixed-tick easing. Rendering between simulation ticks,
inertialized reversals and continuous FSR history are separate contracts.
The current world lens changes retain the renderer's history-reset policy.

For a world whose Camera ID is `00000000000000000000000000000001`:

```csharp
using Poima;

public struct MyState
{
    public EntityId Camera;
    public int Aiming;
}

[GameModule("my.game")]
public sealed class MyGame : Game<MyState>, ICameraLensGame
{
    public override void Initialize(ref MyState state)
    {
        state = default;
        state.Camera = new EntityId(0, 1);
    }

    public override void Tick(ref MyState state, GameContext context)
    {
        var camera = context.GetCameraLens(state.Camera);
        if (camera is null) return;
        context.SetCameraLens(state.Camera,
            world: state.Aiming != 0 ? new CameraLens(35, 0.025, 250)
                                : camera.Value.AuthoredWorld);
    }
}
```

Set `Aiming` from your gameplay input logic; this example only applies the lens.

`GetCameraLens` returns null for a living noncamera. `SetCameraLens` accepts
world and/or viewmodel lenses. These APIs are available in Tick, with at most
256 reads and 64 admitted writes per callback. They are unavailable in Control.
Changing FOV over several ticks is ordinary gameplay code; this milestone does
not supply a weapon framework, camera easing or interpolated ADS presentation.

## Player FOV and zoom

A player's stored `camera.vertical_fov` is their neutral world FOV. Gameplay
zoom composes with it using perspective magnification, rather than multiplying
angles or overriding their preference:

```text
ratio = tan(runtime_fov / 2) / tan(authored_fov / 2)
effective_fov = 2 * atan(tan(player_fov / 2) * ratio)
```

The calculation uses radians internally and returns degrees. Effective FOV
saturates to 5–150 degrees. Without a player override, the runtime FOV is used
exactly. Restoring the authored runtime FOV restores the player's chosen FOV
exactly. Updating a preference while zoomed changes the baseline immediately;
it does not rewrite gameplay lens state. First-person FOV and clipping remain
independent of the world preference. Existing settings observations report the
composed effective value.

## Persistence and rendering

Camera-bearing runtime snapshots use version 7, with complete sorted camera
membership and both live lens channels. Reload validates the frozen Camera
membership, first-person lens presence and all bounds before publication.
Accepted older snapshot versions restore authored lenses. Camera-free
snapshots retain their existing formats. Explicit gameplay-schema upgrades
preserve the camera section; they do not create camera migrations.

Observed, submitted and presented renderer receipts include the exact effective
world `camera_lens` and the existing foreground lens/bindings. A stale
projection cannot pass receipt matching. These receipts describe renderer work,
not scan-out or click-to-photon latency.

World FOV/clipping changes retain the renderer's current temporal-history reset
policy. Foreground-only lens changes no longer reset world reconstruction
history. Continuous temporal ADS quality, a live-window lens qualification and
presentation interpolation remain separate work.

Qualification results are recorded in
[the camera-lens evidence](evidence/m2-camera-lenses.json).
