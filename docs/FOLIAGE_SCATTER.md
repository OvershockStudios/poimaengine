<!-- SPDX-License-Identifier: Apache-2.0 -->
# Foliage scatter

Place existing meshes such as stones, ferns and trees on a heightfield. The
world service returns positions grounded on the terrain triangles, together
with counts explaining which cells were skipped.

## Place foliage in a world

Use Poima 0.0.352 or later. Select `world.foliage.plan` in `world.describe`
or MCP `poima_discover` for the request schema. Cook and bind the terrain
using the [terrain guide](TERRAIN.md#author-terrain-in-a-world). Import a model
for each plant or rock variant. Select a static primitive whose local origin
sits at its intended ground contact. Direct `StaticMesh` bindings use the
primitive coordinates and material. Model node transforms are separate.

Call `world.foliage.plan` with parameters like these. Replace the two hash
placeholders with the reviewed terrain recipe digest and a cooked model ID.

```json
{
  "terrain": {"source": "terrain/coast.json"},
  "expected_terrain_recipe_sha256": "<recipe SHA-256 from world.terrain.inspect>",
  "scatter": {
    "patch_key": "53edc1ae3c4d0001",
    "seed": "0000000000000396",
    "origin_m": [4, 12],
    "cell_size_m": 0.75,
    "grid": [12, 8],
    "density": 0.65,
    "minimum_spacing_m": 0.6,
    "species": [{
      "id": 10,
      "slope_degrees": [0, 30],
      "uniform_scale": [0.8, 1.2],
      "variants": [{"asset": "<cooked model SHA-256>", "primitive": 0}]
    }],
    "exclude_rectangles_m": [[9, 15, 14, 19]],
    "suppressed_cells": [[5, 3]]
  },
  "limit": 32
}
```

Use the same terrain file and digest as the bound tile meshes. `terrain` also
accepts `recipe` with the inline JSON text within the request size limit. Draft
MCP connections resolve `.json` sources from the entry world's directory. The
plan samples the recipe and reports the current authored `revision`. Terrain
cooking also checks collider limits. Verify that the world binds the reviewed
tile identities with identity transforms before applying the plan.

Each placement gives a `key`, zero-based `[column, row]`, chosen species and
variant, and complete `transform` and `static_mesh` component values. The mesh
stays upright with a Y-up yaw and uniform scale. The lower edges of broad or
irregular meshes may need a placement adjustment beyond the single point
where the pivot meets the terrain. Inspect the plan counts to diagnose density,
manual suppression, rectangle exclusion, terrain bounds, slope and spacing.

Continue with the original input, `offset` set to the previous `next_offset`,
and `expected_plan_sha256` set to the previous `plan_sha256`. A null
`next_offset` marks the last page. A changed terrain or plan pin returns
`-32009` with the current digest. Inspect the new source and restart at offset
zero after confirming the change. A malformed recipe returns `-32602`. An
unavailable or unsuitable variant returns `-32050` with its asset and primitive.
Each page recomputes the
same bounded plan and validates the referenced packages.

Create root entities and set their `Transform` and `StaticMesh` in a
[guarded world transaction](WORLD_SERVICE.md#iteration-conflicts-and-retries)
using the reviewed revision. Allocate entity IDs separately from the logical
placement keys. Reuse the matching entity when revising an existing placement.
For a deleted entity, undo its removal or allocate a fresh ID. Keep the
scatter recipe and any key-to-entity mapping in the source project for later
edits. Changes larger than the transaction operation limit need multiple
guarded commits. The plan itself leaves the world unchanged.

Check the placed entities with `entity.world_transform` and use a downward
`runtime.raycast` against the terrain to compare the sampled height. On
Windows Vulkan, capture the placed meshes in the scene and play the relocated
export. The export uses the selected ordinary mesh references. It can render
the placements with the terrain and scatter recipes absent. A placed
`StaticMesh` is visual geometry. Add an explicit collider to plants or rocks
that need physical interaction or should affect a navigation bake.

The world planner supports up to 4,096 cells, 16 species, 32 variant entries,
32 rectangle exclusions, 256 cell suppressions and 64 rows per page. A new
plan admits up to 256 MiB of distinct cooked model packages. Spacing applies
across the selected patch. To change a path exclusion or suppress a cell,
recompute the whole patch, then compare the placement keys and components to
decide which edits to apply.

Run the native planner and world contract from a built simulation checkout.

```sh
ctest --test-dir build/linux-game -R '^(foliage_scatter_native|foliage_authoring_native|world_foliage_contract)$' --output-on-failure
```

From WSL, the same fixture can capture a relocated Windows standalone game
using a matching installed runtime.

```sh
python3 tests/world_foliage_contract.py "$runtime/bin/poima.exe" \
  --windows-interop --runtime-root "$runtime" --gpu 0 \
  --capture-output build/foliage-captures
```

[Qualification results](evidence/m2-foliage-authoring-352.json)

## Bend foliage with wind and character proximity

Add `foliage_motion` to an ordinary rigid `StaticMesh`. The two local heights
are measured in the model primitive's coordinates. Place the root at the
plant's ground contact and the tip at the height that should receive the
full offset. Use the same heights and pivot for every authored detail tier.

```json
{
  "root_y": 0,
  "tip_y": 0.8,
  "wind": {"direction": [1, 0.25], "tip_m": 0.15, "period_ticks": 120, "wavelength_m": 8},
  "touch": {"actor": "00000000000000000000000000000002", "radius_m": 0.85, "tip_m": 0.3}
}
```

Supply either profile or both. Root and tip must be in `[-1024, 1024]`
with a height difference of 0.05 to 128 local units. Wind direction is a
nonzero world XZ vector with coefficients in `[-1, 1]`. The period is an
integer from 2 to 3,600 committed ticks and the wavelength is 0.5 to 256
metres. Both tip amplitudes accept 0 to 2 metres. Touch has a radius from
0.05 to 8 metres and names a distinct authored character controller.

Wind travels across the world using the plant's root position and the
committed simulation tick. Touch uses the world-space feet and current
stance height of the selected character. It contributes when the stem and
character overlap vertically and the character is within the touch radius.
The bend follows a smooth local height curve. Vertices at or below the root
stay fixed. The renderer bends shading frames and shadow casters with
the same shape. Motion products use the preceding accepted pose. [Crytek
production notes on height-weighted bending](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-16-vegetation-procedural-animation-and-shading-crysis)

The authored preview samples tick zero. A runtime save restores the tick
and character state used to reproduce the pose. `runtime.foliage.inspect`
takes `session_id`, `tick`, `entity` and optional `structure_revision`.
It reports the local XZ and world-space tip offsets plus the touch weight.
Use the session and tick from the corresponding render capture. Runtime
queries use the frozen world even if authoring changes the profile later.

The bend is visual. Keep a rigid collider where the plant needs physical
contact. The character response follows the current pose continuously.
Spawn templates accept wind profiles. The touch target belongs to the
authored world. Camera-routed drawables, bound water tiles and deformable
snow tiles reject foliage motion.

Exports report `needs_foliage_motion` when the world or a spawn template
contains a profile. Build and bundle verification check
`features.foliage_motion` in the installed runtime descriptor.

Run `python3 tests/foliage_motion_contract.py POIMA --output DIRECTORY`
for the headless checks. Run
`python3 tests/foliage_motion_capture.py POIMA --runtime-root RUNTIME
--output DIRECTORY --gpu GPU_INDEX --lighting-path deferred` on an
installed Windows build for the relocated Vulkan checks. Use
`--lighting-path forward` for the forward path and `--windows-interop`
when invoking Windows from WSL.

## Light leaves from behind

Add `diffuse_transmission` to a leaf's [PBR material](MATERIAL_AUTHORING.md).
Set it to a fraction from 0 to 1, use `"metallic": 0` and
`"double_sided": true`. The base color tints the transmitted light. Use
`"alpha_mode": "mask"` with a base-color cutout image for leaf silhouettes.
Leave the field absent or zero for the original material response.

With a positive fraction, light behind the leaf contributes colored diffuse
light to the visible side. The same fraction reduces front-side diffuse
reflection. Specular reflection uses the ordinary dielectric response.
The shader checks shadow occluders on either side and uses the same
alpha-test coverage for camera and shadow depth. Sky image-based lighting
uses the diffuse irradiance from both hemispheres.
[Filament diffuse transmission](https://google.github.io/filament/main/materials.html)

This approximation uses the local shading normal, one fraction per material
and a single transmitted diffuse lobe. Keep surface layers on separate
materials. Cutout foliage continues to cast depth-based shadows.

Agents can read and revise the fraction through the authored material
or [live material editing](LIVE_MATERIALS.md). Project inspection reports
`needs_diffuse_transmission` and export requires a runtime advertising
`features.diffuse_transmission`. Set up a backlit and front-lit capture
with a visible shadow blocker and inspect matching pixels in the exported
game. [Qualification results](evidence/m2-diffuse-transmission-470.json)

## Sample a terrain patch from C++

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

## Use placements from the C++ API

Reserve a unique nonzero 64-bit namespace for each scatter patch within a world.
Use `poima::foliage::entity_id(instance.id)` to encode the logical key from
the namespace, row and column. Seeds and species choices preserve the key
assigned to a cell. Use a distinct namespace when creating a separate patch.

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
