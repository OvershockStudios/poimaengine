# Local water volume

[The native water grid](../include/poima/shallow_water.hpp) moves water through
bounded basins, channels and floodable ground. A caller supplies floor heights,
opens or blocks passages, adds or drains water at chosen cells, and advances
fixed steps. The grid uses a single layer of overdamped flow. Wave momentum,
vertical stacking, buoyancy and presentation belong to later integrations.

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
the result on the same build and floating-point environment. The native
snapshot schema is an in-memory contract. Project packaging and disk
persistence need a caller integration.

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
