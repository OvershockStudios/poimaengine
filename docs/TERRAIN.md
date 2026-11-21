# Native terrain geometry

[`poima::terrain::Heightfield`](../include/poima/terrain.hpp) builds indexed
terrain tiles and samples their surface. It takes one rectangular grid of
heights and owns the sample data. Use the same heightfield for both tile
generation and queries.

## Define a heightfield

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

## Build tiles and query the surface

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
