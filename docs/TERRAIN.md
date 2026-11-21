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
project for further editing. Streaming, deformation and automatic terrain LOD
are separate work.

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
