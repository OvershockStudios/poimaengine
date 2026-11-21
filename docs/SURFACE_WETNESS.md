# Shelter-aware surface wetness

Add `SurfaceWetness` to a rigid world drawable with an explicit opaque or
masked `PbrMaterial`. Each receiver has one local point that samples rain
exposure and one film value that stays with the entity when it moves.

```json
{
  "capacity_nm": 12000,
  "retention_q16": 32768,
  "drying_nm_per_second": 1000,
  "probe_local_m": [0, 0.5, 0],
  "darkening": 0.35,
  "roughness_factor": 0.32
}
```

A world supports up to 64 authored receivers. Capacity is an integer from
1 to 20,000,000 nm. Retention is from zero to 65,536, with 65,536 meaning
full retention. Per-second drying is an integer from zero to capacity.
Each probe coordinate can be up to 16 local metres from the origin.
The two material factors are in [0, 1]. At full wetness, this example uses
65% of the dry base tint and 32% of the dry roughness factor.

The probe casts 100 m up in world space after the tick's physics poses
have been published. The nearest static or kinematic collider shelters the
point. The ray skips the receiver's body, dynamic bodies, and character
controllers. Use a roof collider where shelter should apply. A large
drawable uses the same exposure and film across its full extent.

## Apply rain and drying

Each dose covers one complete game-clock second. The first batch can begin at
any supported second. Later batches start at `next_quantum`. Send 1–64
integer rain depths of 0–1,000,000 nm. Send zero for a dry second so the film
can drain. Each second adds rain depth times retention when exposed, then
subtracts drying and clamps the result between zero and capacity. Fractional
nanometres are retained in Q16 state.

Discover the `wetness` section and the two methods below. Use the same
session, request ID, tick and structure guards as for `runtime.step`.
The initial tick of a `runtime.wetness.step` applies the full batch at its
final poses. Replay a moving roof or receiver at the intended sampling
times. Group consecutive seconds when their exposure is held constant.

```json
{"first_quantum":800,"rain_nm":[5000,5000,0]}
```

Put that object in `wetness_dose`. Read `runtime.wetness.inspect` with
`session_id`, `tick` and `structure_revision`. Optionally select one
`entity`. The result includes the cursor and sorted receiver rows with
`film_nm_q16`, capacity and the blocker and exposure from the last accepted
batch. Exposure is null before the first sample. A retained retry returns
the original receipt. A failed tick restores the cursor and film. A save
retains the cursor, exact film and last exposure.

## Drive it from compiled gameplay

Declare `ISurfaceWetnessGame` to request the named 576-byte
`surface_wetness_v1` gameplay service. In `Update`, stage at most one dose
batch per tick. A tick accepts its dose from the caller or gameplay.
The [weather package](WEATHER_CYCLE.md) integrates scheduled rain over the
same complete seconds. Save your game clock or next source index in game state.

```csharp
Span<uint> rain = stackalloc uint[1];
rain[0] = forecast.RainDepthNanometers(state.NextQuantum);
context.AdvanceSurfaceWetness((ulong)state.NextQuantum, rain);
state.NextQuantum++;
```

Forward and deferred lighting use the same dry material with a wet base
tint and roughness factor applied per draw. Textures and existing surface
marks still contribute to the final surface. The renderer compares the
effective factors against its last accepted draw, using the existing local
surface-change signal. The authored material and collision stay intact.
Shallow water and snow geometry retain their own state.

Run the [shelter contract](../tests/world_wetness_contract.py) against a
simulation build to inspect exposure, uptake, drying, rollback and saved
continuation. The [Vulkan fixture](../tests/world_wetness_capture.py) checks
the material and collision in a relocated Windows game.
