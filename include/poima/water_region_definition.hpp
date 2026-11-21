// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/shallow_water.hpp"

#include <string>

namespace poima::water {

inline constexpr std::uint32_t max_region_side = 32;
inline constexpr std::size_t max_region_faces = 128;
inline constexpr std::size_t max_region_exchanges = 128;

struct RegionFace {
    Cell cell;
    FaceAxis axis;
    FaceControl initial;
    std::string action;
    double action_openness = 1;
};

struct RegionDefinition {
    GridConfig config;
    std::vector<double> floor_heights_m;
    std::vector<double> initial_depths_m;
    std::vector<RegionFace> faces;
    std::vector<CellExchange> exchanges;
    std::string plan_sha256;
};

}
