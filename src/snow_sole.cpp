// SPDX-License-Identifier: Apache-2.0
#include "poima/snow_sole.hpp"
#include "poima/snow.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace poima::snow {
namespace {

constexpr std::int64_t q14 = 1 << 14;
constexpr std::int64_t q20 = 1 << 20;
constexpr std::int64_t q12 = sole_weight_scale;

// Nearest integer to 16384 * sin(k * pi / 128) for k in [0, 64].
// Reflect magnitude and sign for the other quadrants. Both axes share this table.
constexpr std::array<std::int16_t, 65> quarter_sine_q14{
    0, 402, 804, 1205, 1606, 2006, 2404, 2801,
    3196, 3590, 3981, 4370, 4756, 5139, 5520, 5897,
    6270, 6639, 7005, 7366, 7723, 8076, 8423, 8765,
    9102, 9434, 9760, 10080, 10394, 10702, 11003, 11297,
    11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395,
    13623, 13842, 14053, 14256, 14449, 14635, 14811, 14978,
    15137, 15286, 15426, 15557, 15679, 15791, 15893, 15986,
    16069, 16143, 16207, 16261, 16305, 16340, 16364, 16379,
    16384,
};

constexpr std::int64_t sine_q14(unsigned heading) noexcept {
    const auto offset = heading % 64;
    const auto index = (heading & 64) != 0 ? 64 - offset : offset;
    const auto magnitude = quarter_sine_q14[index];
    return (heading & 128) != 0 ? -magnitude : magnitude;
}

// All callers bound the numerator before arithmetic. Round halves away from
// zero, including negative projections, without a signed right shift.
constexpr std::int64_t rounded_divide(std::int64_t numerator, std::int64_t denominator) noexcept {
    return numerator < 0 ? -((-numerator + denominator / 2) / denominator) :
        (numerator + denominator / 2) / denominator;
}

constexpr std::int64_t lobe_weight(std::int64_t along, std::int64_t across,
    std::int64_t half_length, std::int64_t half_width) noexcept {
    if (along <= -half_length || along >= half_length || across <= -half_width || across >= half_width) return 0;
    const auto length_penalty = rounded_divide(q12 * along * along, half_length * half_length);
    const auto width_penalty = rounded_divide(q12 * across * across, half_width * half_width);
    return std::max<std::int64_t>(0, q12 - length_penalty - width_penalty);
}

} // namespace

std::optional<std::uint8_t> quantize_sole_heading(const std::array<double, 3>& local_forward) noexcept {
    // The source is a unit planted axis. Allow the same per-axis slack as
    // grounding's saved frame validation and reject a vanishing X/Z plane.
    for (const auto axis : local_forward)
        if (!std::isfinite(axis) || std::abs(axis) > 1.000001) return std::nullopt;
    const auto magnitude = std::hypot(local_forward[0], local_forward[2]);
    if (magnitude <= 1e-12) return std::nullopt;
    const auto x = static_cast<std::int64_t>(std::llround((local_forward[0] / magnitude) * q20));
    const auto z = static_cast<std::int64_t>(std::llround((local_forward[2] / magnitude) * q20));

    auto best_dot = std::numeric_limits<std::int64_t>::min();
    std::uint8_t best = 0;
    for (unsigned bin = 0; bin < 256; ++bin) {
        const auto dot = x * sine_q14(bin) + z * sine_q14(bin + 64);
        // Keep the first (lowest) bin when two signed dots are equal.
        if (dot > best_dot) {
            best = static_cast<std::uint8_t>(bin);
            best_dot = dot;
        }
    }
    return best;
}

std::uint32_t sole_sample_weight(std::int64_t dx, std::int64_t dz,
    const SoleShape& sole, std::uint8_t heading) noexcept {
    const std::int64_t length = sole.half_length_units, width = sole.half_width_units;
    // The authored bound covers both rotated dimensions and two units of
    // rounding slack. Check signed offsets directly, even at INT64_MIN.
    const auto extent = length + width + sole_radius_rounding_units;
    if (length < min_sole_half_length_units || length > max_sole_half_length_units ||
        width < min_sole_half_width_units || width > max_sole_half_width_units || extent > max_radius_units ||
        dx < -extent || dx > extent || dz < -extent || dz > extent) return 0;

    const auto forward_x = sine_q14(heading), forward_z = sine_q14(heading + 64);
    const auto along = rounded_divide(dx * forward_x + dz * forward_z, q14);
    const auto across = rounded_divide(dx * forward_z - dz * forward_x, q14);
    const auto lobe_center = rounded_divide(38 * length, 100);
    const auto lobe_length = rounded_divide(62 * length, 100);
    const auto heel_width = rounded_divide(74 * width, 100);

    const auto heel = lobe_weight(along + lobe_center, across, lobe_length, heel_width);
    const auto toe = lobe_weight(along - lobe_center, across, lobe_length, width);
    return static_cast<std::uint32_t>(std::max(heel, toe));
}

} // namespace poima::snow
