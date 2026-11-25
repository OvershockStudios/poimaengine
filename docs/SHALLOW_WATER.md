# Local water volume

[The native water grid](../include/poima/shallow_water.hpp) moves water through
bounded basins, channels and floodable ground. A caller supplies floor heights,
opens or blocks passages, adds or drains water at chosen cells, and advances
fixed steps. A world can bind one region to cooked terrain and use named inputs
to control gates. The solver uses a single layer of overdamped flow.

## Author a terrain-bound region

Use Poima 0.0.353 or later. Cook and bind the native terrain following
[terrain authoring](TERRAIN.md#author-terrain-in-a-world). Retain the terrain
recipe digest and tile entity IDs in the order the cooker returned them.
Discover `world.water.plan` and `world.water.cook` through `world.describe`
or MCP `poima_discover`.

Plan the region at the current world revision. This example puts two one-metre
cells on a terrain footprint covering X 0–2 m and Z 0–1 m. Replace the digest
and ID placeholders with values from the current project.

```json
{
  "revision": 2,
  "terrain": {"source": "terrain/basin.json"},
  "expected_terrain_recipe_sha256": "<reviewed terrain recipe SHA-256>",
  "tile_entities": ["<first cooked tile entity ID>"],
  "grid": {"cells_x": 2, "cells_z": 1, "cell_size_m": 1,
    "origin_x_m": 0, "origin_z_m": 0, "relaxation_per_second": 6},
  "initial_depths_m": [0.75, 0],
  "faces": [{"cell": [0, 0], "axis": "x", "openness": 0,
    "sill_height_m": 0.25, "action": "sluice.toggle", "action_openness": 0.5}],
  "exchanges": [{"cell": [1, 0], "source_m3_per_second": 0,
    "drain_m3_per_second": 0.012}]
}
```

`terrain` also accepts `recipe` with inline JSON text. Draft file paths start
at the entry world's directory. The region must fit the rounded terrain
surface, including its outer cell corners. The cooker samples each bed at the
cell centre. Selected terrain tiles have root `StaticMesh` and `MeshCollider`
pairs bound to primitive zero at identity transforms.

Send the same request to `world.water.cook` with `expected_plan_sha256` from
the preview. This publishes the exact reviewed `.pwater` bytes. Bind the hash
in a [guarded world transaction](WORLD_SERVICE.md#iteration-conflicts-and-retries)
with `{"op":"water.set","asset":"<plan_sha256>"}`. Use `asset:null` to
clear it. Undo and redo use the same authored field. A stale revision, recipe,
plan or tile binding returns `-32009`. A missing or corrupt package returns
`-32050`. Recook and rebind after changing terrain tiles. Active sessions
continue using their frozen definition until stopped or replaced.

If project preflight stops on a stale binding, open the entry world directly
with `poima world <world.json>`. Clear the stale binding or cook and bind a new
region before opening through the project manifest again.

After binding, inspect the initial water surface in the editor's Scene view
or the stopped Game camera. Agents can capture the same initial depths with
`world.capture` at the current authored revision. Select the underlying terrain
to edit the bed, and use the water binding to change the region. Capture a
runtime tick to inspect the result of flow, rainfall, or an open sluice.

One region supports up to 32×32 cells, 128 authored faces and 128 exchange
cells. Omitted depths start dry. Interior faces default to open, so close the
other faces of a dam to prevent bypassing a sluice. Sills use absolute world Y.
Named input presses flip each matching face between its initial and action
openness. Held inputs leave the gate in place. One press can operate several
faces. Authored source and drain rates apply each committed 60 Hz tick.

## Collect precipitation

Set optional `rain_cells` in the plan and cook requests to designate an open
catchment, for example `"rain_cells":[[0,0],[1,0]]`. Addresses must be unique
and within the grid. The plan orders them by Z then X before hashing.
An absent or empty list keeps the original region format and behavior.
A nonempty list requires an export runtime with `features.water_rain`.
Rain-only regions use `.pwater` v2.

Give the runtime the same one-second precipitation samples used for
[surface wetness](SURFACE_WETNESS.md#apply-rain-and-drying), through
`runtime.wetness.step` or compiled gameplay's `AdvanceSurfaceWetness`.
A catchment can use the clock without a `SurfaceWetness` receiver.
The summed batch depth in nanometres becomes water depth in each designated
cell on the first simulation tick. Existing source and drain rules run next,
then water flows across open faces. A batch advances the weather cursor by
its number of game seconds and uses the requested number of 60 Hz simulation
ticks. Identical doses produce the same total supplied volume at the same
catchment area. Drain and flow results follow the actual simulation cadence.

The authored list defines which cells collect direct precipitation. Adjacent
cells can receive runoff across open faces. Film retention, drying, and
moving shelter affect drawable receivers. These settings do not affect
catchment supply.

Cook a separate bed to use water beside [compactable snow](TERRAIN.md#compact-snow-on-cooked-tiles).
The water grid rectangle and the snow lattice must have disjoint interiors
in XZ. They can share a boundary. The exported game reports the combined
requirement as `needs_snow_water`.

## Author surface ripples

Use Poima 0.0.374 or later. Add `surface_ripples` to the plan and cook request
to change the shading normals of wet cells. All four fields are required when
selected.

```json
{"surface_ripples":{"wavelength_m":0.75,"slope":0.14,"period_ticks":60,"heading_degrees":30}}
```

Choose a wavelength from 0.10 to 8 metres, a normal slope from 0 to 0.4,
an integer period from 8 to 3600 ticks, and a heading from −180 to 180 degrees.
The heading is zero along world +X and rotates toward +Z for positive angles.
Phase follows the committed simulation tick, so replay, pause and save restore
retain the same phase at the same tick. The solver still determines depths,
volume and the water mesh. The terrain retains its authored collision.
A slope of zero keeps the original shading and retains the authored profile.
The renderer combines two directional waves and fades their slopes as the
projected wavelength approaches pixel scale. Forward and deferred lighting
use the resulting shading normals.

Plan, cook and `runtime.water.inspect` return the selected profile. It uses
`.pwater` v3, including when combined with a rain catchment. Omission preserves
the earlier region encodings. A different profile produces a different
region hash. Save restoration checks the region hash from the save.
Dependency and project inspection report
`needs_water_ripples` for selected profiles. Build and bundle verification
require a Vulkan runtime with `features.water_ripples`.

The Vulkan fixture checks normals, terrain collision and save continuation in
relocated builds. With a matching installed Windows runtime, run it from WSL.

```sh
python3 tests/water_ripples_capture.py build/installed/bin/poima.exe \
    --runtime-root build/installed --output build/ripple-captures \
    --windows-interop --lighting-path deferred
```

## Author directional wave layers

Use Poima 0.0.390 or later. Supply `surface_wave_layers` to the water plan
and cook methods when a surface needs independently moving wave directions.
This profile accepts one to four ordered layers with a combined slope of at
most 0.4. Choose one of `surface_wave_layers` and `surface_ripples`.

```json
{
  "surface_wave_layers": [
    {"wavelength_m": 1.8, "slope": 0.055, "period_ticks": 64, "heading_degrees": 22},
    {"wavelength_m": 0.94, "slope": 0.037, "period_ticks": 49, "heading_degrees": -39},
    {"wavelength_m": 0.57, "slope": 0.028, "period_ticks": 37, "heading_degrees": 75},
    {"wavelength_m": 0.31, "slope": 0.017, "period_ticks": 27, "heading_degrees": -108}
  ]
}
```

Each layer uses the [ripple parameter ranges](#author-surface-ripples).
Choose several wavelengths and headings to break up parallel highlights.
Each phase follows the committed tick modulo its own period. The renderer
filters each gradient at its projected pixel footprint, sums the gradients
and normalizes the result. This controls shading while the water grid
determines the visible and queried surface heights, volume and flow.

Cooked layered regions use `.pwater` v4. Layer order participates in the
content hash and is returned by `runtime.water.inspect`. Rain can coexist
with the profile. An all-zero selection gives the unlayered shading and
still uses the v4 package. Project and game inspection expose
`needs_water_wave_layers`, and export requires a simulation and Vulkan
runtime that advertises `features.water_wave_layers`.

The [layered water contract](../tests/water_wave_layers_contract.py) checks
the cooked package, native water state, save continuation, export admission
and Vulkan normals. Pass `--runtime-root`, `--windows-interop` from WSL, and
`--gpu` to exercise the standalone capture path. The forward and deferred
recipes use `--lighting-path`.

## Displace the water surface

Add `surface_swell` to the same region plan and cook request. This example
uses a grid with cells no larger than 1.1 metres.

```json
{
  "surface_swell": [
    {"wavelength_m": 5.4, "slope": 0.16, "period_ticks": 152, "heading_degrees": 32},
    {"wavelength_m": 4.4, "slope": 0.08, "period_ticks": 118, "heading_degrees": -25}
  ]
}
```

Enable `optics.follow_terrain`. Each of the one to four layers needs at least
four cells per wavelength. Wavelength ranges from 0.1 to 1,024 metres and a
period from 8 to 3,600 simulation ticks. Heading zero travels along world +X,
with positive angles turning toward +Z. Slope is positive and the sum is
at most 0.4. Each layer's amplitude is slope times wavelength divided by
`2π`. Their sum is bounded by half a cell or eight metres, whichever is lower.
Keep the optional, shorter `surface_wave_layers` or `surface_ripples` for
shading detail. Existing region plans can add swell through a new cook.

The engine samples the sine waves at the cell centres and shared corners
using the committed tick. Geometry and optical rays use the same piecewise
planar surface. The wetted shore stops where that surface meets the fitted bed.
An adjoining dry wedge needs a wet donor and an open face. The committed stage
must exceed the sill before that face can supply the wedge. Water exchange,
storage and rain accounting continue through the conserved grid.

`runtime.water.inspect` returns the height, geometric normal and submersion
for wet point samples. The `cells` rows and `volume_m3` describe the grid.
A listener at a fixed position can enter or leave the water as the swell
passes. Runtime and save restoration derive the wave phase from the saved
tick. Camera translation and projection leave that phase intact.

Single-sample Vulkan captures use the tilted surface for entry and exit,
refraction, shoreline foam and sprite attenuation. Caustics use the displaced
entry height and the local surface slope when the solver finds a stable light
path. Scene capture reports `water_optics.surface_swell` and the rendered
camera's submersion. The multisampled fallback draws the region's displaced
surface mesh. The region remains bounded to its authored footprint.
Ocean LOD and breaking-wave transport remain separate systems.

The selected field cooks `.pwater` version 11. Project and game inspection
report `needs_water_surface_swell`, and exporting requires
`features.water_surface_swell`. Omission retains the earlier artifact formats.
The [swell fixture](../tests/water_surface_swell_capture.py) compares
independent clipped wave prisms with rendered HDR and checks fresh-process
save continuation, spatial audio and export admission.

### Trace entry into water

Pass up to 64 `segments` to `runtime.water.inspect` with the current
`session_id` and `tick`. Each segment has `from_world` and `to_world`
endpoints in metres as `[x, y, z]` vectors within ±1,000 km of the origin.
The results follow request order. Each entry reports `fraction` from 0 to 1,
`position_m`, an upward-facing unit `normal`, the `cell` as `[x, z]`,
and `offshore`. A miss returns null. Offshore hits have a null cell.

Entry is measured toward the wet side of the top surface. This includes
descending onto flat water and crossing the sloped face of a swell. The
query visits only the crossed grid cells. It uses the fitted wedges, dry
bed clipping and saved wave phase for local water, and the fixed medium
plane offshore. Short waves used for shading leave the entry position intact.
Side entry through a grid wall and exit through the top are outside this
query. Use the existing point query to inspect current submersion.
[Uniform grid traversal reference](https://www.eecs.yorku.ca/~amana/research/grid.pdf)

World particle emitters can [use this contact](PARTICLES.md#splash-on-the-water-surface)
to stop drops and spawn splash effects.

### Float a dynamic box

Add `buoyancy` to a dynamic `BoxCollider` on a hierarchy root. This
example gives a one-metre cube a mass of 250 kg at unit scale. In calm
freshwater deep enough to float, it displaces about 0.25 m³ at equilibrium.

```json
{
  "motion": "dynamic",
  "half_extents": [0.5, 0.5, 0.5],
  "mass": 250,
  "friction": 0.6,
  "restitution": 0,
  "buoyancy": {"linear_drag": 0.6, "angular_drag": 0.2}
}
```

The water displacement calculation uses the scaled, rotated box and clips
it against the intersected water volumes. Dry cells, the fitted terrain bed,
local swell and the fixed offshore plane shape that volume. Poima applies
lift at the center of the submerged part, so an edge touching the water can
tip the body. The Jolt impulse uses freshwater density of 1,000 kg/m³ and
caps lift at 32 times the body's weight. Drag uses the submerged fraction
and the [local water velocity](#carry-floating-objects-with-a-current).
[Jolt buoyancy implementation](https://github.com/jrouwe/JoltPhysics/blob/036ea7b1d717b3e713ac9d8cbd47118fb9cd5d60/Jolt/Physics/Body/Body.cpp#L181)

Both drag coefficients accept 0–2 and default to 0.6 and 0.2. Use an empty
`buoyancy` object for both defaults. Mass must be at least 1 g. A world can
have up to 64 live buoyant bodies, including instances spawned from templates.
Dry boxes use normal rigid-body motion. Submerged boxes wake to receive
the impulse. Each physics step reads the water from the start of the tick.
The displaced volume is used for forces. The water balance remains owned
by the region solver.

Pass `bodies` with up to 64 buoyant entity IDs to `runtime.water.inspect`.
Supply the current `structure_revision` when runtime membership has changed.
The result reports `body_volume_m3`, `submerged_volume_m3`,
`center_of_buoyancy` in world metres or null when dry, and `lift_n`.
These values describe the current body pose and water state. A previously
applied impulse can differ if the body moved or the water changed during
the tick.

Export with a runtime advertising `features.water_buoyancy`. Run the
[buoyancy fixture](../tests/water_buoyancy_capture.py) for geometry, motion
and checkpoint continuation. Add `--gpu` for Vulkan images and
`--runtime-root` to check the relocated export.

### Carry floating objects with a current

Supply `currents` in the region plan and cook request. This example limits
the velocity to 1.2 m/s and uses a 2 cm minimum depth when reconstructing
velocity in a thin wet cell.

```json
{"max_speed_m_s": 1.2, "min_depth_m": 0.02}
```

The grid records the volume actually transferred across each cell face.
The completed tick's discharge provides a horizontal, depth-averaged velocity.
Poima interpolates between a cell's opposing faces and limits the velocity
using the committed depth and the profile. The normal flow at a sealed face
is zero. A sluice can change the current when its action changes openness.
The local field uses the accounted grid water. Dry points and the offshore
plane report a still-water velocity.
[Staggered-grid discharge reference](https://gmd.copernicus.org/articles/14/3577/2021/)

Buoyant boxes sample the volume-weighted velocity of the water they overlap.
Drag acts at the center of buoyancy against the body's relative velocity at
that point, so a stationary object can be carried by flow. The next physics
step uses the preceding committed water tick. Forces act on the body and the
regional solver retains ownership of all water transfers.

`max_speed_m_s` accepts 0.1–30 m/s. `min_depth_m` accepts 0.001–1 m.
The profile cooks `.pwater` version 16 and requires
`features.water_currents` for export. The saved transfer record restores the
current before the next physics step. Existing plans without a current
profile use the earlier still-water force path and snapshot formats.

`runtime.water.inspect` adds `water_velocity_m_s` to requested points and
buoyant bodies in a region with currents. Use `current_faces` to inspect up
to 64 interior faces as `{"cell":[1,0],"axis":"x"}`. Each result reports
`discharge_m3_s`, positive toward increasing X or Z. The
[counterflow fixture](../tests/water_currents_capture.py) compares both
directions with an independent water ledger and replays a relocated save.

### Leave wakes behind floating props

Supply `surface_wakes` in a water plan with `optics.follow_terrain: true`.
The profile below works with one-metre cells. A moving buoyant box adds a
crest in the direction of its motion through the water and a trough on
the other side of the transfer.

```json
{
  "speed_m_s": 5,
  "damping_per_second": 4,
  "max_amplitude_m": 0.1,
  "body_gain": 1
}
```

The wave field advances on the water tick. Open wet faces couple adjacent
cells and partially open gates attenuate the wave transfer. Closed or dry
seams anchor the affected corners. Prop sources use relative motion at the
center of buoyancy and scale by immersion and body size. Wakes propagate
after the source moves on and decay toward the underlying water surface.
The grid still owns the water volume ledger.
[Interactive water wave research](https://visualcomputing.ist.ac.at/publications/2018/WSW/)

Wave speed is 0.1–30 m/s with a limit of half a cell per tick. Damping
is 0.5–20 per second. Amplitude is 0.001–0.25 m and at most 12% of the
cell width. Combined wake and authored swell amplitude is limited to
half a cell or 8 m, whichever is smaller. Body gain is greater than zero
and at most one. A body can generate a wake when it is 1–99% submerged
and moves through the water faster than 0.05 m/s horizontally.

Read `runtime.water.inspect` at the committed tick. Selected cells
report `wake_height_m` and `wake_vertical_step_m`. Point submersion and
segment crossings include the displaced surface, which also drives
buoyancy and fitted-water rendering. `render_diagnostics.water_optics`
reports when the rendered region uses `surface_wakes`.

This profile cooks `.pwater` version 17 and saves both the height field
and its vertical increment. Export with `features.water_surface_wakes`.
The [wake fixture](../tests/water_wakes_capture.py) checks that a crest
crosses an optical probe while the underlying grid stage is below it,
then compares the field and captured frames after save restoration.

### Let props push water into adjacent cells

Add `body_displacement` when planning and cooking a local region. Keep
`buoyancy` on each dynamic box that should move water. This example lets
a body raise the regional water stage by up to ten centimetres.

```json
{"max_rise_m": 0.1}
```

At the start of a tick, Poima clips each participating box against the
committed water geometry. It accumulates the submerged volume in each
overlapped cell, converts it to an equivalent height and uses the higher
stage for the next transfer. The extra pressure can move water through an
open gate or leave a raised stage behind a sealed gate. Each outgoing face
is limited by the donor's water. Removing the box frees its occupied
space on the next tick.

`max_rise_m` accepts 0.01–4 m and at most twice the cell width. The
applied rise is also limited to one quarter of each cell's water-equivalent
depth to keep the explicit transfer stable. This is a bounded,
depth-averaged approximation to excluded volume. The solver tracks liquid
volume separately from the raised surface. Rain, wells and drains retain
their existing volume accounting.
[Two-way shallow-water interaction research](https://diglib.eg.org/items/c70c2671-ab7e-49ab-8136-7caf47ca7d59)

In `runtime.water.inspect`, a selected cell reports
`body_occupied_depth_m` after the authored cap and
`body_surface_rise_m` after the depth limit. Its `depth_m` and
`volume_m3` describe liquid. Points and segments use the resulting
geometric surface. Currents can report the signed flow generated by the
body, and wakes can add their separate displacement to the surface.

The profile cooks `.pwater` version 18 and saves the occupation field.
Export with `features.water_body_displacement`. The
[displacement fixture](../tests/water_body_displacement_capture.py)
checks a sealed sluice, a downstream waterline rise, backflow after
removal, and save continuation in the relocated build.

### Vary the swell across a coast

Set `swell_variation` alongside `surface_swell` in the plan and cook request.
This profile varies the crest positions and amplitude across the world:

```json
{
  "scale_m": 172,
  "strength": 0.75,
  "seed": 1597463007
}
```

The patch scale must be at least eight grid cells and lie in 0.1–65,536 m.
Strength is greater than zero and at most one. Seed is an integer from zero
through 4,294,967,295 and selects a reproducible field relative to the grid
origin. The cooker includes the variation in its conservative four-cells-per-
wavelength check and 0.4 combined slope bound. Longer swell wavelengths
leave more room for crest variation at coarse grid resolutions.

The fitted surface samples the field at its centres and corners. The
offshore shader evaluates the same field and filters the wave slope using
the resulting local wavelength. Spatial patches stay at fixed positions as
the committed wave phase advances. Save continuation restores that phase.
The offshore medium still uses the fixed plane for depth and crossings.

A selected profile cooks `.pwater` version 13. Inspect it in
`runtime.water.inspect` and `render_diagnostics.water_optics.swell_variation`.
Project dependencies report `needs_water_swell_variation`. Export with a
runtime that advertises `features.water_swell_variation`. The
[offshore fixture](../tests/water_offshore_capture.py) exercises the field
with `--swell-variation`, including a view across offshore patch boundaries.

## Continue the water offshore

Set `offshore` inside the terrain-fitted `optics` profile when the water
should continue beyond one side of the simulation grid. Choose the side that
faces open water and a stage at the same world height as the adjacent coast.

```json
{
  "edge": "min_z",
  "surface_height_m": 0,
  "depth_m": 80
}
```

`edge` accepts `min_z`, `max_x`, `max_z` or `min_x`. The offshore band
extends outward along that axis and across the other axis. It has a fixed
surface height and depth. The world height ranges from −1,000,000 to
1,000,000 m and the depth from 0.1 to 10,000 m. Keep terrain and opaque
geometry in front of the band wherever those surfaces should occlude the sea.

The fitted surface meets the offshore stage at its outer corners. Authored
swell continues to move the inner corners and centers. The simulation grid
keeps its original closed exterior faces and volume. Exchanges, rain, gates
and saves continue to account for the simulated cells. Offshore uses the same
extinction, scattering, refraction and authored wave normals as local water.
Foam fades toward the boundary. Geometric swell and focused caustics remain
within the local fit. Single-sample Vulkan color draws the offshore water,
including the FSR path. Multisample fallback draws the local surface mesh.

`runtime.water.inspect` reports offshore columns with `offshore:true`,
`cell:null` and a vertical surface normal. Listener audio uses depth through
the same boundary. A submitted color capture reports
`render_diagnostics.water_optics.offshore`.

With `surface_swell` authored, the reflection and refraction shaders use
the same analytic wave slope across the fitted water and the offshore plane.
Each band fades as its phase spans more of a projected screen pixel. Color
capture reports the number of active bands in
`render_diagnostics.water_optics.offshore_swell_layers`. The fitted triangles
and offshore plane still determine water depth, medium crossings and the
normal returned by point queries.

When the camera sees the water surface from air, height fog and aerial
perspective use the distance to the first water entry. This applies across
the join, including views with a visible seabed or a sky background.
[Multi-scale wave filtering research](https://onlinelibrary.wiley.com/doi/10.1111/cgf.70495)

The profile cooks `.pwater` version 12 and adds `needs_water_offshore` to
project dependencies. Use a runtime that advertises `features.water_offshore`
to export it. Run [the offshore fixture](../tests/water_offshore_capture.py)
with `--help` for scenario, capture and relocated-export options.

## Author the waterline sound transition

Discover `water_listener_audio` on an audio and simulation build. Add a
`listener_audio` object to the parameters for `world.water.plan` and
`world.water.cook`, then bind the reviewed hash as usual. For example:

```json
{"listener_audio":{"cutoff_hz":900,"gain":0.6,"full_depth_m":0.5}}
```

The digital low-pass cutoff accepts 80 to 8,000 Hz. Gain accepts 0 to 1.
Full blend depth accepts 0.01 to 10 metres. The three fields are required
together. A selected profile is stored in version 5 of the cooked region,
alongside any existing catchment and wave settings. Projects report
`needs_water_listener_audio` and export checks the corresponding runtime feature.
Regions without a profile retain their existing artifact format and audio mix.

The listener samples the committed column at its world position. The target
blend is zero at the surface and reaches one at the authored depth below it.
As the target changes, the 48 kHz stereo world mix crossfades between its
spatial output and the attenuated low-pass signal. A full-scale change takes
about 50 ms. Authored captures use the initial water levels. Frozen live
captures apply the selected tick's target from the first sample.

Compare `listener_water.target_blend` and `listener_water.submersion_m` in
`runtime.audio.inspect` against a point query at the same tick.
`runtime.audio.replay` records initial and final observations with the native
stereo mix. This is a listener-side effect. Sources still use the existing
direct paths and optional early reflections. Underwater-specific propagation,
cross-surface sound transmission and underwater optics need separate systems.
[Native audio controls](AUDIO.md)

### Observe the water column at a world position

Pass optional `points` to `runtime.water.inspect` alongside the session and
exact tick. Up to 64 finite world-space `[x,y,z]` positions are reported in
request order. Each row contains the original `point_world`, a `column` with
its cell index, bed height, surface height and depth, and `submersion_m`.
A point outside the region has a null column and zero submersion. A point
inside the footprint has zero submersion when the column is dry, when it is
on or above the surface, or when it is below that cell's terrain bed. At an
interior cell edge the higher-index cell owns the point. The normal-only
wave profiles leave these sampled heights unchanged. Selected surface swell
moves the point column's surface.

## Author transparent water

Add an optional `optics` profile to `world.water.plan` and
`world.water.cook`. Keep the terrain revision and plan hash guards. The two
RGB arrays are required together. Extinction is measured per metre in
`[0,8]`. In-scattered radiance is scene-linear and bounded to `[0,16]`.

```json
{"optics":{"extinction_per_m":[1.15,0.38,0.16],"scattered_radiance":[0.025,0.15,0.25]}}
```

The water becomes more opaque over longer sightlines. Higher red
extinction can produce the blue-green cast of clear water. Increase all
three channels for shorter visibility. The scattering color is the
radiance the view approaches as the path through water gets longer.
Choose it alongside the scene lighting and exposure.

Single-sample Vulkan color replaces the opaque water surface with the
world media compositor. It intersects the view ray with the committed
water columns and stops at opaque scene depth. It integrates RGB
transmittance and constant in-scattering over the intersected lengths.
The first visible top surface reflects according to its angle and a
water index of refraction of 1.333. Above water it reflects the
environment IBL cube, or ambient radiance when IBL is disabled. Existing
ripples and wave layers affect the reflected angle. From below, the
reflection uses the water scattering radiance and the Fresnel term permits
total internal reflection. Air-media effects apply after water in the
shared compositor. By default, transmission follows the original
sightline. Water-aware transparent particle ordering remains open.
[Grid traversal](https://par.cse.nsysu.edu.tw/resource/paper/2024/241022/A%20Fast%20Voxel%20Traversal%20Algorithm.pdf)
[Water optics reference](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-2-rendering-water-caustics)

Use one sample with forward or deferred rendering. FSR can consume the
composed color. MSAA and diagnostic views retain the opaque water mesh.
Observe `render_diagnostics.water_optics` for the active path, sampled
column count, camera submersion and shared media timing. Compare the
submersion to a `runtime.water.inspect` point query at the same tick.
Use preserved raw scene HDR and depth to distinguish water changes from
a change to the underlying opaque geometry. Authored optics select
version 6 of the cooked region. Project inspection, export and relocated
game validation require `features.water_optics`.

### Soften water reflections

Set `optics.reflection_roughness` to a perceptual roughness from 0 to 1.
With [environment IBL](ENVIRONMENT_IBL.md) enabled, the sky reflection
uses that roughness as its minimum. Ripples and offshore swell add
roughness as their slopes become too fine for the projected pixel footprint.
Each wave band contributes its unresolved mean-square slope. Poima adds
that contribution to the fourth power of the authored roughness and takes
the fourth root to select the existing GGX sky level and split-sum response.
The result is an isotropic approximation to the combined wave slopes.

Zero keeps the sharpest lobe where all wave detail is resolved. Omit the
field to retain the earlier reflection model. The rough reflection uses
the existing sky cube and applies to air-side interfaces and the optical
particle pass. A constant ambient reflection still works with IBL disabled.
An underwater view keeps its dielectric interface and total internal
reflection. Medium crossings and displaced surface depths follow the
committed water geometry.

This setting cooks `.pwater` version 14. Check
`render_diagnostics.water_optics.rough_reflection` for the authored value
and active state. Project dependencies report `needs_water_rough_reflection`
and export checks the corresponding runtime feature. The
[offshore fixture](../tests/water_offshore_capture.py) accepts
`--rough-reflection` with optional `--swell-variation`.
[Multi-scale ocean lighting](https://www-evasion.imag.fr/Publications/2010/BNH10/),
[Projected-space specular filtering](https://www.jcgt.org/published/0010/02/02/)

### Let scattered water light follow the sky

Set `optics.solar_scattering` to a strength from 0 to 1 when water should
dim as the scene's [solar lighting](WEATHER_CYCLE.md#drive-the-sun-and-sky)
changes. Set it to 1 to follow the full brightness change. Intermediate
values blend between the authored radiance and the sky-scaled radiance.
For example, a strength of 0.75 and a committed sky gain of 0.2 scale each
`scattered_radiance` channel by 0.4. Until gameplay commits a sky sample,
the renderer uses a gain of 1.

The compositor supplies the same effective scene-linear radiance to
fitted and offshore water, views from below the surface and water-aware
sprites. The strength acts on the medium's scattered-light term. Extinction,
Fresnel reflection, surface geometry, caustics and foam use their own controls.
This is an artist-controlled approximation to changing incident light.

An omitted or zero strength cooks the existing artifact bytes. A positive
strength cooks `.pwater` version 15. Project and bundle inspection report
`needs_water_solar_scattering`, and export requires a runtime with
`features.water_solar_scattering`. The existing water-optics fallback
conditions apply. Check `render_diagnostics.water_optics.active` to confirm
optical composition and its `solar_scattering` object for the authored
strength, committed sky gain and selected radiance scale.

Run the [solar scattering fixture](../tests/water_solar_scattering_capture.py)
with a simulation build to check authoring, legacy cooking and save
continuation. With Windows Vulkan, a compiled solar fixture and
`--gpu GPU_INDEX`, it checks HDR above and below water. Supply
`--runtime-root` to exercise the relocated build and older-runtime rejection.
Use `--solar-native-descriptor` on the
[optical sprite fixture](../tests/water_sprite_capture.py) to check blending
through the dusk medium.

### Refract the view across the surface

Add `"refraction": true` inside the `optics` profile. This cooks version 7 of
the region and requires `features.water_refraction` for export. Omission and
`false` cook the same version 6 bytes.

From above or below, the renderer bends the transmitted ray at the first
visible surface using Snell's law and the current ripple normal. It traces
up to 12 metres from that interface against camera-visible opaque depth.
A depth hit must have a consistent bracket, lie on the transmitted side of
the surface, and stay in the corresponding medium along the bent path.
RGB absorption follows the new length through water. Foreground
occluders, depth breaks, missing depth and off-screen rays use the
original sightline. A short fade at the image edge smooths that fallback.
Total internal reflection uses the existing reflected color.
[Screen-space depth tracing](https://jcgt.org/published/0003/04/04/paper.pdf)

`render_diagnostics.water_optics.refraction` is present on submitted
views with the effect active. Compare two captures at the same tick,
one with the option enabled and one without. Use raw HDR and depth to
confirm that a foreground object stays in front of the water, then
compare resolved HDR to judge the visible displacement.

### Focus sunlight underwater

Set `"caustics_strength": 0.9` inside `optics`. The range is `[0,1]`.
Omission and zero produce the same artifact. A positive strength cooks
version 8 and requires `features.water_caustics` for export. The same
profile can also enable refraction.

Set `LightingEnvironment.sky.sun` to the directional light that
illuminates the water. The light needs positive intensity and a direction
above the horizon cutoff. Author ripples or wave layers with positive
slope. Their phase advances on the committed water clock and restores
with it. The renderer reports the active path or an inactive reason in
`render_diagnostics.water_optics.caustics`.

The lighting pass bends incoming sunlight at the surface and estimates
how much the neighboring rays converge at each submerged opaque
receiver. The signed change in the sun's contribution is bounded and
experiences RGB attenuation along the light's water path. Ordinary surface
shadows remain in effect. A shadow at the calculated water entry also
suppresses the pattern. Cloud shadows attenuate the linked sun when
enabled.

The approximation searches around one solution seeded by a flat water
surface. It fades in near the surface, fades beyond 3 metres of depth,
and stops at 8 metres. Near-horizon light, unstable solutions and a
large height change between the receiver and entry column fade or fall
back. Strong wave folds can send several light paths to one receiver.
This stage captures the local contribution and uses the authored
strength to control it. It runs in the forward and deferred lighting
paths when single-sample water optics are active, including with FSR.
The refracted image can see the lit geometry. Underwater volumetric
shafts and multiple light paths through the waves remain open.
[Water caustics reference](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-2-rendering-water-caustics)

For a diagnostic, capture `raw_hdr` at the same receiver with strength
zero and positive. Compare two committed phases. Test a dry receiver, a
zero-slope control and a shadow at the water entry point. Inspect
`resolved_hdr` or the BMP to review the effect through depth color and
refraction.

### Add shoreline foam

Set `shore_foam` inside `optics` when planning and cooking a water region.

```json
{"shore_foam":{"depth_m":0.65,"opacity":0.92}}
```

Choose a vertical depth limit between 0.02 and 3 metres and opacity from
0 to 1. Both fields are required. Zero opacity keeps the profile with no
visual effect. Omitting the profile retains the original artifact rules.
A selected profile cooks `.pwater` version 9. Dependency and standalone
inspection report `needs_water_shore_foam`. Export requires an installed
runtime with `features.water_shore_foam`.

The renderer samples the committed water interface and interpolates the
terrain bed between the grid's cell centres. Foam fades as this depth
approaches the limit. An original world-space pattern moves in bands
along the shallow bed. It uses a four-second cycle from the committed tick,
and small features fade where the camera cannot resolve them. Geometry in
front of the surface occludes foam. Ambient light and the linked directional
sun illuminate it, including ordinary geometry shadows. The profile controls
a surface effect on local water in single-sample forward and deferred color
views, including with FSR. Spray, wakes, breaking surf, cloud shadows on foam
and foam as seen from below the water remain open.

Inspect authored values in `runtime.water.inspect` and the selected
rendering path in `render_diagnostics.water_optics.shore_foam`. Compare
`resolved_hdr` or the presented image at a shallow receiver, a deep point
and a point outside the water. Capture two committed phases and restore
the later phase from a save. The independent [Vulkan fixture](../tests/water_shore_foam_capture.py)
checks those paths and shading under an isolated shadow.

### Fit the shoreline to the terrain

Set `"follow_terrain":true` inside `optics` to cook `.pwater` version 10.
The cooker samples the reviewed heightfield at the grid corners as well as
the cell centres. Four triangles connect each centre to its edges. Water
ends where the committed level meets the fitted bed. Choose a cell size
that resolves the important changes in slope. The optical bed approximates
terrain detail between those samples.

At a wet cell the surface stays at its committed level. On a dry cell an
adjacent wet cell can extend the surface only into the triangle facing their
shared edge. The bed at the dry centre must be at or above the water level,
part of the edge must be below that level, and the face must be open with
its sill below the surface. Depressions fill through the existing solver.
The shoreline follows changes in the water level and gates.

Fitted geometry drives scene attenuation, refraction, caustics, foam and the
opaque fallback surface. `runtime.water.inspect` uses it for `points`,
and listener audio uses the same submersion. `cells`, `volume_m3` and the
flow ledger continue to report the conserved grid state. Saves retain the
gate controls and solved depths and derive the fitted surface on restore.
`render_diagnostics.water_optics.follow_terrain` reports the selected
profile. Export advertises `needs_water_terrain_fit` and requires a runtime
with `features.water_terrain_fit`. Omission and explicit false select
the same earlier artifact.

The [terrain fit fixture](../tests/water_terrain_fit_capture.py) checks an
affine bed contour, an opening gate, a blocking sill, unchanged cell volumes
and dry/submerged point audio. It also checks a relocated save with the
authored project unavailable.

## Use translucent particles in water

Enable water optics and author ordinary [translucent sprites](PARTICLES.md#see-particles-through-water).
The water and particle passes read the same committed bed and stages.
Particles retain their saved motion, opacity and lifetime as they pass
through the surface. Water filters each card according to its visible
camera path.

## Observe and verify local flow

Discover `runtime.water.inspect` in a simulation build. Pass `session_id`, the
exact `tick`, and up to 64 `cells` as `[x,z]` pairs. An observation reports the
plan SHA-256, sampled depths and surface heights, face controls, and the latest
tick's volume receipt. A catchment also reports its sorted `rain.cells`,
`rain.cumulative_depth_nm`, `rain.cumulative_m3`, and `last_step.rain_m3`.
The cumulative depth counts across the shared weather clock. Multiply it
by 10⁻⁹, the selected cell count and the area of each cell in square metres
to obtain cubic metres supplied. Check that the latest volume equals the
previous volume plus authored supply and rain supply minus the drain, within
floating-point tolerance. Check a dry cell beyond the closed sluice, replay a
`pressed:true` named input on the controller, and check for downstream depth.
The same read is available to a [standalone verification suite](GAME_VERIFICATION.md).

The fixed tick owns water state. A failed batch restores depths, gate settings
and receipts. Durable saves store the state under the frozen region identity.
Fresh-process loads verify the identity and restore the water tick before more
input. Rain-enabled saves include the water depths and cumulative supply
ledger, whose bounds depend on the saved weather cursor and simulation tick.
The package includes the selected `.pwater` and cooked terrain meshes.
Keep the editable heightfield recipe for later revisions.

The default renderer projects wet cells into one opaque, depth-tested mesh.
From 0.0.355, changing geometry uploads through frame-owned vertex and index
buffers. The depth-color palette stays bound across changes in cell heights.
Compare captures taken before and after opening a gate from a view that
exposes the downstream bed. Use [the draw report](RENDER_DIAGNOSTICS.md#submission-and-presentation-lifetime)
to check the water and particle payloads together. Terrain can obscure shallow
edges where its slope rises above the local water surface. Wave momentum,
vertical stacking, buoyancy, optical effects and large water bodies are
separate work.

Run the integrated checks from a built simulation checkout.

```sh
ctest --test-dir build/linux-game -R '^(shallow_water_native|water_region_native|water_region_scene_native|world_water(_rain)?_contract)$' --output-on-failure
```

From WSL, use an installed Windows runtime of the same build for capture and
standalone verification. Repeat with each available GPU index.

```sh
python3 tests/world_water_contract.py "$runtime/bin/poima.exe" \
  --windows-interop --runtime-root "$runtime" --gpu 0 \
  --capture-output build/water-captures
```

To check authored wet and dry surfaces without starting simulation, run the
focused capture fixture. It compares rendered depth and the projected wet
footprint, and verifies that each capture preserves the authored files.

```sh
python3 tests/water_authored_capture.py "$runtime/bin/poima.exe" \
  --windows-interop --gpu 0 --output build/authored-water-captures
```

[Authored water qualification](evidence/m2-authored-water-399.json).
[Water qualification](evidence/m2-terrain-water-353.json).
[Combined water and particle replay](evidence/m2-shared-water-particle-stream-355.json).

## Build a basin

Each grid has 1 to 256 square cells on either axis. Store cells from west to
east in each row, then advance along world Z. Heights and depths use meters,
with world Y up. Pass one floor height per cell. The optional initial depth
array uses the same order and defaults to zero throughout.

```cpp
const std::array floors{0.0, 0.0, 0.0};
const std::array initial_depths{1.0, 0.0, 0.0};
poima::water::Grid basin({.cells_x = 3, .cells_z = 1}, floors, initial_depths);
basin.set_face({0, 0}, poima::water::FaceAxis::x,
               {.openness = 0.0, .sill_height_m = 0.25});
basin.step();
basin.set_face({0, 0}, poima::water::FaceAxis::x,
               {.openness = 1.0, .sill_height_m = 0.25});
const auto receipt = basin.step();
const auto downstream = basin.sample({1, 0});
```

The first tick leaves the stored depth intact. The second sends 0.075 m³ to
the center cell. A face is addressed from the lower coordinate along its
axis. Its openness ranges from 0 for a closed passage to 1 for fully open.
Its sill is an absolute world height. The effective sill is the highest of
the two floors and the supplied sill. Faces along the outside of the grid
are closed. A raised floor or sill keeps water with a lower surface on its
original side.

`sample_world` finds the containing cell in the half-open X/Z footprint.
The sample contains the floor, depth, volume and surface height. The surface
of a dry sample equals the floor, and its depth is zero. Callers can use these
values to place water surfaces or drive depth-based gameplay.

## Advance and restore the grid

Pass a cell exchange only on ticks where a source or drain should run. Rates
use m³/s. A drain can remove the water supplied to its cell on the same tick,
then removes up to the available stored volume. Water arriving across faces
is available to drains on the next tick. Each cell may appear once per tick.
The receipt reports the committed tick and the volumes before and after the
step, supplied and drained. In exact arithmetic, after equals before plus
supplied minus drained. Use a floating-point tolerance when reconciling
observations.

The solver computes all face transfers against the same post-exchange
depths, then applies their signed contributions. The transferable head on
each side is its water depth above the effective sill, clamped to zero.
The head difference times openness and the configured rate produces the
transfer. At 60 Hz, the maximum rate of 12 s⁻¹ moves at most one fifth of
a cell's depth through any one face per tick. An interior donor with four
outgoing faces therefore retains water before receiving incoming flow.
Changing cell spacing changes how far this approximation moves water per
tick. Tune the rate for the chosen resolution.

Cell size ranges from 0.01 to 256 m. Horizontal origins, authored floors
and sills support magnitudes through 1,000,000 m. Supported depths range
from zero to 1,000,000 m, and source and drain rates range from zero to
1,000,000 m³/s. A step that would exceed the supported depth throws
`std::overflow_error` before publishing its state. Invalid shapes, values
and duplicate exchanges also leave the grid intact.

`snapshot` captures the grid configuration, tick, every floor and depth
and both face arrays. `Grid::restore` validates the materialized snapshot.
X face rows hold one fewer value than cell rows. Z faces have one fewer
row. Continue with the same future exchanges and gate changes to reproduce
the result on the same build and floating-point environment. The higher-level
`water::Region` owns authored exchanges, press-driven gate controls and the
versioned snapshot used by the native runtime.

## Check the result

The focused native fixture checks flood routing across an opened dam,
stationary water on uneven terrain, supply and drain receipts, unchanged
state after rejected steps, and exact replay after snapshot restoration.
It also advances a wet and dry slope for 1,024 ticks and measures total
volume. Build that fixture directly with a C++20 compiler.

```sh
water_test_dir=$(mktemp -d)
c++ -std=c++20 -O2 -Iinclude src/shallow_water.cpp tests/shallow_water_native.cpp -o "$water_test_dir/shallow-water"
"$water_test_dir/shallow-water"
```

Adjacent heightfield columns and face-local transport follow the setup used
in virtual-pipe research. Poima's present rate rule is a synchronous
head-relaxation approximation. Recent research includes multiple vertical
layers for blocked flow and a separate surface-wave model for deep water.
[Mei, Decaudin and Hu](https://nlpr.ia.ac.cn/2007papers/gjhy/116.pdf),
[Nilles et al.](https://diglib.eg.org/bitstream/handle/10.2312/vmv20241211/vmv20241211.pdf),
[Jeschke and Wojtan](https://visualcomputing.ist.ac.at/publications/2023/GSWSDSW/)
