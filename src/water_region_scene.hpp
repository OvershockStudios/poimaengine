// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace poima {
struct SceneSnapshot;

namespace water {
class Region;

// Append the region's current water surface to an immutable presentation.
// The region remains authoritative for water levels and gameplay queries.
void append_region_scene(const Region& region, SceneSnapshot& scene);
}
}
