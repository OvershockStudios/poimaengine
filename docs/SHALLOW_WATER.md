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

One region supports up to 32×32 cells, 128 authored faces and 128 exchange
cells. Omitted depths start dry. Interior faces default to open, so close the
other faces of a dam to prevent bypassing a sluice. Sills use absolute world Y.
Named input presses flip each matching face between its initial and action
openness. Held inputs leave the gate in place. One press can operate several
faces. Authored source and drain rates apply each committed 60 Hz tick.

## Observe and verify local flow

Discover `runtime.water.inspect` in a simulation build. Pass `session_id`, the
exact `tick`, and up to 64 `cells` as `[x,z]` pairs. An observation reports the
plan SHA-256, sampled depths and surface heights, face controls, and the latest
tick's volume receipt. Check a dry cell beyond the closed sluice, replay a
`pressed:true` named input on the controller, and check for downstream depth.
The same read is available to a [standalone verification suite](GAME_VERIFICATION.md).

The fixed tick owns water state. A failed batch restores depths, gate settings
and receipts. Durable saves store the state under the frozen region identity.
Fresh-process loads verify the identity and restore the water tick before more
input. The package includes the selected `.pwater` and cooked terrain meshes.
Keep the editable heightfield recipe for later revisions.

The renderer projects wet cells into one opaque, depth-tested mesh. Compare
captures taken before and after opening a gate from a view that exposes the
downstream bed. Terrain can obscure the shallow edges of cells where its slope
rises above the local water surface. Wave momentum, vertical stacking,
buoyancy, optical effects and large water bodies are separate work.

Run the integrated checks from a built simulation checkout.

```sh
ctest --test-dir build/linux-game -R '^(shallow_water_native|water_region_native|water_region_scene_native|world_water_contract)$' --output-on-failure
```

From WSL, use an installed Windows runtime of the same build for capture and
standalone verification. Repeat with each available GPU index.

```sh
python3 tests/world_water_contract.py "$runtime/bin/poima.exe" \
  --windows-interop --runtime-root "$runtime" --gpu 0 \
  --capture-output build/water-captures
```

[Qualification results](evidence/m2-terrain-water-353.json)

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
