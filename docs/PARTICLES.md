# Fixed-tick scene particles

A `ParticleEmitter` component gives an entity or spawn-template member a
seeded fixed-tick effect. Use [focused discovery](WORLD_SERVICE.md#focused-discovery-development)
to retrieve the component and `runtime.particles.inspect` schemas from the
installed executable. Authoring needs the component schema. Running and
shipping the scene need simulation and, for export, a matching runtime with
`features.scene_particles`. Windows Vulkan presents camera-facing quads.
An emitter can use the default opaque emissive quad or a textured,
translucent sprite.

## Author a scene effect

Use the following component value in a revision-guarded `component.set`
operation or a spawn template. This example emits detached sparks from a
rotated source when the `spark` input action is pressed.

```json
{
  "spawn_space": "local",
  "simulation_space": "world",
  "seed": "0000000000000007",
  "lifetime_ticks": 36,
  "size_m": 0.035,
  "linear_rgb": [1, 0.35, 0.08],
  "velocity_m_s": {"minimum": [-1, 2, -1], "maximum": [1, 4, 1]},
  "acceleration_m_s2": [0, -9.81, 0],
  "burst": {"action": "spark", "count": 12}
}
```

Bind `spark` in a [named input profile](INPUT_ACTIONS.md) for physical play.
Replay can supply the named action directly. A pressed edge triggers a burst
once, even when its input spans several ticks. The emitter uses the pose
committed after physics, animation and attachments for that tick. An emitter's
world basis must be rigid and proper. An inherited scale, shear or reflection
that violates this basis causes the edit or runtime operation to fail.

Continuous emission defaults to zero. Set `particles_per_second` to an integer
up to 61,440. `emitting: false` pauses the continuous rate and still permits
explicit bursts. The optional seed is a sixteen-digit lowercase hexadecimal
string, which preserves all 64 bits in JSON. Lifetime is 1–36,000 ticks, size
is 0.001–100 m, and linear RGB channels are 0–1. Authored offset, velocity
and acceleration components are bounded to ±1,000,000 in their respective SI
units. Random ranges must be ordered on each axis. The scene reserves up to
16 active or draining sources and 4,096 live particles. Set `live_limit` to
bound one source within that pool. Zero allows the full shared capacity.

Removing an attached source removes its local particles immediately. Removing
a world-space source stops emission and lets its detached particles live out
their original lifetime. A draining source retains its identity until the
last particle expires. A runtime attachment transfer updates local particle
positions at the same tick, with their ages unchanged. The renderer uses
frame-owned Vulkan buffers for the changing quad geometry.

## Add a translucent sprite

Import a PNG or JPEG through `asset.image.import`. Choose `srgb` for ordinary
color textures. Use the resulting cooked image SHA-256 in the emitter's
`sprite` value.

```json
{
  "texture": "<cooked image SHA-256>",
  "opacity": 0.5,
  "depth_fade_m": 0.4
}
```

Opacity defaults to 1. Depth fade defaults to zero, which gives a hard
occlusion edge at solid geometry. A positive value up to 10 m fades the sprite
over its distance from the opaque surface behind it. The sprite uses
`linear_rgb` as a tint. The renderer premultiplies each texel in linear space
before bilinear filtering at the base mip. This prevents hidden RGB in a
transparent texel from bleeding into its neighbors.

The translucent pass sorts instances from far to near across emitters.
Persistent particle IDs break ties at the same view depth. It blends into
scene HDR after opaque shading and before independent foreground, post
effects, bloom, tone mapping, and UI. Depth, normal, and motion observations
continue to describe the opaque geometry underneath.

Use one world sample with reconstruction disabled. The exported runtime must
advertise `features.scene_particle_sprites`. Cooked images belong to the
frozen world and are prepared even before an emitter's first burst. Up to 16
distinct sprite images may be referenced by the world and its templates. Each
image is at most 1024×1024, with a combined 16 MiB limit for the base levels.

## Observe and verify

After a completed tick, call `runtime.particles.inspect` with that tick and
the runtime session ID. The reply reports source poses, draining status,
spawn/expiry/removal counts and live positions and velocities in world space.
Read up to 64 particles per page. To continue, send the decimal-string
`next_after` value as `after` and keep the same session and tick. A changed
tick fails the guard. The counts distinguish capacity drops from particles
that expired or were removed.

The simulation carries source identities, seeds, positions, velocities and
remaining lifetimes through rollback and durable saves. Use replay to compare
the attached and detached centers at the same completed ticks, then use
`game verify` and a Vulkan capture to verify the shipped game. The fixture
tests both spaces in motion, template removal, world-space draining and
byte-identical continuation across fresh processes.

```sh
python3 tests/world_particles_contract.py build/linux-game/poima
python3 tests/world_particles_contract.py "$runtime/bin/poima.exe" \
  --windows-interop --runtime-root "$runtime" --capture-output build/particles --gpu 0
```

Run the standalone sprite fixture to check overlap, depth occlusion, fade,
texture fringes, and the final color at the selected pixels.

```sh
python3 tests/sprite_particles_capture.py "$runtime/bin/poima.exe" \
  --windows-interop --runtime-root "$runtime" --output build/sprite-captures --gpu 0
```

Surface deposition, mesh and ribbon rendering, general VFX graphs, translucent
MSAA and temporal reconstruction, and production-scale frame budgets are
further work.

## Native particle API

The native [`poima::particles::System`](../include/poima/particles.hpp) advances
temporary visual effects at 60 ticks per second. It takes emitter definitions,
rigid poses and explicit bursts, and writes world-space instances into
caller-owned memory. The headless test uses a moving source to show the
difference between attached particles and a detached trail.

### Run an emitter

An owner registers an emitter with a nonzero ID, its seed and its initial pose.
Set the source pose and queue any gameplay-triggered bursts before calling
`step()` for the next tick. `write_instances()` emits live positions, velocities,
stable particle IDs, emitter IDs, age, lifetime, size and linear RGBA. The output
is in spawn order. The receiving renderer can associate an emitter ID with its
own presentation resources.

```cpp
#include "poima/particles.hpp"
#include <array>

using namespace poima::particles;

System particles({2, 64});
EmitterConfig sparks;
sparks.seed = 7;
sparks.simulation_space = Space::World;
sparks.velocity_m_s = {{-1, 2, -1}, {1, 4, 1}};
sparks.acceleration_m_s2 = {0, -9.81, 0};
sparks.lifetime_ticks = 36;
sparks.size_m = 0.035f;
sparks.linear_rgba = {4, 0.6f, 0.1f, 1};
particles.add_emitter(1, sparks);
particles.set_pose(1, {{2, 1.5, 0}});
particles.burst(1, 12);
particles.step();

std::array<Instance, 64> storage;
const auto live = particles.write_instances(storage);
```

### Spaces and tick order

`spawn_space` chooses the axes for the spawn offset and initial velocity. Local
axes use the emitter rotation at birth. World axes align with the world. Both
use the current emitter position as the origin of the spawn offset. Positions,
velocities and acceleration use metres, metres per second and metres per
second squared. The pose is a translation and an `x, y, z, w` unit quaternion.

`simulation_space` chooses whether particles follow the emitter's subsequent
translation and rotation. Local particles integrate in emitter coordinates and
project through the latest tick's pose. World particles integrate independently
after spawning. Acceleration is expressed in simulation axes. Reported velocity
describes particle motion within its simulation frame projected into world axes.

Each step increments existing ages, expires particles at their lifetime, and
updates the survivors with semi-implicit Euler (`v += a / 60`, then
`p += v / 60`). It then applies queued poses and spawns age-zero particles.
Pausing continuous emission keeps its fractional phase. Queued bursts still
run and existing particles age. Emitters can drain this way before removal.
Removing an emitter immediately retires its live particles and releases its
slot. Queued bursts for that emitter retire with the slot.

### Capacity and replay

- One system supports up to 128 emitters and 65,536 live particles. The
  constructor reserves its chosen capacities. A definition can set a lower live
  limit for its own emitter.
- Continuous emission uses an integer `particles_per_second` rate from 0 to
  61,440 and retains its remainder across ticks. Up to 1,024 burst particles can
  be queued per emitter per tick. Lifetime ranges from 1 to 36,000 ticks.
- Authored pose, offset, velocity and acceleration components must be finite
  and within ±10⁹. Each random range must be ordered. Size is positive and at
  most 10⁹ metres. Linear RGB is in [0, 10⁹] and alpha is in [0, 1].
- Existing particles keep their places when capacity is full. Spawns are
  admitted in emitter slot order using each emitter's live limit and remaining
  shared capacity. New emitters take the first free slot. Excess attempts are
  counted as dropped and are not queued for a later tick.
- Each emitter has a seeded SplitMix64 stream. Every spawn attempt occupies six
  samples including dropped attempts. Reconstruct a system and submit the
  same definitions, poses and emission actions in the same order for a repeatable
  replay in the same build and floating-point environment.
- `status()` reports the completed tick, configured capacities and counts.
  `emitter_status()` reports per-emitter counts, its sampled pose and pending
  emission. System counts satisfy `spawned = live + expired + removed`.
  `write_instances()` rejects an undersized output before writing.

### Headless check

Compile and run the standalone native fixture from the repository root.

```sh
mkdir -p build
c++ -std=c++20 -Iinclude src/particles.cpp tests/particles_native.cpp -o build/poima-particles-test
build/poima-particles-test
```

The fixture checks both spawn axes against a rotating emitter, attached and
detached trajectories, expiry, same-seed replay, random sequence continuity
under overflow, capacity ordering, removal and invalid inputs.
