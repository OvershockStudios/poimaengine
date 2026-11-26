# Native terrain geometry

## Author terrain in a world

Create a `poima.terrain.heightfield` v1 recipe. This example makes a sloped
tile two metres wide and two metres deep. Heights are offsets from the origin.

```json
{
  "format": "poima.terrain.heightfield",
  "version": 1,
  "samples_x": 2,
  "samples_z": 2,
  "tile_cells": 1,
  "origin_m": [0, 0, 0],
  "spacing_m": [2, 2],
  "heights_m": [0, 0.5, 0, 0.5]
}
```

1. Call `world.terrain.inspect` with `source` pointing to the `.json` file,
   or pass the JSON text as `recipe`. The result lists the canonical
   `recipe_sha256`, ordered tiles, codec asset IDs, primitive indices and bounds.
2. Call `world.terrain.cook` with the same input and `expected_recipe_sha256`
   from the preview. A changed recipe returns a conflict before publication.
   A successful cook publishes standard `.pmodel` files in the world asset
   store. An exact retry reuses those files.
3. Use `world.transact` at the current revision to create an entity for each
   tile. Leave the transform at identity because the vertices are in world
   space. Give `StaticMesh` and `MeshCollider` the same `asset` and `primitive`
   from the tile. Set `StaticMesh.visible` to `true` and supply the collider's
   friction and restitution.
4. Read `world.dependencies` to check the selected tile hashes. Capture the
   surface with `world.capture` and raycast from above with `runtime.raycast`.
   Check an interior point on each tile and a point on the shared edge.

`source` is relative to the world file's directory or absolute in an unrestricted
authoring session. A scoped MCP draft accepts paths relative to that directory.
Use a source file for recipes approaching the shared session's 1 MiB request limit. Both calls
compute the recipe synchronously. They leave authored entities and the revision
unchanged. If publication stops partway through a cook, retry with the same
reviewed digest before binding the tiles. Successfully written packages are
immutable and can be reused.

The cooker accepts at most 4 MiB of source text, 131,072 height samples, 64
tiles, 250,000 triangles and 32 MiB of encoded models. The generated meshes also
pass the static collider geometry admission checks. Export includes referenced
tiles through the ordinary model dependency closure, so the shipped game can
load the surface without the source recipe. Save the recipe in the source
project for further editing. The [snow binding](#compact-snow-on-cooked-tiles)
adds bounded deformation to a pair of cooked tiles.

Use the reviewed recipe for [grounded foliage placement](FOLIAGE_SCATTER.md)
and the bound collider geometry for [navigation baking](NAVIGATION.md).
After replacing tile geometry, bake and bind a new navigation asset before
starting gameplay or exporting.

The world contract checks guarded publication, render and collider asset
references, project relocation and a fresh-process save continuation. The
Windows Vulkan run also compares scene captures against the projected surface,
checks depth at both sides of the seam and replays a relocated standalone build.
[Qualification results](evidence/m2-terrain-authoring-351.json)

Run the world and native contracts from a built simulation checkout.

```sh
ctest --test-dir build/linux-game -R '^(terrain_native|terrain_asset_native|world_terrain_contract)$' --output-on-failure
```

From WSL, use an installed Windows runtime of the same build for the capture
and export checks. Repeat with each available GPU index.

```sh
python3 tests/world_terrain_contract.py "$runtime/bin/poima.exe" \
  --windows-interop --runtime-root "$runtime" --gpu 0 \
  --capture-output build/terrain-captures
```

## Cook terrain detail levels

Set `version` to `2` and add `lods` to the same heightfield recipe.
Each entry specifies an interior sampling stride and the camera distance
at which the tile may use it. Strides are increasing integers in 2–128
cells. Distances are increasing metres in `(0, 1000000]`. Supply one to
four entries.

```json
{
  "version": 2,
  "lods": [
    {"stride": 4, "min_distance_m": 24},
    {"stride": 8, "min_distance_m": 50}
  ]
}
```

The cooker preserves every border segment and its vertex positions, UVs,
normals and tangents. Each coarser tier samples fewer interior heights.
The transition triangles join coarse interior cells to the full border.
Neighboring tiles can select different tiers and retain the same shared
edge, including the last partial tile. A requested tier appears in the
tile only when it reduces its triangle count. Interior heights can change
when the camera crosses a threshold.

Use each v2 result's `static_mesh` as the complete `StaticMesh` value.
It contains the tile's model, a distance anchor at its bounds center,
and the chosen `lods` when the tile has lower tiers. `lod_tiers` lists the
stride, primitive, vertex and triangle counts of those alternates. Keep
the entity transform at identity. Make the `MeshCollider` from the tile's
top-level `asset` and `primitive`, which select the original full-resolution
grid. The size budget includes all primitives of all tiles.

Use matching [material height profiles](MATERIAL_DISPLACEMENT.md) on adjacent
tiles to preserve a shared displaced edge. The same camera distance chooses
the geometry for depth, shading and shadows. Displacement keeps its existing
patch budget. A mutable snow region uses its fixed resolution.

`tests/terrain_lod_capture.py` audits the cooked topology and collision ray.
The Windows trial checks mixed tiers across a camera move, then replays those
captures from a relocated standalone build.
[Qualification results](evidence/m2-terrain-lods-467.json)

## Compact snow on cooked tiles

Cook one heightfield with `samples_x: 65`, `samples_z: 33`, `tile_cells: 32`
and `spacing_m: [0.03125, 0.03125]`. Bind the resulting west and east tiles
as visible, opaque `StaticMesh` and matching `MeshCollider` components with
identity root transforms. Keep the grid within ±512 m, including the
substrate under the authored snow. Add this operation to `world.transact`:

```json
{
  "op": "snow.set",
  "definition": {
    "tiles": ["00000000000000000000000000000010", "00000000000000000000000000000011"],
    "thickness_units": 256
  }
}
```

Each unit is 1/4096 m. The example permits up to 6.25 cm of compaction.
The supported thickness is 1–512 units. Use `definition: null` to remove
the binding. From 0.0.379, snow and [baked shallow water](SHALLOW_WATER.md)
can share a world. Use distinct terrain entities for each binding. Their XZ
footprints may meet at a boundary. Interior overlap is rejected even when
every water cell is dry. Configure baked navigation in a world without a
snow binding. Use other receivers for material footprint stamps and
acoustic meshes.

The world dependency and project inspection results report `needs_snow_water`
for the combined scene. Export with an installed simulation runtime that
advertises `features.snow_water_regions`. Snow compaction and the water
solver save their state at the same committed tick. A failed step restores
both. Use separate requests for explicit snow stamps and rain doses.

Run [the combined terrain contract](../tests/snow_water_contract.py) against
a simulation build. Pass `--runtime-root` with an installed runtime to check
export and relocation. The [Vulkan fixture](../tests/snow_water_capture.py)
compares the snow surface, its shadows and the adjacent water across
compaction and a fresh-process save load.

The `snow` section of `world.describe` reports the lattice dimensions,
physical units, and supported mutation. Select `runtime.snow.step` to
discover its full request schema.

Start the runtime and query `runtime.snow.inspect` at the committed tick and
structure revision. Read `generation` and use `runtime.raycast` to get the
tile ID and triangle under the intended stamp. Send `runtime.snow.step` with
the usual step guards and a required `snow_stamp` parameter. It accepts the
other step inputs in the same batch. This example uses a radius of 15.625 cm and
a maximum depth of 3.125 cm at the center:

```json
{
  "expected_generation": 0,
  "receiver": "00000000000000000000000000000010",
  "triangle": 1054,
  "ray": {"origin": [0.48, 2, 0.51], "direction": [0, -1, 0], "distance": 3},
  "radius_units": 640,
  "depth_units": 128
}
```

Use the triangle from your actual raycast. The ray must first hit the
expected upward-facing triangle. The engine rounds X and Z to the closest
compaction unit, applies a radial falloff proportional to `1 − distance² / radius²`,
and clamps each sample at the substrate. Radius accepts 128–1536 units and
depth accepts 1–512 units. One stamp applies before physics and gameplay on
the first tick of the requested batch. A stamp that changes a held foot's
triangle or its immediately connected samples is refused. Script the stamp
after the foot lifts. A stamp that changes no samples only advances the tick.

Read up to 64 lattice coordinates with `samples: [[31, 16], [32, 16], [33, 16]]`
in `runtime.snow.inspect`. It reports base height, compacted height, and lost
volume. Each sample contributes its compaction times the number of touching
triangles, including the triangles on both sides of the seam. Multiply
`lost_volume_units` by the cell area and by 1/4096 m, then divide by six to
get cubic metres. The save stores the generation and nonzero sample depths.
The runtime reconstructs the exact derived mesh from the saved depths and
the immutable cooked tiles when opening a save. A failed step restores the
previous meshes, collision shapes and compaction together.

Vulkan uses the current tile vertices for both the visible surface and its
shadows. With reconstruction active, a visible tile uses motion from
corresponding vertices when the preceding accepted frame also drew it.
A tile that reappears establishes new correspondence. Inspect
`render_diagnostics.last_draws` for streamed geometry counts and byte totals.

Run the [snow contract](../tests/snow_compaction_contract.py) for guarded
edits, collision, rollback, and fresh save continuation:

```sh
python3 tests/snow_compaction_contract.py build/linux-game/poima \
  --headless --output build/snow-contract
```

For Windows Vulkan, supply the installed executable and add `--runtime-root`,
`--gpu`, and `--lighting-path`. Use `--windows-interop` when running from WSL.
The [shadow contract](../tests/snow_shadow_contract.py) compares light rays
and HDR samples before and after compaction, with shadow casting separately
enabled and disabled. The native `poima-reconstruction-quality-test` accepts
`OUTPUT_PREFIX GPU --mutable-snow-motion forward fsr3_native`. It also accepts
`deferred` and `fsr3_quality`.

### Add material height to snow

Apply the same [height profile](MATERIAL_DISPLACEMENT.md) to both tile
`StaticMesh` components in one guarded transaction. Use the `y` axis and a
linear height texture. The cooked positions give both tiles the same
projection origin. The camera and shadow draws use the height detail on
top of the current compaction lattice. Motion uses the prior lattice with
the same profile when a capture preserves renderer history. Collision,
gameplay queries and stored snow samples use the physical lattice.

Project and world inspection report `needs_snow_material_displacement`.
Export requires `features.snow_material_displacement`. The runtime uses
the base snow geometry when hardware support or the frame patch budget
precludes tessellation.

### Leave depressions with planted feet

Add a `foot_release` profile to the `snow.set` definition to use authored
[planted-foot locking](FOOT_PLANTING.md).

```json
{"radius_units": 512, "depth_units": 96, "sole_tolerance_m": 0.002}
```

This profile uses a 12.5 cm radius and allows up to 2.34 cm of additional
compaction per contact, subject to the snow thickness and radial falloff.
Use the same radius and depth bounds as a scripted stamp. The allowed sole
tolerance is 0.001–0.02 m. The runtime records a newly locked foot when its
solved sole meets the tolerance and the contact hits a bound snow triangle.

The footprint becomes ready on a confirmed lift. Starting with the next
tick, the runtime tries the oldest ready footprint whose compaction would
preserve every held foot's support triangle and immediately connected
samples. A safe footprint can proceed while another waits. An explicit
`runtime.snow.step` stamp gets the first tick's attempt, including a stamp
that leaves the geometry unchanged. A foot that loses its lock when its
slot is disabled or its rig is removed gives up the held footprint. A
footprint that is already ready remains queued if its source rig is removed.

Read `foot_release` in `runtime.snow.inspect` for holding and ready rows and
outcome counts. Each row identifies the rig, slot, acquisition tick, snow
tile, source triangle and quantized center. Ready rows also give the release
tick. The queue holds up to 64 planted contacts and 16 ready footprints. When
full, it keeps the existing ready footprints and counts new overflow. A
ready footprint with zero effective compaction is consumed as `saturated`.
An enabled region saves the queue and its outcome counts with the compacted
grid. Loading a save taken during a hold or after a lift preserves the same
next eligible footprint. Restoration checks the contact timeline against
the saved IK state for live rigs and the allocation history for retired IDs.

#### Shape the sole

Use `sole` in place of `radius_units` for a narrower heel and a wider toe.
This profile describes a boot about 28 cm long and 12.5 cm wide at the toe.

```json
{"sole":{"half_length_units":576,"half_width_units":256},"depth_units":96,"sole_tolerance_m":0.002}
```

The length and width are half extents in 1/4096 m units. The allowed length
is 256–1024 units and the width is 128–512 units. Their sum plus two units
must fit within 1536 units. The stamp samples the same cooked snow lattice
and uses the larger pressure from its two rounded lobes at each sample.

A shaped contact records the sole's heading in the tile's XZ plane when it
locks. In `runtime.snow.inspect`, shaped rows report `heading_bin` from
0 to 255, starting at +Z and increasing toward +X at 64. Turning after
lift leaves the queued heading intact. Shaped snow uses snapshot version 3.
Circular profiles use version 2, and regions without automatic footprints
use version 1.

Run the [plant and release contract](../tests/snow_foot_release_contract.py)
or the [shaped sole contract](../tests/snow_sole_shape_contract.py) with the
pinned Original Knight GLB and CC0 notice.

```sh
python3 tests/snow_foot_release_contract.py build/linux-game/poima \
  --source "$knight_glb" --license "$knight_notice" --output build/snow-sole-check
```

The [walking fixture](../tests/snow_sole_locomotion_contract.py) uses the same
arguments and plays the original walk at 1×. It follows a fitted kinematic
path through a quarter turn, then blends to idle. It compares the committed
soles, footprint geometry and save continuation. When using a new character,
fit the pace and mount height to its planted reach before evaluating prints.

The [circular](../tests/snow_foot_release_capture.py) and
[shaped](../tests/snow_sole_shape_capture.py) Vulkan contracts check the
visible terrain and actor in a relocated Windows export. Supply the installed
Windows runtime with `--runtime-root` and select the adapter and rendering
path with `--gpu` and `--lighting-path`.

## Generate and sample geometry from C++

[`poima::terrain::Heightfield`](../include/poima/terrain.hpp) builds indexed
terrain tiles and samples their surface. It takes one rectangular grid of
heights and owns the sample data. Use the same heightfield for both tile
generation and queries.

### Define a heightfield

Set `samples_x` and `samples_z` to the number of vertices on each axis. The
grid has one fewer cell than vertex on each axis. The height array starts with
the smallest Z row and visits consecutive X samples within each row. Poima
uses a right-handed, Y-up frame in metres. The height at a vertex is added to
`origin.y`. The first sample starts at `origin.x` and `origin.z`. The two
`spacing` values set the X and Z distances between samples.

```cpp
#include "poima/terrain.hpp"

const std::array<float, 4> heights{0, 0, 0, 4};
const poima::terrain::Heightfield field({2, 2, 1, {-4, 12, 7}, {2, 4}}, heights);
auto mesh = field.build_tile(0, 0);
const auto hit = field.sample(-3, 9);
// hit->height is 14 metres.
```

| Input | Bounds |
| --- | --- |
| Samples per axis | 2–4,097 |
| Total samples | At most 4,194,304, with exactly one height per vertex |
| Cells per tile axis | 1–128. The final tile on an axis can have fewer cells. |
| Horizontal spacing | Finite and at least 0.01 m |
| Origin and emitted positions | Finite and within ±1,000,000 m per axis |

Invalid source inputs throw `std::invalid_argument`. The constructor rounds
vertex positions once to the float representation in `MeshAsset`. It rejects
spacing that would collapse neighboring rounded X or Z coordinates. The
heightfield owns at most 16 MiB of height samples plus two coordinate arrays.

### Build tiles and query the surface

`tile_count_x()` and `tile_count_z()` give the valid range of zero-based tile
indices. `build_tile()` throws `std::out_of_range` for an invalid tile index.
It returns a static `MeshAsset` with X-fastest vertices, 32-bit indices,
continuous UVs and tangent frames. A full 128-cell tile contains 16,641
vertices and 32,768 triangles. Bind the mesh with an identity transform when
using its world-space positions in a `SceneObject`.

Each cell uses the diagonal from `(x, z)` to `(x + 1, z + 1)`. Its first
triangle runs from `(x, z)` to the diagonal end and then to the
corner towards +X. Its second triangle runs from `(x, z)` to the
corner towards +Z and then to the diagonal end. Both faces have upward
winding. The vertex normal is the normalized area-weighted sum of incident
faces in the whole heightfield, so independently built adjacent tiles have
bit-identical positions, normals, UVs and tangents on their shared edges.

`sample(x, z)` returns the world Y height in the selected triangle, its
geometric unit normal, and the normalized interpolation of the triangle's
mesh vertex normals. The shading normal describes the mesh before material
normal maps. The query uses the rounded positions emitted to the mesh.
On the diagonal it selects the first triangle. On an interior X or Z grid
line it selects the cell towards increasing coordinates. The outer extent
is inclusive. An out-of-bounds or nonfinite query returns `std::nullopt`.

The module supplies in-memory geometry and sampling. Callers choose which
tiles to generate and retain.

Run the focused test directly with a C++20 compiler from the repository root.

```sh
terrain_test=$(mktemp -d)
c++ -std=c++20 -O2 -Iinclude src/terrain.cpp tests/terrain_native.cpp -o "$terrain_test/terrain-native"
"$terrain_test/terrain-native"
```
