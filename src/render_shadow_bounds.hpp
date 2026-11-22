// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "poima/assets.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <limits>

namespace poima::render_shadow_bounds {

using Row = std::array<double, 4>;

struct Plane {
    Row equation{};
    Row clip_magnitudes{};
};

struct Frustum {
    std::array<Plane, 6> planes;
    bool usable = true;
};

struct LocalFrustum {
    std::array<Plane, 6> planes;
    bool usable = true;
};

// Limit only the optimization's numeric domain. At this scale the absolute
// guard below also covers flushing float subnormals through both shader dots.
inline constexpr double coefficient_limit = 1e12;

inline Frustum frustum(const float (&view_projection)[16]) {
    Frustum result{};
    for (const float coefficient : view_projection) {
        if (!std::isfinite(coefficient) || std::abs(coefficient) > coefficient_limit) {
            result.usable = false;
            return result;
        }
    }
    // Vulkan clip planes are w ± x, w ± y, z and w - z.
    constexpr std::size_t axis[] = {0, 0, 1, 1, 2, 2};
    constexpr double sign[] = {1, -1, 1, -1, 1, -1};
    for (std::size_t side = 0; side < result.planes.size(); ++side) {
        auto& plane = result.planes[side];
        for (std::size_t col = 0; col < 4; ++col) {
            const double w = side == 4 ? 0 : view_projection[4 * col + 3];
            const double axis_value = view_projection[4 * col + axis[side]];
            plane.equation[col] = w + sign[side] * axis_value;
            plane.clip_magnitudes[col] = std::abs(w) + std::abs(axis_value);
        }
    }
    return result;
}

inline LocalFrustum local_frustum(const float (&model)[3][4], const Frustum& frustum) {
    LocalFrustum result{};
    result.usable = frustum.usable;
    if (!result.usable) return result;
    std::array<Row, 3> model_magnitudes;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t col = 0; col < 4; ++col) {
            const double coefficient = model[row][col];
            if (!std::isfinite(coefficient) || std::abs(coefficient) > coefficient_limit) {
                result.usable = false;
                return result;
            }
            model_magnitudes[row][col] = std::abs(coefficient);
        }
    }
    for (std::size_t side = 0; side < result.planes.size(); ++side) {
        const auto& world = frustum.planes[side];
        auto& local = result.planes[side];
        local.equation[3] = world.equation[3];
        local.clip_magnitudes[3] = world.clip_magnitudes[3];
        for (std::size_t col = 0; col < 4; ++col) {
            for (std::size_t row = 0; row < 3; ++row) {
                local.equation[col] += world.equation[row] * model[row][col];
                local.clip_magnitudes[col] += world.clip_magnitudes[row] * model_magnitudes[row][col];
            }
        }
    }
    return result;
}

inline bool intersects(const Bounds& local, const LocalFrustum& frustum) {
    if (!frustum.usable) return true;
    Row absolute_position{0, 0, 0, 1};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double low = local.minimum[axis], high = local.maximum[axis];
        if (!std::isfinite(low) || !std::isfinite(high) || low > high) return true;
        absolute_position[axis] = std::max(std::abs(low), std::abs(high));
        if (absolute_position[axis] > coefficient_limit) return true;
    }

    bool separated = false;
    constexpr double guard = 64 * std::numeric_limits<float>::epsilon();
    for (const auto& plane : frustum.planes) {
        double support = plane.equation[3], magnitude = plane.clip_magnitudes[3];
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const double coefficient = plane.equation[axis];
            support += coefficient * (coefficient >= 0 ? local.maximum[axis] : local.minimum[axis]);
            magnitude += plane.clip_magnitudes[axis] * absolute_position[axis];
        }
        // The unsimplified magnitudes enclose float rounding in the model
        // and clip dots, even when two clip coordinates almost cancel.
        separated |= support < -guard * (magnitude + 1);
    }
    return !separated;
}

inline bool intersects(const Bounds& local, const float (&model)[3][4], const Frustum& frustum) {
    return intersects(local, local_frustum(model, frustum));
}

} // namespace poima::render_shadow_bounds
