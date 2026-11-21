<!-- SPDX-License-Identifier: Apache-2.0 -->
# Foliage scatter

`poima::foliage::scatter` evaluates where instances of existing foliage assets
should sit on a supplied surface. The CPU authoring API works without a render
device and returns placements plus counts explaining why other cells were
rejected.

## Sample a terrain patch

Use a metre-based, Y-up grid. The caller supplies the terrain height and the
geometric unit normal's Y component at the chosen XZ position. The callback can
return an empty sample where a painted mask, path or structure excludes foliage.

```cpp
#include "poima/foliage.hpp"
#include "poima/terrain.hpp"

#include <algorithm>
#include <array>

poima::foliage::ScatterResult sample_foliage_patch() {
    const poima::terrain::Heightfield field(
        {.samples_x = 2, .samples_z = 2, .spacing = {16, 16}},
        std::array{0.0f, 0.5f, 0.25f, 1.0f});
    const poima::foliage::ScatterArea area{.id = 0x53, .columns = 16, .rows = 16};
    const poima::foliage::ScatterRecipe recipe{
        .seed = 918, .density = 0.65, .minimum_spacing_m = 0.6};
    const std::array species{
        poima::foliage::Species{.id = 10, .weight = 4, .variation_count = 3,
                                .maximum_slope_degrees = 30,
                                .minimum_scale = 0.8, .maximum_scale = 1.2},
        poima::foliage::Species{.id = 20, .maximum_slope_degrees = 20},
    };
    return poima::foliage::scatter(area, recipe, species,
        [&](double x, double z) -> std::optional<poima::foliage::SurfaceSample> {
            const auto point = field.sample(x, z);
            if (!point) return std::nullopt;
            return poima::foliage::SurfaceSample{
                point->height, std::clamp(point->geometric_normal[1], 0.0, 1.0)};
        });
}
```

The recipe evaluates one jittered candidate per cell. Density is the
probability of evaluating that candidate. Integer weights pick a species
before its slope range is evaluated. A rejected species leaves the cell empty.
Spacing applies to all accepted species within a patch in the horizontal XZ
plane. Seeded priority decides which near neighbors survive. The final order is
by row, then column. The counts cover every cell and identify density cuts,
surface exclusions, slope exclusions and spacing exclusions separately.

The same seed, patch, configuration and sampler repeat on the same numeric
platform and build. Species input order does not affect the result. Boundary
decisions depend on floating-point height, slope and spacing calculations.

## Author scene instances

Reserve a unique nonzero 64-bit namespace for each scatter patch within a world.
Use `poima::foliage::entity_id(instance.id)` to obtain a 32-digit hexadecimal
entity ID from the namespace, row and column. Seeds and species choices never
change the ID assigned to a cell. Preserve those IDs when applying an edit to
the same patch, and resolve collisions with unrelated authored IDs before
applying a transaction. Use a distinct namespace when creating a separate patch.

Map the returned species ID and variation index to your approved mesh or prefab
choice. Use `instance.position_m` directly on a root entity. A Y-up quaternion
for its yaw is `[0, sin(yaw/2), 0, cos(yaw/2)]`. Set all three scale axes to
`instance.uniform_scale`. Apply the transforms and bindings using revision-
guarded [world transactions](WORLD_SERVICE.md#iteration-conflicts-and-retries)
within the service's operation and world size limits. Entity IDs retired by
deletion require restoration through undo history if the same ID should return.

This placement module supplies authoring inputs. Mesh choices, scene commits,
patch reconciliation, cross-patch spacing, vegetation rendering, wind,
collision and navigation remain caller or subsystem responsibilities.

## Limits and checks

- One to 65,536 cells and one to 64 species per patch. Grid cell size is 0.01
  to 10,000 metres. Grid bounds and sampled heights stay within ±1,000,000 metres.
- Density is in [0, 1]. Spacing is either disabled at zero or between 0.001
  and 2,000,000 metres. The normal-up value is in [0, 1].
- Species IDs are unique and nonzero. Weight is between 1 and 1,000,000.
  Variation counts are between 1 and 256. Slope and uniform scale ranges are
  inclusive and must stay within 0–90 degrees and 0.001–100 respectively.
- Invalid recipes or supplied samples raise `std::invalid_argument` before a
  result is returned. Nothing is committed to a world during evaluation.

Run the native unit check without a renderer build.

```sh
test_dir=$(mktemp -d)
c++ -std=c++20 -Wall -Wextra -Wconversion -Werror -pedantic -O2 -Iinclude \
  src/foliage.cpp tests/foliage_scatter_native.cpp -o "$test_dir/foliage_scatter"
"$test_dir/foliage_scatter"
```

The test covers deterministic variation and stable IDs, analytic terrain
contact, exclusions and slope limits, independent pairwise spacing checks and
input bounds.
