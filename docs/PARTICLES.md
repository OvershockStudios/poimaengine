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

## Place a roof-aware rain source

Give an authored world entity a `ParticleEmitter` with world spawn and
simulation spaces. Weather drives its births through the same guarded
[rain doses](SURFACE_WETNESS.md#apply-rain-and-drying) used for surface films.
Rain-only worlds can receive doses. A rain source uses zero continuous rate.
All of its births originate from weather.

```json
{
  "spawn_space": "world",
  "simulation_space": "world",
  "seed": "0000000000000721",
  "lifetime_ticks": 12,
  "size_m": 0.03,
  "linear_rgb": [0.24, 0.31, 0.42],
  "live_limit": 96,
  "offset_m": {"minimum": [-1.1, 0, -0.9], "maximum": [1.1, 0, 0.9]},
  "velocity_m_s": {"minimum": [0.1, -10, 0], "maximum": [0.4, -7, 0.2]},
  "rain": {"nm_per_particle": 30, "max_particles_per_tick": 6}
}
```

The last complete game second in a dose batch sets the current visual
intensity. It takes effect on the accepted tick and lasts up to 60 simulation
ticks, including that tick. A new dose replaces the window. Zero closes it.
All seconds in the batch contribute to wetness. During an active window,
the emitter turns one depth increment per tick into a bounded integer birth
count. Dividing the dose's nanometres by `nm_per_particle` gives the scheduled
births across a full window, subject to the tick cap and carry-over.
The native phase retains the fractional part across active windows. Set
`nm_per_particle` between 1 and 1,000,000 and `max_particles_per_tick`
between 1 and 32. Particle capacity and `emitting: false` also apply.

Place the emitter's origin within the same area as its spawn offsets.
The physics roof test casts up 100 m from that origin each active tick and
uses the final physics pose. Static and kinematic collision can block the
source. Covered ticks spend their scheduled births. Live drops keep aging
and moving in world space when the window ends or a roof slides over the
source. Place separate emitters on opposite sides of a shelter boundary.
This pass samples one exposure point per emitter and handles birth and
motion through the established particle path.

To keep rain active during play, implement `ISurfaceWetnessGame` and call
`context.AdvanceSurfaceWetness` with successive complete game seconds.
With a one-to-one game clock, send one dose every 60 gameplay ticks. Save
the game's clock and forecast using gameplay state. Configure a zero dose
for a dry second. Add a translucent sprite for a visible streak.

`runtime.particles.inspect` returns a `rain` window with the last dose depth,
remaining ticks, and the last active sample tick. Each enabled rain
source includes its phase, blocker and scheduled births from that sample.
The phase denominator is `nm_per_particle * 60`.
The native counts report actual spawn and capacity outcomes. The scheduled
count belongs to `sample_tick` after a dry dose or the window's expiry.
The rain clock, birth window, phase, shelter witness and live drops
participate in rollback and save continuation.

```sh
python3 tests/world_precipitation_contract.py build/linux-game/poima
python3 tests/world_precipitation_contract.py "$runtime/bin/poima.exe" \
  --windows-interop --runtime-root "$runtime" --gpu 0 --output build/rain-captures
```

Pass `--native-descriptor` with the descriptor from the
[compiled weather fixture](SURFACE_WETNESS.md#drive-it-from-compiled-gameplay)
to test a rain-only world through the existing gameplay service.

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
scene HDR after opaque shading, fog and reflections and before FSR,
independent foreground, post effects, bloom, tone mapping and UI. Depth,
normal and motion observations continue to describe the opaque geometry
underneath. With FSR active, the [temporal sprite path](RECONSTRUCTION.md#textured-world-sprites)
uses visible particle coverage to protect the current frame's sprite color.

Use one world sample. The exported runtime must
advertise `features.scene_particle_sprites`. Cooked images belong to the
frozen world and are prepared even before an emitter's first burst. Up to 16
distinct sprite images may be referenced by the world and its templates. Each
image is at most 1024×1024, with a combined 16 MiB limit for the base levels.

### See particles through water

Translucent sprite emitters automatically use active [water optics](SHALLOW_WATER.md#author-transparent-water).
The renderer traces the committed water to each visible card. A card seen
through water picks up the same depth-dependent absorption and
in-scattering as opaque objects. Reflections and foam remain visible in
front of particles behind the interface. Cards in front of the water retain
their authored color. Overlapping cards use the ordinary back-to-front blend
and retain the water shared by both sightlines. This includes FSR coverage,
opaque occlusion and authored soft depth fade.

The runtime advertises `features.water_sprite_optics` and completed frames
report `render_diagnostics.water_optics.sprites` when they used this path.
Existing emitters and cooked water assets use their current formats.
Refraction samples scene depth for opaque geometry. Sprite positions use
their direct screen projection. The sprite pass leaves atmospheric haze
and local height fog behavior as before.

The [water sprite fixture](../tests/water_sprite_capture.py) compares
translucent cards against opaque colors at matching depths in air and water.
It checks overlapping and blocked cards, FSR and a resumed standalone save.

### Animate size, opacity, and the sprite sheet

Add `size_over_life`, `opacity_over_life`, or `flipbook` inside the emitter's
`sprite` object. This example starts small, grows, fades, and plays six cells
from a four-column, two-row texture.

```json
{
  "texture": "<cooked image SHA-256>",
  "size_over_life": [[0, 0.4], [0.25, 1], [1, 1.5]],
  "opacity_over_life": [[0, 0], [0.25, 1], [1, 0]],
  "flipbook": {
    "columns": 4,
    "rows": 2,
    "first_frame": 1,
    "frame_count": 6,
    "ticks_per_frame": 4,
    "loop": false
  }
}
```

Curves have two to eight knots. Phases start at 0, increase strictly, and end
at 1. The renderer multiplies the base size and opacity by linearly
interpolated values at `age_ticks / lifetime_ticks`. Size multipliers range
from 0 to 8 and opacity multipliers from 0 to 1. An omitted curve supplies 1.
A particle with zero displayed size or opacity still ages and counts toward
the simulation limit.

Sprite-sheet frames are numbered from zero, left to right and then top to
bottom. The source image must divide evenly into the grid, which can have up
to 32 columns and 32 rows. `first_frame` defaults to 0. The active range must
fit inside the sheet. `ticks_per_frame` sets the hold time at 60 Hz. Looping is
on by default. With looping off, the sprite holds its last frame until expiry.
Bilinear taps stay inside the selected cell, including one-texel cells.

## Trigger a burst from gameplay

Implement `IParticleBurstGame` on a C# game module to declare the optional
`particle_burst_v1` service. Inside `Tick`, call
`context.BurstParticles(emitter, count)` with the entity ID of a
`ParticleEmitter`. This also accepts an emitter created by a template spawn
earlier in the callback. Use this for effects tied to gameplay, such as dust
when a character lands or sparks when a moving switch reaches its stop.

Requests take effect after motion, physics, and attachment updates. A burst
uses the emitter's final world pose for that tick. If its target is removed
before then, the entire step batch rolls back. A failed tick also rolls back
particles created on earlier ticks of the same batch.

Each request takes 1 to 1,024 particles. The engine accepts up to 32 callback
attempts per tick. Each emitter's gameplay requests and named input burst
share the 1,024-particle burst limit. Exceeding the combined limit rejects
the batch. Global and per-emitter live-particle limits still apply, and
`runtime.particles.inspect` counts births dropped at capacity. Query
`gameplay_particle_bursts` in `world.describe` for these limits.

The callback uses a 32-byte request and the optional 536-byte epoch-7
service prefix. Earlier gameplay modules keep their negotiated service
views. Pending gameplay requests exist only during a tick. Committed
particles continue through the same replay and save format.

To reproduce the compiled fixture, publish it with a serviced .NET 10 SDK
on the target operating system. Set `$dotnet` to that SDK's executable.
The build and test paths below run on Linux.

```sh
python3 scripts/publish_native_gameplay.py \
  --project tests/managed_particle_burst_gameplay/Poima.ManagedParticleBurstGame.csproj \
  --type Poima.Tests.ManagedParticleBurstGame --dotnet "$dotnet" \
  --output build/press-native
python3 tests/gameplay_particle_burst_contract.py build/linux-game/poima \
  --native-descriptor build/press-native/native-gameplay.json --output build/press-checks
```

The fixture also accepts `--hostfxr`, `--bridge`, and `--assembly` for the
CoreCLR route. Pass an installed Windows `--runtime-root` to check a
relocated Native AOT build and `--gpu` to capture its output.

## Observe and verify

After a completed tick, call `runtime.particles.inspect` with that tick and
the runtime session ID. The reply reports source poses, draining status,
spawn/expiry/removal counts and live positions and velocities in world space.
Read up to 64 particles per page. To continue, send the decimal-string
`next_after` value as `after` and keep the same session and tick. A changed
tick fails the guard. The counts distinguish capacity drops from particles
that expired or were removed.

Sprite observations include a `presentation` object with the cooked texture,
sampled size and opacity, `texel_rect` as `[x, y, width, height]`, and
`visible` for nonzero size and opacity. The `frame` field is null when the
sprite uses the entire image. These values derive from the frozen emitter
and saved particle age.

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

Run the lifetime fixture to verify curve samples, atlas boundaries, and
fresh-process continuation at specific particle ages.

```sh
python3 tests/sprite_lifetime_capture.py build/linux-game/poima \
  --headless --output build/sprite-lifetimes
python3 tests/sprite_lifetime_capture.py "$runtime/bin/poima.exe" \
  --windows-interop --runtime-root "$runtime" --output build/sprite-lifetimes --gpu 0
```

Surface deposition, mesh and ribbon rendering, general VFX graphs, translucent
MSAA and temporal reconstruction, and production-scale frame budgets are
further work.

## Stop particles at surfaces

Use `surface_collision: "expire"` on a `ParticleEmitter` for drops, sparks
or small debris that should disappear on contact. Set `simulation_space` to
`world` and an explicit `live_limit` from 1 to 64. Local and world spawn axes
both work. The same setting can be used on a weather-driven rain emitter.

```json
{
  "spawn_space": "local",
  "simulation_space": "world",
  "surface_collision": "expire",
  "live_limit": 32,
  "lifetime_ticks": 45,
  "particles_per_second": 48,
  "size_m": 0.055,
  "linear_rgb": [1, 0.38, 0.045],
  "velocity_m_s": {
    "minimum": [-1, -9, -1],
    "maximum": [1, -5, 1]
  },
  "acceleration_m_s2": [0, -9.81, 0]
}
```

At each fixed tick, Poima tests the segment from the previous particle
centre to its integrated next position against the final poses of static
and kinematic colliders. This includes terrain triangle meshes. It ignores
the source's own body. An age-zero birth starts moving at the next tick.
Lifetime expiry takes precedence when the particle reaches its last tick.
A contact removes the particle before publishing render instances and
frees its capacity for births on the same tick.

The surface test uses the centre of the particle and two-sided triangle
queries. It costs at most one sweep per moving candidate and 1,024 sweeps
per world tick. Segments shorter than 1 mm or longer than 10 km and endpoints
beyond ±1,000 km count as skipped. [Native ray query reference](https://jrouwe.github.io/JoltPhysicsDocs/5.7.0/class_narrow_phase_query.html)

Read `runtime.particles.inspect` at the committed tick. An opted-in source
reports `surface_collision.last_step` with its `tick`, `tested`, `skipped`
and `hits`. `first` identifies the first contact in particle ID order when
one occurred, including the receiver, hit position, nullable normal and
nullable mesh triangle. `last_step` starts empty after opening a save.
The cumulative `collided` counter persists with the usual particle state.
A zero collision count is omitted. The counts balance as
`spawned = live + expired + removed + collided`.

An installed runtime needs `features.particle_collisions` for an export
that uses the feature. Run the [native contact fixture](../tests/particle_collision_capture.py)
against a simulation build, or add `--gpu` for Vulkan image checks. With
`--runtime-root`, the fixture runs a relocated export and opens its save in
a separate process. From WSL, use `--windows-interop` and a local Windows
drive for the fixture's save directories.

## Spawn a burst at contact

Add `impact_burst` to a source that uses `surface_collision: "expire"`.
Name another `ParticleEmitter` and choose 1–8 births per contact.

```json
{
  "impact_burst": {
    "emitter": "00000000000000000000000000000384",
    "count": 3
  }
}
```

Give the receiving emitter `simulation_space: "world"`. A receiver can
serve multiple sources. To use it only for contact bursts, leave the
continuous rate at zero and omit its input burst. Keep weather-driven
emission on the source. Members of a hierarchical template can reference
another emitter in that template. Each spawned instance resolves the link
to its own receiver.

The impact point is the receiving particle's birth origin. For
`spawn_space: "local"`, local +Y points out of the surface and opposes the
incoming sweep. A splash can use an upward velocity and a small positive
Y offset to clear the surface. `spawn_space: "world"` keeps the authored
axes. The target's own entity pose and the trajectories of existing particles
stay intact. Contact bursts use the receiver's color, sprite, size, velocity
range, lifetime and acceleration. A hit with no surface normal retires the
source particle without offering a burst.

Births appear at age zero on the contact's simulation tick. They start
moving on the next tick. Within each receiving slot, ordinary births take
space first. Contact bursts follow in incoming particle ID order and share
the slot's 1,024-burst tick budget, live capacity and seeded random sequence.
The pool counts attempts that reach the live-capacity check. Requests beyond
the burst limit are skipped before consuming random samples.

In `runtime.particles.inspect`, the source's `impact_burst` includes its
receiver and `requested_last_step`. Compare the requested count with the
receiver's age-zero particles and its `spawned` and `dropped` totals. Saves
retain the spawned world positions, velocities, ages and counters. A restored
source begins with an empty last-step trace. Sources already draining after
instance retirement continue to collide with surfaces.

Exports require the `particle_impact_bursts` runtime feature. The
[impact fixture](../tests/particle_impact_burst_capture.py) uses the same
arguments as the surface contact fixture. It checks a sloped ledge, rain
on terrain, capacity ordering, a remapped template and relocated save
continuation. [Qualification results](evidence/m2-particle-impact-432.json)

### Vary a burst by surface

Give each receiving [collider a surface name](PHYSICS_INTERACTIONS.md#name-a-collision-surface).
`impact_burst.by_surface` accepts up to eight names. A matching entry replaces
the fallback. Use `null` to suppress a burst on that surface. Untagged and
unmatched hits use the fallback when it is supplied. Authors can omit both
top-level `emitter` and `count` to respond only to named entries.

```json
{
  "impact_burst": {
    "emitter": "00000000000000000000000000000384",
    "count": 2,
    "by_surface": {
      "metal": {"emitter": "000000000000000000000000000003b6", "count": 4},
      "stone": null
    }
  }
}
```

Contact expiry still occurs on a suppressed hit. Tagged samples in
`runtime.particles.inspect` include `surface` on the first contact. A source
with named routes also reports `impact_emitter` on its first contact. A null
value means that contact offered no burst. Follow the chosen pool's counts
and new particles to see which offers were admitted.

The [surface fixture](../tests/collision_surfaces_capture.py) checks a tagged
moving box, a tagged cooked terrain mesh, suppression, fallback and a local
route inside a spawned template. Use the same arguments as the contact
fixture for headless or Vulkan checks. [Qualification results](evidence/m2-collision-surfaces-433.json)

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
  emission. System counts satisfy `spawned = live + expired + removed + collided`.
- `step(ContactTest)` queries world-space centre segments for contact-enabled
  sources before mutating particle state. Worlds without such sources can
  continue to call `step()`.
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
