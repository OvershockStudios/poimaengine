// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cmath>

namespace poima {
enum class WaterFlowDirection { Forward, Reverse, Both };

inline double selected_water_flow(double signed_flow, WaterFlowDirection direction) {
    if (direction==WaterFlowDirection::Both) return std::abs(signed_flow);
    return std::max(0.,direction==WaterFlowDirection::Reverse ? -signed_flow : signed_flow);
}
}
